#!/usr/bin/env bash
# cognitive-os-agent build script — compiles with the bundled portable zig toolchain.
#   build.sh            build everything
#   build.sh cli        build only the CLI
#   build.sh test       build only the test binary
#   build.sh clean      remove build outputs
set -euo pipefail
cd "$(dirname "$0")"

OS="$(uname -s)"
ZIG=""
if   [ -x tools/zig/zig.exe ]; then ZIG="tools/zig/zig.exe"
elif [ -x tools/zig/zig     ]; then ZIG="tools/zig/zig"
elif [ -x ../../c-agent/tools/zig/zig.exe ]; then ZIG="../../c-agent/tools/zig/zig.exe"
elif [ -x ../../c-agent/tools/zig/zig     ]; then ZIG="../../c-agent/tools/zig/zig"
elif command -v zig >/dev/null 2>&1; then ZIG="zig"
else
  echo "ERROR: zig not found. Download it to tools/ (see README) or install zig into PATH." >&2
  exit 1
fi

CC="$ZIG cc"
CFLAGS="-std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3"
LIBS=""
EXE=""
case "$OS" in
  MINGW*|MSYS*|CYGWIN*) LIBS="-lws2_32 -lwinhttp -lm"; EXE=".exe" ;;
  *)                    LIBS="-lpthread -ldl -lm" ;;
esac

SRCS="$(find src third_party/cJSON -name '*.c' | sort) third_party/wasm3/wasm3_all.c"
mkdir -p build

# Regenerate the embedded web UI (include/cognitive-os-agent/api/web_ui.h) from
# apps/web/index.html whenever the page or the generator script is newer.
if [ -f apps/web/index.html ] && { [ apps/web/index.html -nt include/cognitive-os-agent/api/web_ui.h ] || [ tools/gen_web_ui.py -nt include/cognitive-os-agent/api/web_ui.h ]; }; then
  python tools/gen_web_ui.py
fi

TARGET="${1:-all}"
case "$TARGET" in
  all)
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
  clean)
    rm -rf build
    ;;
  *)
    echo "usage: $0 [all|cli|test|mock|e2e|bench|bench-real|bench-bfcl|clean]" >&2
    exit 1
    ;;
esac
echo "[done]"
