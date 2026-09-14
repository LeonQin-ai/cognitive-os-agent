// desktop.cc — native WebView2 shell for the cognitive-os-agent desktop edition.
//
// The installed app is a single window (no console, no separate browser). On
// launch it:
//   1. spawns  cognitive-os-agent.exe serve 18300  (hidden, output to cognitive-os-agent-server.log)
//   2. opens the window IMMEDIATELY on an inline "starting" page — the boot
//      can legitimately take several seconds (restored MCP servers pay their
//      handshake timeouts before the HTTP listener comes up), and a blank
//      window used to read as "the app froze"
//   3. a worker thread polls for the HTTP server; once up the window
//      navigates to http://localhost:18300/, on timeout it shows an
//      in-window error page with recovery instructions
//   4. terminates the server when the window is closed
//
// Host implementation: raw WebView2 COM (the same pattern proven by the
// wv2test diagnostic host). The vendored webview.h (third_party/webview,
// v0.10.0) was dropped: on WebView2 runtime 153 its environment/controller
// wiring reliably wedges ALL main-frame navigation (window paints, renderer
// and JS evaluate work, but Page.navigate never starts) — reproduced with a
// minimal webview.h host and exhaustively bisected against the working raw
// COM host without finding a single-call difference. The raw COM host
// navigates in under 2 seconds every time.
//
// Build (package.sh does this). --subsystem=windows links the shell as a GUI
// binary so double-clicking it never pops a console window:
//   zig c++ -std=c++17 -O1 -I third_party/webview -o cognitive-os-agent-desktop.exe tools/desktop.cc \
//     -Wl,--subsystem=windows -lole32 -loleaut32 -luuid -lshlwapi -luser32 -lgdi32 -lws2_32 -lshell32 -ladvapi32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shlwapi.h>
#include <unknwn.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "WebView2.h"

static const int kPort = 18300;
static const UINT WM_APP_NAV = WM_APP + 1; /* wParam 1 = console, 2 = error page */

static HWND g_hwnd = nullptr;
static ICoreWebView2 *g_web = nullptr;
static ICoreWebView2Controller *g_ctrl = nullptr;
static HANDLE g_server = nullptr;
static std::wstring g_url_main;
static std::wstring g_url_error;

/* ---- ICoreWebView2EnvironmentOptions: pass --disable-features to the
 * browser process.  The WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS env var is
 * honored by the runtime too, but the options object is the supported
 * channel; both are set (the env var preserves any user-provided value). */
class DisableFeaturesOptions : public ICoreWebView2EnvironmentOptions {
    LONG ref_ = 1;
    std::wstring args_;
public:
    explicit DisableFeaturesOptions(const std::wstring &args) : args_(args) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2EnvironmentOptions)
            *ppv = static_cast<ICoreWebView2EnvironmentOptions *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR *v) override {
        if (!v) return E_POINTER;
        *v = (LPWSTR)CoTaskMemAlloc((args_.size() + 1) * sizeof(wchar_t));
        if (*v) wcscpy(*v, args_.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR v) override { args_ = v ? v : L""; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *v) override {
        if (!v) return E_POINTER;
        *v = (LPWSTR)CoTaskMemAlloc(6 * sizeof(wchar_t));
        if (*v) wcscpy(*v, L"zh-CN");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR *v) override {
        if (!v) return E_POINTER;
        *v = (LPWSTR)CoTaskMemAlloc(14 * sizeof(wchar_t));
        if (*v) wcscpy(*v, L"120.0.0.0");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *v) override { if (v) *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return S_OK; }
};

/* ---- completion handlers ---- */
class EnvHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    LONG ref_ = 1;
public:
    bool done = false;
    HRESULT hr = E_FAIL;
    ICoreWebView2Environment *env = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment *environment) override {
        hr = errorCode;
        env = environment;
        if (env) env->AddRef();
        done = true;
        return S_OK;
    }
};

class CtrlHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    LONG ref_ = 1;
public:
    bool done = false;
    HRESULT hr = E_FAIL;
    ICoreWebView2Controller *ctrl = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller *controller) override {
        hr = errorCode;
        ctrl = controller;
        if (ctrl) ctrl->AddRef();
        done = true;
        return S_OK;
    }
};

/* Load the runtime's own loader DLL (installed evergreen runtime). */
using CreateWebViewEnvironmentWithOptionsInternal_t =
    HRESULT(STDMETHODCALLTYPE *)(bool, int, PCWSTR, IUnknown *,
                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static HMODULE g_runtime_dll = nullptr;
static CreateWebViewEnvironmentWithOptionsInternal_t g_create_env = nullptr;

static bool load_runtime() {
    char ver[64] = {0};
    DWORD sz = sizeof(ver);
    HKEY key;
    if (RegOpenKeyA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients"
                    "\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
                    &key) == ERROR_SUCCESS) {
        if (RegQueryValueExA(key, "pv", nullptr, nullptr, (BYTE *)ver, &sz) != ERROR_SUCCESS)
            ver[0] = 0;
        RegCloseKey(key);
    }
    char path[MAX_PATH] = {0};
    if (ver[0]) {
        snprintf(path, sizeof(path),
                 "C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\%s\\EBWebView\\x64\\EmbeddedBrowserWebView.dll", ver);
        g_runtime_dll = LoadLibraryA(path);
    }
    if (!g_runtime_dll) {
        /* fallback: scan the Application dir for any version */
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA("C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\*.*", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && fd.cFileName[0] >= '0' && fd.cFileName[0] <= '9') {
                    snprintf(path, sizeof(path),
                             "C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\%s\\EBWebView\\x64\\EmbeddedBrowserWebView.dll", fd.cFileName);
                    g_runtime_dll = LoadLibraryA(path);
                    if (g_runtime_dll) break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (!g_runtime_dll) return false;
    g_create_env = (CreateWebViewEnvironmentWithOptionsInternal_t)(void *)GetProcAddress(
        g_runtime_dll, "CreateWebViewEnvironmentWithOptionsInternal");
    return g_create_env != nullptr;
}

/* Pump messages until *flag turns true (or timeout_ms elapses). */
static bool pump_until(bool *flag, int timeout_ms) {
    DWORD start = GetTickCount();
    MSG msg;
    while (!*flag) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (*flag) break;
        if (GetTickCount() - start > (DWORD)timeout_ms) return false;
        WaitMessage();
    }
    return true;
}

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

/* Poll the local HTTP server until it accepts a connection or we time out. */
static bool wait_server(int port, int timeout_ms) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    ULONGLONG start = GetTickCount64();
    bool up = false;
    while (GetTickCount64() - start < (ULONGLONG)timeout_ms) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            if (connect(s, (sockaddr *)&addr, sizeof(addr)) == 0) {
                up = true;
            }
            closesocket(s);
        }
        if (up) break;
        Sleep(200);
    }
    WSACleanup();
    return up;
}

/* Inline HTML pages shown while the backend boots / if it never comes up. */
static const char kStartingHtml[] = R"(<!doctype html><html><head><meta charset="utf-8"><style>
body{display:flex;align-items:center;justify-content:center;height:100vh;margin:0;
background:#101418;color:#9fb3c8;font:15px/1.6 system-ui,sans-serif;user-select:none}
.s{width:40px;height:40px;border:4px solid #2a3642;border-top-color:#4da3ff;border-radius:50%;
animation:r 1s linear infinite;margin-right:18px;flex:none}
@keyframes r{to{transform:rotate(360deg)}}
</style></head><body><div class="s"></div><div>正在启动 cognitive-os-agent…</div></body></html>)";

static const char kErrorHtml[] = R"(<!doctype html><html><head><meta charset="utf-8"><style>
body{display:flex;align-items:center;justify-content:center;height:100vh;margin:0;
background:#101418;color:#c9d4de;font:15px/1.8 system-ui,sans-serif}
.b{max-width:640px;padding:0 32px}
h1{font-size:18px;color:#e8eef4}code{background:#1c242c;padding:2px 6px;border-radius:4px}
</style></head><body><div class="b">
<h1>服务未能按时启动</h1>
<p>如果已有一个 cognitive-os-agent 窗口在运行，请先关闭它（端口 18300
只能被一个实例占用），然后重新打开本程序。</p>
<p>详细原因见安装目录下的 <code>cognitive-os-agent-server.log</code>。</p>
</div></body></html>)";

/* data: URL helper — percent-encode the UTF-8 payload so no character in the
 * inline pages can break URL parsing. */
static std::string data_url(const char *html) {
    static const char *hex = "0123456789ABCDEF";
    std::string out = "data:text/html;charset=utf-8,";
    for (const unsigned char *p = (const unsigned char *)html; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static void navigate_main() {
    if (g_web) g_web->Navigate(g_url_main.c_str());
}

static void navigate_error() {
    if (g_web) g_web->Navigate(g_url_error.c_str());
}

/* Spawn the hidden backend server, output redirected to the log file.
 * Returns process handle (caller closes) or NULL on failure. */
static HANDLE spawn_server(const wchar_t *exe, const wchar_t *dir) {
    std::wstring log = std::wstring(dir) + L"\\cognitive-os-agent-server.log";

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE logf = CreateFileW(log.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    std::memset(&si, 0, sizeof(si));
    std::memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = logf;
    si.hStdError = logf;

    std::wstring cmdline = L"\"" + std::wstring(exe) + L"\" serve " +
                           std::to_wstring(kPort);
    wchar_t *cmd = new wchar_t[cmdline.size() + 1];
    wcscpy(cmd, cmdline.c_str());

    BOOL ok = CreateProcessW(exe, cmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, dir, &si, &pi);
    delete[] cmd;
    if (logf) CloseHandle(logf);
    if (!ok)
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* Worker thread: wait for the backend; if it dies while booting (it has a
 * known intermittent crash), restart it a few times before giving up. Once
 * up, keep watching — a crash mid-session is recovered the same way (server
 * respawn + page reload) instead of leaving a dead white window. Navigation
 * is marshalled to the UI thread via PostMessage(WM_APP_NAV). */
static DWORD WINAPI boot_waiter(LPVOID) {
    int restarts = 0;
    bool ever_up = false;

    for (;;) {
        bool up = wait_server(kPort, 60000);
        if (up) {
            if (!ever_up)
                PostMessageA(g_hwnd, WM_APP_NAV, 1, 0);
            ever_up = true;
        } else {
            if (restarts >= 3) {
                PostMessageA(g_hwnd, WM_APP_NAV, 2, 0);
                return 0;
            }
            restarts++;
            if (g_server) {
                TerminateProcess(g_server, 0);
                CloseHandle(g_server);
            }
            std::wstring dir = exe_dir();
            std::wstring exe = dir + L"\\cognitive-os-agent.exe";
            g_server = spawn_server(exe.c_str(), dir.c_str());
            if (!g_server)
                return 0;
            continue;
        }

        /* Steady state: watch the server process; recover from crashes. */
        ULONGLONG healthy_since = GetTickCount64();
        int probes = 0;
        for (;;) {
            DWORD wait = WaitForSingleObject(g_server, 2000);
            if (wait == WAIT_OBJECT_0) {
                /* server process exited (crash or clean stop). If the window
                 * is gone too, exit; otherwise restart the backend. */
                if (!IsWindow(g_hwnd) || restarts >= 3)
                    return 0; /* window close terminates us anyway */
                restarts++;
                CloseHandle(g_server);
                std::wstring dir = exe_dir();
                std::wstring exe = dir + L"\\cognitive-os-agent.exe";
                g_server = spawn_server(exe.c_str(), dir.c_str());
                if (!g_server)
                    return 0;
                break; /* re-enter wait_server loop, then reload the page */
            }
            /* A hung-but-alive server is also recovered: probe every ~6s. */
            if (++probes % 3 == 0 && !wait_server(kPort, 1000)) {
                if (restarts >= 3)
                    return 0;
                restarts++;
                TerminateProcess(g_server, 0);
                CloseHandle(g_server);
                std::wstring dir = exe_dir();
                std::wstring exe = dir + L"\\cognitive-os-agent.exe";
                g_server = spawn_server(exe.c_str(), dir.c_str());
                if (!g_server)
                    return 0;
                break;
            }
            /* Five stable minutes restore the full crash budget. */
            if (GetTickCount64() - healthy_since > 300000)
                restarts = 0;
        }
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_NAV:
        if (wp == 1)
            navigate_main();
        else if (wp == 2)
            navigate_error();
        return 0;
    case WM_SIZE: {
        if (g_ctrl && wp != SIZE_MINIMIZED) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_ctrl->put_Bounds(rc);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::wstring dir = exe_dir();
    std::wstring exe = dir + L"\\cognitive-os-agent.exe";

    /* Work around two Edge/WebView2 behaviors that stall navigations on
     * networks where Microsoft's backends are slow or unreachable:
     *  - SmartScreen / device-bound-session checks pause every main-frame
     *    load at URL_REQUEST_DELEGATE_CONNECTED (observed in netlog) and the
     *    pause never resumes when the check cannot complete;
     *  - Local Network Access gating similarly parks requests to localhost.
     * Preserve any user-provided value of the environment variable. */
    {
        wchar_t existing[1024];
        DWORD n = GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                                          existing, 1024);
        std::wstring args = L"--disable-features=msSmartScreenProtection,"
                            L"SmartScreen,DeviceBoundSessions,LocalNetworkAccess";
        if (n > 0 && n < 1024)
            args = std::wstring(existing) + L" " + args;
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args.c_str());
    }

    /* Show the window immediately with the inline starting page — never a
     * dead silent desktop. The worker thread switches over to the console
     * once the backend accepts connections (or renders the error page). */
    WNDCLASSA wc;
    std::memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512));
    wc.lpszClassName = "cognitive-os-agent";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("cognitive-os-agent", "cognitive-os-agent",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           1280, 800, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Failed to create the main window.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    g_url_main = L"http://localhost:" + std::to_wstring(kPort) + L"/";
    {
        std::string err8 = data_url(kErrorHtml);
        g_url_error = std::wstring(err8.begin(), err8.end());
    }

    /* Backend first (so its boot overlaps the WebView2 env creation), then
     * the raw COM host — the sequence proven by the wv2test diagnostic tool:
     * window shown → COM STA → env (pump) → controller (pump) → navigate. */
    g_server = spawn_server(exe.c_str(), dir.c_str());
    if (!g_server) {
        MessageBoxW(nullptr, L"Failed to start the cognitive-os-agent server.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"COM initialization failed.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }

    if (!load_runtime()) {
        MessageBoxW(nullptr,
                    L"未找到 WebView2 运行时。\n请安装 Microsoft Edge WebView2 Runtime 后重试。",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }

    DisableFeaturesOptions opts(
        L"--disable-features=msSmartScreenProtection,"
        L"SmartScreen,DeviceBoundSessions,LocalNetworkAccess");
    /* UDF: %APPDATA%\<exe name>, same location the previous builds used */
    wchar_t udf[MAX_PATH];
    {
        char appdata[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::string u = std::string(appdata) + "\\cognitive-os-agent-desktop.exe";
            MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, udf, MAX_PATH);
        } else {
            wcscpy(udf, dir.c_str());
        }
    }

    EnvHandler *eh = new EnvHandler();
    hr = g_create_env(true, 0 /*installed*/, udf, &opts, eh);
    if (FAILED(hr) || !pump_until(&eh->done, 60000) || FAILED(eh->hr) || !eh->env) {
        MessageBoxW(nullptr, L"WebView2 environment creation failed.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }

    CtrlHandler *ch = new CtrlHandler();
    hr = eh->env->CreateCoreWebView2Controller(g_hwnd, ch);
    if (FAILED(hr) || !pump_until(&ch->done, 60000) || FAILED(ch->hr) || !ch->ctrl) {
        MessageBoxW(nullptr, L"WebView2 controller creation failed.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }

    g_ctrl = ch->ctrl;
    g_ctrl->AddRef();
    HRESULT hrg = ch->ctrl->get_CoreWebView2(&g_web);
    if (FAILED(hrg) || !g_web) {
        MessageBoxW(nullptr, L"WebView2 object unavailable.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }

    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_ctrl->put_Bounds(rc);

    /* Starting page first, then the boot watcher takes over. */
    {
        std::string start8 = data_url(kStartingHtml);
        std::wstring wstart(start8.begin(), start8.end());
        g_web->Navigate(wstart.c_str());
    }

    HANDLE boot_thread = CreateThread(nullptr, 0, boot_waiter, nullptr, 0, nullptr);
    if (boot_thread)
        CloseHandle(boot_thread); /* joined implicitly by process exit */

    /* UI message loop */
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        /* Forward keyboard accelerators (Ctrl+C/V/X/A/Z, F5, ...) to the
         * WebView2 controller. Without this the hosted browser never sees
         * them, so e.g. paste into the page's inputs silently does nothing. */
        if (g_ctrl && g_ctrl->TranslateAccelerator(&msg) == S_OK)
            continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (g_server) {
        TerminateProcess(g_server, 0);
        CloseHandle(g_server);
    }
    CoUninitialize();
    return 0;
}
