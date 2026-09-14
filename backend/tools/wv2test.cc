// wv2test.cc — minimal WebView2 diagnostic host.
// Usage: wv2test.exe <url> [timeout_sec] [extra browser args...]
// Prints every HRESULT and the NavigationCompleted error status so the full
// failure reason is visible (unlike truncated CDP errorText).
//
// Build:
//   zig c++ -std=c++17 -O1 -I third_party/webview -o build/wv2test.exe tools/wv2test.cc \
//     -lole32 -loleaut32 -luuid -luser32 -lgdi32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <cstdio>
#include <string>

#include "WebView2.h"

/* Load the runtime's own loader DLL the same way webview.h does. */
using CreateWebViewEnvironmentWithOptionsInternal_t =
    HRESULT(STDMETHODCALLTYPE *)(bool, int, PCWSTR, IUnknown *,
                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static HMODULE g_runtime_dll = nullptr;
static CreateWebViewEnvironmentWithOptionsInternal_t g_create_env = nullptr;

static bool load_runtime() {
    /* evergreen runtime: registry gives the version dir, we just scan */
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
    char path[MAX_PATH];
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
    g_create_env = (CreateWebViewEnvironmentWithOptionsInternal_t)(void *)GetProcAddress(g_runtime_dll, "CreateWebViewEnvironmentWithOptionsInternal");
    printf("runtime dll=%s create_fn=%p\n", path, (void *)g_create_env);
    return g_create_env != nullptr;
}

static void pump_until(bool *flag) {
    MSG msg;
    while (!*flag && GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ---- ICoreWebView2EnvironmentOptions implementation ---- */
class EnvOptions : public ICoreWebView2EnvironmentOptions {
    LONG ref_ = 1;
    std::wstring args_;
public:
    explicit EnvOptions(const std::wstring &args) : args_(args) {}
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
        *v = (LPWSTR)CoTaskMemAlloc((args_.size() + 1) * sizeof(wchar_t));
        wcscpy(*v, args_.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR v) override { args_ = v; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *v) override {
        const char *lang = getenv("WV2TEST_LANG");
        std::wstring l = lang && lang[0] ? std::wstring(lang, lang + strlen(lang)) : L"en-US";
        *v = (LPWSTR)CoTaskMemAlloc((l.size() + 1) * sizeof(wchar_t));
        wcscpy(*v, l.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR *v) override {
        const char *tgt = getenv("WV2TEST_TARGET");
        std::wstring t = tgt && tgt[0] ? std::wstring(tgt, tgt + strlen(tgt)) : L"94.0.992.31";
        *v = (LPWSTR)CoTaskMemAlloc((t.size() + 1) * sizeof(wchar_t));
        wcscpy(*v, t.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *v) override { *v = FALSE; return S_OK; }
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

class NavHandler : public ICoreWebView2NavigationCompletedEventHandler {
    LONG ref_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2NavigationCompletedEventHandler)
            *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler *>(this);
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
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) override {
        BOOL ok = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        args->get_IsSuccess(&ok);
        args->get_WebErrorStatus(&st);
        printf("NAV-COMPLETED ok=%d webErrorStatus=%d\n", (int)ok, (int)st);
        fflush(stdout);
        return S_OK;
    }
};

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: wv2test.exe <url> [timeout_sec] [browser args...]\n");
        return 2;
    }
    std::string url = argv[1];
    int timeout_sec = (argc >= 3) ? atoi(argv[2]) : 45;
    std::string extra;
    for (int i = 3; i < argc; i++) {
        if (i > 3) extra += " ";
        extra += argv[i];
    }
    printf("url=%s timeout=%ds extra=[%s]\n", url.c_str(), timeout_sec, extra.c_str());

    /* ---- webview.h mimic switches (env-gated, all default off) ---- */
    const char *dpi_env = getenv("WV2TEST_DPI");
    if (dpi_env && dpi_env[0] == '1') {
        /* webview.h enable_dpi_awareness(): per-monitor-v2 before window */
        using Fn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
        HMODULE u32 = GetModuleHandleA("user32.dll");
        Fn set_ctx = (Fn)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (set_ctx)
            printf("dpi ctx hr=%d\n", (int)set_ctx((DPI_AWARENESS_CONTEXT)-4));
    }

    const char *comfirst_env = getenv("WV2TEST_COMFIRST");
    if (comfirst_env && comfirst_env[0] == '1') {
        HRESULT chr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        printf("com-first hr=0x%08lX\n", (unsigned long)chr);
    }

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "wv2test";
    RegisterClassA(&wc);
    /* WV2TEST_ZERO=1 mimics webview.h: create window 0x0, resize to 640x480,
     * then show — the sequence webview.h uses before creating the controller. */
    const char *zero_env = getenv("WV2TEST_ZERO");
    HWND hwnd;
    if (zero_env && zero_env[0] == '1') {
        hwnd = CreateWindowA("wv2test", "wv2test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        RECT wr = {0, 0, 640, 480};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetFocus(hwnd);
        printf("zero-size creation path\n");
    } else {
        hwnd = CreateWindowA("wv2test", "wv2test", WS_OVERLAPPEDWINDOW, 100, 100,
                             900, 650, nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(hwnd, SW_SHOW);
    }

    if (!comfirst_env || comfirst_env[0] != '1') {
        HRESULT hrc = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        printf("CoInitializeEx hr=0x%08lX\n", (unsigned long)hrc);
    }
    HRESULT hr = S_OK;

    EnvOptions *opts = new EnvOptions(std::wstring(extra.begin(), extra.end()));
    /* WV2TEST_NULLOPT=1 mimics webview.h: pass nullptr options to the loader */
    const char *nullopt_env = getenv("WV2TEST_NULLOPT");
    if (nullopt_env && nullopt_env[0] == '1')
        opts = nullptr;
    if (!load_runtime()) {
        printf("FATAL: WebView2 runtime loader DLL not found\n");
        return 1;
    }
    wchar_t udf[MAX_PATH];
    const char *udf_env = getenv("WV2TEST_UDF");
    MultiByteToWideChar(CP_UTF8, 0, udf_env && udf_env[0] ? udf_env : "C:\\Users\\94207\\AppData\\Local\\Temp\\wv2test-udf", -1, udf, MAX_PATH);
    wprintf(L"udf=%s\n", udf);
    EnvHandler *eh = new EnvHandler();
    hr = g_create_env(true, 0 /*installed*/, udf, opts, eh);
    printf("CreateEnvironment hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    pump_until(&eh->done);
    printf("env done hr=0x%08lX env=%p\n", (unsigned long)eh->hr, (void *)eh->env);
    if (FAILED(eh->hr) || !eh->env) return 1;

    CtrlHandler *ch = new CtrlHandler();
    hr = eh->env->CreateCoreWebView2Controller(hwnd, ch);
    printf("CreateController hr=0x%08lX\n", (unsigned long)hr);
    pump_until(&ch->done);
    printf("ctrl done hr=0x%08lX ctrl=%p\n", (unsigned long)ch->hr, (void *)ch->ctrl);
    if (FAILED(ch->hr) || !ch->ctrl) return 1;

    ICoreWebView2 *web = nullptr;
    ch->ctrl->get_CoreWebView2(&web);
    printf("web=%p\n", (void *)web);
    if (!web) return 1;

    RECT rc;
    GetClientRect(hwnd, &rc);
    ch->ctrl->put_Bounds(rc);

    /* mimic webview.h: AddScriptToExecuteOnDocumentCreated + MoveFocus */
    const char *script_env = getenv("WV2TEST_SCRIPT");
    if (script_env && script_env[0] == '1') {
        EventRegistrationToken st;
        (void)st;
        HRESULT shr = web->AddScriptToExecuteOnDocumentCreated(
            L"window.external={invoke:s=>window.chrome.webview.postMessage(s)}", nullptr);
        printf("AddScript hr=0x%08lX\n", (unsigned long)shr);
    }
    const char *focus_env = getenv("WV2TEST_FOCUS");
    if (focus_env && focus_env[0] == '1') {
        HRESULT fhr = ch->ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        printf("MoveFocus hr=0x%08lX\n", (unsigned long)fhr);
    }

    NavHandler *nh = new NavHandler();
    EventRegistrationToken tok;
    web->add_NavigationCompleted(nh, &tok);

    // log navigation start too
    // (Navigate returns immediately; result comes via NAV-COMPLETED)
    {
        wchar_t wurl[8192];
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl, 8192);
        HRESULT nhr = web->Navigate(wurl);
        printf("Navigate hr=0x%08lX\n", (unsigned long)nhr);
    }

    DWORD start = GetTickCount();
    MSG msg;
    bool quit = false;
    while (!quit) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (GetTickCount() - start > (DWORD)timeout_sec * 1000) break;
        Sleep(50);
    }
    printf("done waiting\n");
    return 0;
}
