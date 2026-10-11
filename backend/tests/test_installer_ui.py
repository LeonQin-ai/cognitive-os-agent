"""Verify the built Inno wizard on an isolated desktop; cancel before installation."""
import ctypes as C
from ctypes import wintypes as W
from array import array
from pathlib import Path
import time
import os

ROOT = Path(__file__).resolve().parents[1]
user = C.WinDLL('user32', use_last_error=True)
kernel = C.WinDLL('kernel32', use_last_error=True)
user.CreateDesktopW.argtypes = [W.LPCWSTR, W.LPCWSTR, C.c_void_p, W.DWORD, W.DWORD, C.c_void_p]
user.CreateDesktopW.restype = W.HANDLE
user.SetThreadDesktop.argtypes = [W.HANDLE]
user.GetThreadDesktop.argtypes = [W.DWORD]
user.GetThreadDesktop.restype = W.HANDLE
user.CloseDesktop.argtypes = [W.HANDLE]
user.PrintWindow.argtypes = [W.HWND, W.HDC, W.UINT]
user.SetProcessDpiAwarenessContext.argtypes = [W.HANDLE]
user.SetProcessDpiAwarenessContext(W.HANDLE(-4))
user.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
user.SendMessageW.restype = W.LPARAM
kernel.GetCurrentThreadId.restype = W.DWORD

class Startup(C.Structure):
    _fields_ = [('cb', W.DWORD), ('lpReserved', W.LPWSTR), ('lpDesktop', W.LPWSTR),
                ('lpTitle', W.LPWSTR), ('dwX', W.DWORD), ('dwY', W.DWORD),
                ('dwXSize', W.DWORD), ('dwYSize', W.DWORD), ('dwXCountChars', W.DWORD),
                ('dwYCountChars', W.DWORD), ('dwFillAttribute', W.DWORD),
                ('dwFlags', W.DWORD), ('wShowWindow', W.WORD), ('cbReserved2', W.WORD),
                ('lpReserved2', C.c_void_p), ('hStdInput', W.HANDLE),
                ('hStdOutput', W.HANDLE), ('hStdError', W.HANDLE)]

class Process(C.Structure):
    _fields_ = [('hProcess', W.HANDLE), ('hThread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]

kernel.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p,
                                 W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR,
                                 C.POINTER(Startup), C.POINTER(Process)]
kernel.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
kernel.CloseHandle.argtypes = [W.HANDLE]
kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
kernel.OpenProcess.restype = W.HANDLE

def windows():
    result = []
    win32gui.EnumWindows(lambda h, _: result.append(h), None)
    return result

def children(window):
    result = []
    win32gui.EnumChildWindows(window, lambda h, _: result.append(h), None)
    return result

def control_text(window):
    length = win32gui.SendMessage(window, win32con.WM_GETTEXTLENGTH, 0, 0)
    buffer = array('H', [0] * (length + 1))
    win32gui.SendMessage(window, win32con.WM_GETTEXT, length + 1, buffer)
    return bytes(buffer).decode('utf-16-le').rstrip('\0')

def wait_for(fn):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(.1)
    details = [(win32gui.GetClassName(h), win32gui.GetWindowText(h),
                [(win32gui.GetClassName(c), win32gui.GetWindowText(c)) for c in children(h)
                 if win32gui.IsWindowVisible(c)]) for h in windows() if win32gui.IsWindowVisible(h)]
    raise AssertionError(f'Installer UI did not reach the expected state: {details!r}')

def screenshot(window, name):
    left, top, right, bottom = win32gui.GetWindowRect(window)
    dc = win32ui.CreateDCFromHandle(win32gui.GetWindowDC(window))
    target = dc.CreateCompatibleDC()
    bitmap = win32ui.CreateBitmap()
    bitmap.CreateCompatibleBitmap(dc, right-left, bottom-top)
    target.SelectObject(bitmap)
    assert user.PrintWindow(window, target.GetSafeHdc(), 0)
    bitmap.SaveBitmapFile(target, str(ROOT / 'build' / name))
    win32gui.DeleteObject(bitmap.GetHandle())
    target.DeleteDC()
    handle = dc.GetSafeHdc()
    dc.DeleteDC()
    win32gui.ReleaseDC(window, handle)

original = user.GetThreadDesktop(kernel.GetCurrentThreadId())
desktop_name = f'CognitiveOSInstallerTest-{os.getpid()}'
desktop = user.CreateDesktopW(desktop_name, None, None, 0, 0x10000000, None)
assert desktop and user.SetThreadDesktop(desktop), C.get_last_error()
# MFC initialization creates a hidden window, so import it after switching desktops.
import win32con
import win32gui
import win32ui
import win32process
process = Process()
wizard_process = None
try:
    exe = ROOT / 'dist' / 'cognitive-os-agent-setup.exe'
    # Spaces and Unicode test the actual directory edit and ready-page summary.
    destination = str(ROOT / 'build' / '安装路径验证 with spaces')
    startup = Startup()
    startup.cb = C.sizeof(startup)
    startup.lpDesktop = desktop_name
    command = C.create_unicode_buffer(f'"{exe}" /LANG=english /DIR="{destination}" /NORESTART')
    assert kernel.CreateProcessW(str(exe), command, None, None, False, 0, None,
                                 str(ROOT), C.byref(startup), C.byref(process))
    wizard = wait_for(lambda: next((h for h in windows() if win32gui.GetClassName(h) == 'TWizardForm'), None))
    wizard_process = kernel.OpenProcess(1, False, win32process.GetWindowThreadProcessId(wizard)[1])
    directory = wait_for(lambda: next((h for h in children(wizard)
        if win32gui.IsWindowVisible(h) and 'Edit' in win32gui.GetClassName(h)
        and win32gui.GetWindowText(h) == destination), None))
    assert win32gui.GetWindowText(directory) == destination
    browse = [h for h in children(wizard) if win32gui.IsWindowVisible(h)
              and 'browse' in win32gui.GetWindowText(h).lower()]
    assert browse, 'Directory page must offer Browse'
    screenshot(wizard, 'installer-directory.bmp')
    chosen = destination + ' changed'
    value = C.create_unicode_buffer(chosen)
    C.set_last_error(0)
    result = user.SendMessageW(directory, win32con.WM_SETTEXT, 0, C.addressof(value))
    assert control_text(directory) == chosen, (result, C.get_last_error(), control_text(directory))
    next_button = next(h for h in children(wizard) if win32gui.IsWindowVisible(h)
                       and win32gui.GetWindowText(h).replace('&', '').startswith('Next'))
    user.SendMessageW(next_button, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, (10 << 16) | 10)
    user.SendMessageW(next_button, win32con.WM_LBUTTONUP, 0, (10 << 16) | 10)
    wait_for(lambda: next((h for h in children(wizard) if win32gui.IsWindowVisible(h)
                          and win32gui.GetWindowText(h).replace('&', '') == 'Install'), None))
    ready = wait_for(lambda: next((h for h in children(wizard)
        if win32gui.IsWindowVisible(h) and 'Edit' not in win32gui.GetClassName(h)
        and chosen in control_text(h)), None))
    assert chosen in control_text(ready)
    screenshot(wizard, 'installer-ready.bmp')
    assert not Path(chosen).exists(), 'Wizard test must not install files'
    print('PASS: installer directory page, Browse, Unicode/space path editing and ready-page path confirmation.')
finally:
    # Only the dedicated test process is terminated; no installation is performed.
    if wizard_process:
        kernel.TerminateProcess(wizard_process, 0)
        kernel.CloseHandle(wizard_process)
    if process.hProcess:
        kernel.TerminateProcess(process.hProcess, 0)
        kernel.CloseHandle(process.hProcess)
        kernel.CloseHandle(process.hThread)
    # Terminate any bootstrap child still running on this dedicated test desktop.
    for window in windows():
        try:
            if win32gui.IsWindow(window) and win32gui.GetClassName(window) == 'TWizardForm':
                handle = kernel.OpenProcess(1, False, win32process.GetWindowThreadProcessId(window)[1])
                if handle:
                    kernel.TerminateProcess(handle, 0)
                    kernel.CloseHandle(handle)
        except Exception:
            pass
    user.SetThreadDesktop(original)
    user.CloseDesktop(desktop)
