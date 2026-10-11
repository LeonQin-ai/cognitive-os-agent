# Windows title bar and installation folder

The desktop title bar uses native button controls with vector symbols, hover and
pressed feedback, keyboard focus and accessible Chinese names. Dimensions and
text scale with monitor DPI. Maximizing respects the monitor's work area;
double-clicking the title still maximizes/restores and the edges still resize.
`WM_NCCALCSIZE` removes the reserved system frame so the custom title bar and
WebView fill the window, including after maximizing. This fixes the black side
strips reported in [#95](https://github.com/LeonQin-ai/cognitive-os-agent/issues/95).

The Inno Setup wizard always shows the destination page, including upgrades.
Users can browse or type a writable folder on another drive. The previous
installation location is suggested when available; otherwise the default is
`%LOCALAPPDATA%\cognitive-os-agent`. The ready page shows the selected path.
This is a per-user installer, so choosing a protected system folder does not
automatically grant administrator permissions.

Tracking: [#93](https://github.com/LeonQin-ai/cognitive-os-agent/issues/93) and
[#94](https://github.com/LeonQin-ai/cognitive-os-agent/issues/94).

## Verification

Run from `backend` in PowerShell:

```powershell
& ./tools/zig/zig.exe c++ -std=c++17 -O1 -Wno-nullability-completeness -I third_party/webview -o build/test-desktop-chrome.exe tests/test_desktop_chrome.cc -lole32 -loleaut32 -luuid -lshlwapi -luser32 -lgdi32 -lws2_32 -lshell32 -lversion -ladvapi32
./build/test-desktop-chrome.exe
python tests/test_installer_ui.py
```

Build the installer first with `./package.sh` in Bash, or compile `desktop.cc`
with the same libraries and `-Wl,--subsystem=windows`, then run Inno Setup's
`ISCC.exe /Q tools/setup.iss`. The installer test requires Python with pywin32.

The native regression runs the actual window procedure and controls on an
isolated Windows desktop. Its 91 checks cover 100%, 150%, 200% scaling, button
geometry, dragging, minimum size, minimize/maximize/restore, work area limits,
mouse capture cancellation, hover and keyboard close. At each scale, both normal
and maximized client rectangles must exactly fill the outer window. Full-window
captures verify that the left and right title-bar edges have the expected color,
and maximized bounds must equal the monitor's work area. Before the black-border
fix, the new client-origin assertion reproduced the defect and failed.

The wizard test launches the actual built installer on another isolated desktop,
confirms the destination and Browse controls, edits a path containing Chinese
characters and spaces, advances to Ready to Install and verifies the exact path
in its summary. It stops before installing files or modifying installation
registration. It does not test a complete install/uninstall cycle.

Both tests save BMP screenshots under `build/chrome-*.bmp` and
`build/installer-*.bmp`. The desktop binary and installer were rebuilt successfully.
