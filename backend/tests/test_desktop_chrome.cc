/* Runs the actual desktop window procedure on an isolated Windows desktop. */
#include "../tools/desktop.cc"
#include <cassert>

static int checks = 0;
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); return 1; } ++checks; } while (0)

static void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
}

static bool snapshot(HWND hwnd, const char *path) {
    RECT rc; GetWindowRect(hwnd, &rc);
    int width = rc.right-rc.left, height = chrome_height();
    HDC screen = GetDC(hwnd), memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = SelectObject(memory, bitmap);
    // Capture the outer window too: client-only images hide non-client black strips.
    bool ok = PrintWindow(hwnd, memory, 0) != FALSE;
    COLORREF right_edge = g_hover_button == g_caption_buttons[2] ? RGB(196,43,28) : RGB(248,250,252);
    ok = ok && GetPixel(memory, 0, height/2) == RGB(248,250,252)
            && GetPixel(memory, width-1, height/2) == right_edge;
    BITMAPFILEHEADER header = {0x4D42, (DWORD)(sizeof(header)+sizeof(info.bmiHeader)+width*height*4),
                              0, 0, sizeof(header)+sizeof(info.bmiHeader)};
    FILE *file = fopen(path, "wb");
    if (file) {
        fwrite(&header, sizeof(header), 1, file);
        fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file);
        fwrite(pixels, width*height*4, 1, file); fclose(file);
    } else ok = false;
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(hwnd, screen);
    return ok;
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(L"CognitiveOSChromeTest", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    CHECK(desktop && SetThreadDesktop(desktop));
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CognitiveOSChromeTest";
    CHECK(RegisterClassW(&wc));
    HWND window = CreateWindowW(wc.lpszClassName, L"Chrome test",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
        100, 100, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(window);
    ShowWindow(window, SW_SHOWNOACTIVATE); pump();
    for (UINT dpi : {96u, 144u, 192u}) {
        RECT suggested = {100, 100, 1100, 800};
        SendMessageW(window, WM_DPICHANGED, MAKELONG(dpi, dpi), (LPARAM)&suggested); pump();
        CHECK(chrome_height() == (int)(40*dpi/96));
        RECT client; GetClientRect(window, &client);
        RECT outer; GetWindowRect(window, &outer);
        POINT origin = {0, 0}; ClientToScreen(window, &origin);
        CHECK(origin.x == outer.left && origin.y == outer.top);
        CHECK(client.right == outer.right-outer.left && client.bottom == outer.bottom-outer.top);
        CHECK(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(outer.left+1, outer.top+1)) == HTTOPLEFT);
        for (int i=0; i<3; ++i) {
            RECT button; GetWindowRect(g_caption_buttons[i], &button);
            MapWindowPoints(nullptr, window, (POINT *)&button, 2);
            CHECK(button.right-button.left == (int)(46*dpi/96));
            CHECK(button.bottom == chrome_height());
            CHECK(button.right == client.right-scaled(46)*(2-i));
        }
        POINT caption = {scaled(80), scaled(20)}; ClientToScreen(window, &caption);
        CHECK(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(caption.x, caption.y)) == HTCAPTION);
        MINMAXINFO sizes = {};
        SendMessageW(window, WM_GETMINMAXINFO, 0, (LPARAM)&sizes);
        CHECK(sizes.ptMinTrackSize.x == scaled(640));
        CHECK(sizes.ptMinTrackSize.y == scaled(480));
        char path[100]; snprintf(path, sizeof(path), "build/chrome-%u.bmp", dpi);
        CHECK(snapshot(window, path));
        SendMessageW(g_caption_buttons[1], BM_CLICK, 0, 0); pump();
        CHECK(IsZoomed(window));
        GetClientRect(window, &client); GetWindowRect(window, &outer);
        origin = {0, 0}; ClientToScreen(window, &origin);
        CHECK(origin.x == outer.left && origin.y == outer.top);
        CHECK(client.right == outer.right-outer.left && client.bottom == outer.bottom-outer.top);
        MONITORINFO work = {sizeof(work)};
        CHECK(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &work));
        CHECK(EqualRect(&outer, &work.rcWork));
        snprintf(path, sizeof(path), "build/chrome-maximized-%u.bmp", dpi);
        CHECK(snapshot(window, path));
        SendMessageW(g_caption_buttons[1], BM_CLICK, 0, 0); pump();
        CHECK(!IsZoomed(window));
    }
    HWND close = g_caption_buttons[2];
    SendMessageW(close, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    SendMessageW(close, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-20, -20));
    SendMessageW(close, WM_LBUTTONUP, 0, MAKELPARAM(-20, -20)); pump();
    CHECK(IsWindow(window)); // releasing outside must cancel the close operation
    SendMessageW(g_caption_buttons[0], BM_CLICK, 0, 0); pump(); CHECK(IsIconic(window));
    ShowWindow(window, SW_RESTORE); pump(); CHECK(!IsIconic(window));
    SendMessageW(g_caption_buttons[1], BM_CLICK, 0, 0); pump(); CHECK(IsZoomed(window));
    wchar_t label[32]; GetWindowTextW(g_caption_buttons[1], label, 32);
    CHECK(wcscmp(label, L"还原") == 0);
    CHECK(snapshot(window, "build/chrome-maximized.bmp"));
    MONITORINFO monitor = {sizeof(monitor)};
    CHECK(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor));
    RECT bounds; GetWindowRect(window, &bounds);
    CHECK(bounds.left == monitor.rcWork.left && bounds.top == monitor.rcWork.top);
    CHECK(bounds.right == monitor.rcWork.right && bounds.bottom == monitor.rcWork.bottom);
    SendMessageW(g_caption_buttons[1], BM_CLICK, 0, 0); pump(); CHECK(!IsZoomed(window));
    GetWindowTextW(g_caption_buttons[1], label, 32); CHECK(wcscmp(label, L"最大化") == 0);
    SendMessageW(close, WM_MOUSEMOVE, 0, MAKELPARAM(10,10)); CHECK(g_hover_button == close);
    CHECK(snapshot(window, "build/chrome-close-hover.bmp"));
    SendMessageW(close, WM_MOUSELEAVE, 0, 0); CHECK(g_hover_button == nullptr);
    SetFocus(close);
    SendMessageW(close, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(close, WM_KEYUP, VK_SPACE, 0); pump(); CHECK(!IsWindow(window));
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CHECK(SetThreadDesktop(original)); CloseDesktop(desktop);
    printf("PASS: %d native chrome checks (DPI, controls, capture, keyboard, work area).\n", checks);
    return 0;
}
