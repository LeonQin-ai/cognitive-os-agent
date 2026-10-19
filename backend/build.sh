#!/usr/bin/env bash
# cognitive-os-agent build script — compiles with the pinned Zig toolchain on Windows.
#   build.sh            build everything
#   build.sh cli        build only the CLI
#   build.sh test       build only the test binary
#   build.sh clean      remove build outputs
set -euo pipefail
cd "$(dirname "$0")"

OS="$(uname -s)"
ZIG=""
if   [[ "$OS" == Darwin* ]]; then ZIG=""
elif [ -x tools/zig/zig.exe ]; then ZIG="tools/zig/zig.exe"
elif [ -x tools/zig/zig     ]; then ZIG="tools/zig/zig"
elif command -v zig >/dev/null 2>&1; then ZIG="zig"
else
  case "$OS" in
  MINGW*|MSYS*|CYGWIN*)
    if command -v powershell.exe >/dev/null 2>&1; then
      powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/ensure-zig.ps1
      ZIG="tools/zig/zig.exe"
    elif command -v pwsh >/dev/null 2>&1; then
      pwsh -NoProfile -ExecutionPolicy Bypass -File tools/ensure-zig.ps1
      ZIG="tools/zig/zig.exe"
    else
      echo "ERROR: zig not found and PowerShell is unavailable to download the pinned Windows compiler." >&2
      exit 1
    fi
    ;;
  *)
    echo "ERROR: zig not found. Install Zig or use the platform's documented build command." >&2
    exit 1
    ;;
  esac
fi

# Keep the local cached Windows compiler aligned with CI. A stale local Zig
# should not silently build the release with a different toolchain version.
if [[ "$OS" == MINGW* || "$OS" == MSYS* || "$OS" == CYGWIN* ]] && [ "$("$ZIG" version 2>/dev/null || true)" != "0.14.1" ]; then
  if command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/ensure-zig.ps1
  elif command -v pwsh >/dev/null 2>&1; then
    pwsh -NoProfile -ExecutionPolicy Bypass -File tools/ensure-zig.ps1
  else
    echo "ERROR: tools/zig/zig.exe is not Zig 0.14.1 and PowerShell is unavailable to update it." >&2
    exit 1
  fi
  ZIG="tools/zig/zig.exe"
fi

if [[ "$OS" == Darwin* ]]; then
  CC="$(xcrun --find clang)"
else
  CC="$ZIG cc"
fi
CFLAGS="-std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3"
LIBS=""
EXE=""
case "$OS" in
  MINGW*|MSYS*|CYGWIN*) LIBS="-lws2_32 -lwinhttp -lbcrypt -lcrypt32 -lshell32 -lm"; EXE=".exe"; PLAT="src/os/windows"; POSIX="" ;;
  Darwin*)              LIBS="-lpthread -lm -framework Security -framework CoreFoundation"; EXE=""; PLAT="src/os/macos"; POSIX="src/os/posix" ;;
  *)                    LIBS="-lpthread -ldl -lm";    EXE="";      PLAT="src/os/linux";  POSIX="src/os/posix" ;;
esac

# platform backends live in per-OS dirs (issue #19): compile exactly one OS
# directory + the shared POSIX base, exclude all of them from the common set
SRCS="$(find src third_party/cJSON -name '*.c' | grep -v -E '^src/os/(linux|macos|windows|posix)/' | sort) $(find $POSIX "$PLAT" -name '*.c' 2>/dev/null) third_party/wasm3/wasm3_all.c"
mkdir -p build

# Always regenerate the embedded web UI before compiling. Timestamp checks can
# leave a stale page embedded in a freshly built executable (for example after
# archive extraction or when source/header mtimes are preserved).
python3 tools/gen_web_ui.py

python3 tools/gen_attachment_extractor.py

TARGET="${1:-all}"
case "$TARGET" in
  all)
    echo "[build] bench-runtime"
    $CC $CFLAGS -o build/bench-runtime$EXE $SRCS tests/bench_runtime.c $LIBS
    echo "[build] cognitive-os-agent"
    $CC $CFLAGS -o build/cognitive-os-agent$EXE $SRCS cli/main.c $LIBS
    echo "[build] cognitive-os-agent-test"
    $CC $CFLAGS -o build/cognitive-os-agent-test$EXE $SRCS tests/test_all.c $LIBS
    echo "[build] cognitive-os-agent-scenario"
    $CC $CFLAGS -o build/cognitive-os-agent-scenario$EXE $SRCS tests/test_scenario.c $LIBS
    echo "[build] mock-llm-server"
    $CC $CFLAGS -o build/mock-llm-server$EXE $SRCS tools/mock_llm_server.c $LIBS
    echo "[build] test-adapters"
    $CC $CFLAGS -o build/test-adapters$EXE $SRCS tests/test_adapters.c $LIBS
    echo "[build] cognitive-os-agent-e2e"
    $CC $CFLAGS -o build/cognitive-os-agent-e2e$EXE $SRCS tests/test_e2e.c $LIBS
    echo "[build] cognitive-os-agent-bench"
    $CC $CFLAGS -o build/cognitive-os-agent-bench$EXE $SRCS tests/bench_agent.c $LIBS
    echo "[build] cognitive-os-agent-bench-real"
    $CC $CFLAGS -o build/cognitive-os-agent-bench-real$EXE $SRCS tests/bench_real.c $LIBS
    echo "[build] cognitive-os-agent-bench-bfcl"
    $CC $CFLAGS -o build/cognitive-os-agent-bench-bfcl$EXE $SRCS tests/bench_bfcl.c $LIBS
    echo "[build] cognitive-os-agent-bench-gaia"
    $CC $CFLAGS -o build/cognitive-os-agent-bench-gaia$EXE $SRCS tests/bench_gaia.c $LIBS
    ;;
  cli)
    echo "[build] cognitive-os-agent"
    $CC $CFLAGS -o build/cognitive-os-agent$EXE $SRCS cli/main.c $LIBS
    ;;
  test)
    echo "[build] cognitive-os-agent-test"
    $CC $CFLAGS -o build/cognitive-os-agent-test$EXE $SRCS tests/test_all.c $LIBS
    ;;
  scenario)
    echo "[build] cognitive-os-agent-scenario"
    $CC $CFLAGS -o build/cognitive-os-agent-scenario$EXE $SRCS tests/test_scenario.c $LIBS
    ;;
  mock)
    echo "[build] mock-llm-server"
    $CC $CFLAGS -o build/mock-llm-server$EXE $SRCS tools/mock_llm_server.c $LIBS
    ;;
  e2e)
    echo "[build] cognitive-os-agent-e2e"
    $CC $CFLAGS -o build/cognitive-os-agent-e2e$EXE $SRCS tests/test_e2e.c $LIBS
    ;;
  runtime-bench)
    echo "[build] bench-runtime"
    $CC $CFLAGS -o build/bench-runtime$EXE $SRCS tests/bench_runtime.c $LIBS
    ;;
  bench)
    echo "[build] cognitive-os-agent-bench"
    $CC $CFLAGS -o build/cognitive-os-agent-bench$EXE $SRCS tests/bench_agent.c $LIBS
    ;;
  bench-real)
    echo "[build] cognitive-os-agent-bench-real"
    $CC $CFLAGS -o build/cognitive-os-agent-bench-real$EXE $SRCS tests/bench_real.c $LIBS
    ;;
  bench-bfcl)
    echo "[build] cognitive-os-agent-bench-bfcl"
    $CC $CFLAGS -o build/cognitive-os-agent-bench-bfcl$EXE $SRCS tests/bench_bfcl.c $LIBS
    ;;
  bench-gaia)
    echo "[build] cognitive-os-agent-bench-gaia"
    $CC $CFLAGS -o build/cognitive-os-agent-bench-gaia$EXE $SRCS tests/bench_gaia.c $LIBS
    ;;
  test-plan-cycle)
    echo "[build] cognitive-os-agent-test-plan-cycle"
    $CC $CFLAGS -o build/cognitive-os-agent-test-plan-cycle$EXE $SRCS tests/test_plan_cycle.c $LIBS
    ;;
  clean)
    rm -rf build
    ;;
  *)
    echo "usage: $0 [all|cli|test|mock|e2e|bench|bench-real|bench-bfcl|bench-gaia|test-plan-cycle|clean]" >&2
    exit 1
    ;;
esac
echo "[done]"
