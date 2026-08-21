#!/usr/bin/env bash
# package.sh — one-click build + package the c-agent desktop installer.
#
#   1. compiles the release CLI binary (build/cagent[.exe])
#   2. compiles the native WebView2 desktop shell (build/c-agent-desktop.exe)
#   3. generates the application icon (dist/c-agent.ico)
#   4. builds a standard Inno Setup installer (dist/c-agent-setup.exe) with a
#      wizard, desktop + start-menu shortcuts, and an uninstaller
#
# The installed app is a single native window: c-agent-desktop.exe starts the
# backend (cagent.exe serve 8080) and opens the web console at
# http://localhost:8080/ — no console, no separate browser.
#
# Prerequisites: bundled zig (tools/zig or PATH), python, and Inno Setup
# (`winget install JRSoftware.InnoSetup`).
#
# Usage:
#   ./package.sh              build + produce dist/c-agent-setup.exe
#   ./package.sh clean        remove dist/ and the desktop shell
set -euo pipefail
cd "$(dirname "$0")"

ROOT="$(pwd)"
OUT="$ROOT/dist/c-agent-setup.exe"
DESKTOP="$ROOT/build/c-agent-desktop.exe"

if [ "${1:-}" = "clean" ]; then
    rm -rf "$ROOT/dist"
    rm -f  "$DESKTOP"
    echo "removed dist/ and $DESKTOP"
    exit 0
fi

# --- locate the bundled zig (same resolution as build.sh) ---------------------
ZIG=""
if   [ -x tools/zig/zig.exe ];            then ZIG="tools/zig/zig.exe"
elif [ -x tools/zig/zig ];                then ZIG="tools/zig/zig"
elif [ -x ../../c-agent/tools/zig/zig.exe ]; then ZIG="../../c-agent/tools/zig/zig.exe"
elif [ -x ../../c-agent/tools/zig/zig ];  then ZIG="../../c-agent/tools/zig/zig"
elif command -v zig >/dev/null 2>&1;      then ZIG="zig"
else
    echo "ERROR: zig not found (tools/zig or PATH)." >&2
    exit 1
fi
command -v python >/dev/null 2>&1 || { echo "ERROR: python required to generate the icon." >&2; exit 1; }

# --- locate Inno Setup compiler -----------------------------------------------
ISCC=""
for c in \
    "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" \
    "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "/c/Program Files/Inno Setup 6/ISCC.exe" \
    "$(command -v ISCC.exe 2>/dev/null)" \
    "$(command -v ISCC 2>/dev/null)" ; do
    if [ -n "$c" ] && [ -x "$c" ]; then ISCC="$c"; break; fi
done
if [ -z "$ISCC" ]; then
    echo "ERROR: Inno Setup not found. Install it with: winget install JRSoftware.InnoSetup" >&2
    exit 1
fi

# --- 1. build CLI -------------------------------------------------------------
echo "[1/5] building release CLI binary"
./build.sh cli
BIN="build/cagent"
[ -f "$BIN" ] || BIN="build/cagent.exe"
[ -f "$BIN" ] || { echo "ERROR: build/cagent[.exe] not produced." >&2; exit 1; }

# --- 2. build desktop shell ---------------------------------------------------
echo "[2/5] building native desktop shell (WebView2)"
"$ZIG" c++ -std=c++17 -O1 -Wno-nullability-completeness -I third_party/webview \
    -o "$DESKTOP" tools/desktop.cc -Wl,--subsystem=windows \
    -lole32 -loleaut32 -luuid -lshlwapi -luser32 -lgdi32 -lws2_32 -lshell32 \
    -lversion -ladvapi32

# --- 3. generate icon ---------------------------------------------------------
echo "[3/5] generating application icon"
python tools/gen_icon.py

# --- 4. build installer -------------------------------------------------------
echo "[4/5] building installer (Inno Setup)"
"$ISCC" //Q tools/setup.iss

# --- 5. report ----------------------------------------------------------------
echo "[5/5] done"
if [ -f "$OUT" ]; then
    sz=$(du -h "$OUT" | cut -f1)
    echo ""
    echo "  installer : $OUT"
    echo "  size      : $sz"
    echo ""
    echo "  Run it to install to %LOCALAPPDATA%\\c-agent with desktop + start-menu"
    echo "  shortcuts and an uninstaller. The installed app opens the web console"
    echo "  in a single native window (no console, no browser)."
else
    echo "ERROR: installer was not produced." >&2
    exit 1
fi
