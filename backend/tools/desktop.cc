// desktop.cc — native WebView2 shell for the cognitive-os-agent desktop edition.
//
// The installed app is a single window (no console, no separate browser). On
// launch it:
//   1. spawns  cognitive-os-agent.exe serve 18300  (hidden, output to cognitive-os-agent-server.log)
//   2. waits for the HTTP server to accept connections on 127.0.0.1:18300
//   3. opens a native WebView2 window pointed at http://localhost:18300/
//   4. terminates the server when the window is closed
//
// The WebView2 window is provided by the vendored single-header webview.h
// (third_party/webview/webview.h, v0.10.0). Its built-in loader resolves the
// installed WebView2 runtime directly, so no WebView2Loader.dll is shipped.
//
// Build (package.sh does this). --subsystem=windows links the shell as a GUI
// binary so double-clicking it never pops a console window:
//   zig c++ -std=c++17 -O1 -I third_party/webview -o cognitive-os-agent-desktop.exe tools/desktop.cc \
//     -Wl,--subsystem=windows -lole32 -loleaut32 -luuid -lshlwapi -luser32 -lgdi32 -lws2_32 -lshell32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shlwapi.h>

#include <cstring>
#include <string>

#include "webview.h"

static const int kPort = 18300;

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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::wstring dir = exe_dir();
    std::wstring exe = dir + L"\\cognitive-os-agent.exe";
    std::wstring log = dir + L"\\cognitive-os-agent-server.log";

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

    std::wstring cmdline = L"\"" + exe + L"\" serve " + std::to_wstring(kPort);
    wchar_t *cmd = new wchar_t[cmdline.size() + 1];
    wcscpy(cmd, cmdline.c_str());

    BOOL ok = CreateProcessW(exe.c_str(), cmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    delete[] cmd;
    if (logf) CloseHandle(logf);
    if (!ok) {
        MessageBoxW(nullptr, L"Failed to start the cognitive-os-agent server.",
                    L"cognitive-os-agent", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);

    if (!wait_server(kPort, 10000)) {
        MessageBoxW(nullptr,
                    L"The cognitive-os-agent server did not start in time.\n\n"
                    L"See cognitive-os-agent-server.log in the install folder for details.",
                    L"cognitive-os-agent", MB_ICONWARNING);
    }

    webview::webview w(false, nullptr);
    w.set_title("cognitive-os-agent");
    w.set_size(1280, 800, WEBVIEW_HINT_NONE);
    w.navigate("http://localhost:" + std::to_string(kPort) + "/");
    w.run();

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    return 0;
}
