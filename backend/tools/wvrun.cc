/* wvrun.cc — minimal webview.h host, isolates desktop.cc from the equation.
 * Builds a webview::webview, navigates to a data: URL, pumps for N seconds.
 * Env: WVRUN_URL (navigate target; default = inline data: page)
 *      WVRUN_SECS (pump seconds, default 20)
 */
#include <windows.h>
#include <cstdio>
#include <string>
#include <thread>
#include "webview.h"

/* options object, toggled by WVRUN_OPT=1 */
class RunOptions : public ICoreWebView2EnvironmentOptions {
    LONG ref_ = 1;
    std::wstring args_;
public:
    explicit RunOptions(const std::wstring &args) : args_(args) {}
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
        fprintf(stderr, "[OPT] get_AdditionalBrowserArguments called\n");
        *v = (LPWSTR)CoTaskMemAlloc((args_.size() + 1) * sizeof(wchar_t));
        if (*v) wcscpy(*v, args_.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR v) override { args_ = v ? v : L""; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Language(LPWSTR *v) override {
        *v = (LPWSTR)CoTaskMemAlloc(6 * sizeof(wchar_t));
        if (*v) wcscpy(*v, L"zh-CN");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR *v) override {
        *v = (LPWSTR)CoTaskMemAlloc(14 * sizeof(wchar_t));
        if (*v) wcscpy(*v, L"120.0.0.0");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL *v) override { if (v) *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return S_OK; }
};

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

static const char kHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
    "body{background:#101418;color:#9fb3c8;font:16px sans-serif;display:flex;"
    "align-items:center;justify-content:center;height:100vh;margin:0}</style></head>"
    "<body><div>WVRUN STARTING PAGE</div></body></html>";

int main(int argc, char **argv) {
    /* TEST: webview.h never calls CoInitializeEx (app's job). Try STA first. */
    HRESULT comhr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    fprintf(stderr, "[W0b] CoInitializeEx hr=0x%08lX\n", (unsigned long)comhr);
    int secs = (argc >= 2) ? atoi(argv[1]) : 20;
    const char *url_env = getenv("WVRUN_URL");
    std::string url = url_env && url_env[0] ? url_env : data_url(kHtml);
    printf("wvrun: secs=%d url=%.60s\n", secs, url.c_str());
    fflush(stdout);

    RunOptions *opts = new RunOptions(
        L"--disable-features=msSmartScreenProtection,"
        L"SmartScreen,DeviceBoundSessions,LocalNetworkAccess");
    const char *opt_env = getenv("WVRUN_OPT");
    if (opt_env && opt_env[0] == '1') {
        webview::g_webview2_env_options = opts;
        fprintf(stderr, "[W0] options object INSTALLED\n");
    } else {
        fprintf(stderr, "[W0] options NOT set (nullptr)\n");
    }

    webview::webview w(false, nullptr);
    fprintf(stderr, "[W1] constructed\n");
    w.set_title("wvrun");
    w.set_size(1280, 800, WEBVIEW_HINT_NONE);
    fprintf(stderr, "[W2] sized\n");
    w.navigate(url);
    fprintf(stderr, "[W3] navigate called\n");

    /* watchdog: end the pump after N seconds */
    std::thread wd([&w, secs] {
        Sleep((DWORD)secs * 1000);
        w.dispatch([&w] { w.terminate(); });
    });

    w.run();
    wd.join();
    printf("wvrun: pump exited after timeout\n");
    return 0;
}
