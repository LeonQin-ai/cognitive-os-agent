#!/usr/bin/env bash
# c-agent build script — compiles with the bundled portable zig toolchain.
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
case "$OS" in
  MINGW*|MSYS*|CYGWIN*) LIBS="-lws2_32 -lwinhttp -lm" ;;
  *)                    LIBS="-lpthread -ldl -lm" ;;
esac

SRCS="$(find src third_party/cJSON -name '*.c' | sort) third_party/wasm3/wasm3_all.c"
mkdir -p build

# Regenerate the embedded web UI (include/cagent/api/web_ui.h) from
# apps/web/index.html whenever the source is newer.
if [ -f apps/web/index.html ] && [ tools/gen_web_ui.py -nt include/cagent/api/web_ui.h ]; then
  python tools/gen_web_ui.py
fi

TARGET="${1:-all}"
case "$TARGET" in
  all)
    echo "[build] cagent"
    $CC $CFLAGS -o build/cagent $SRCS cli/main.c $LIBS
    echo "[build] cagent-test"
    $CC $CFLAGS -o build/cagent-test $SRCS tests/test_all.c $LIBS
    echo "[build] mock-llm-server"
    $CC $CFLAGS -o build/mock-llm-server $SRCS tools/mock_llm_server.c $LIBS
    echo "[build] test-adapters"
    $CC $CFLAGS -o build/test-adapters $SRCS tests/test_adapters.c $LIBS
    echo "[build] cagent-e2e"
    $CC $CFLAGS -o build/cagent-e2e $SRCS tests/test_e2e.c $LIBS
    echo "[build] cagent-bench"
    $CC $CFLAGS -o build/cagent-bench $SRCS tests/bench_agent.c $LIBS
    echo "[build] cagent-bench-real"
    $CC $CFLAGS -o build/cagent-bench-real $SRCS tests/bench_real.c $LIBS
    ;;
  cli)
    echo "[build] cagent"
    $CC $CFLAGS -o build/cagent $SRCS cli/main.c $LIBS
    ;;
  test)
    echo "[build] cagent-test"
    $CC $CFLAGS -o build/cagent-test $SRCS tests/test_all.c $LIBS
    ;;
  mock)
    echo "[build] mock-llm-server"
    $CC $CFLAGS -o build/mock-llm-server $SRCS tools/mock_llm_server.c $LIBS
    ;;
  e2e)
    echo "[build] cagent-e2e"
    $CC $CFLAGS -o build/cagent-e2e $SRCS tests/test_e2e.c $LIBS
    ;;
  bench)
    echo "[build] cagent-bench"
    $CC $CFLAGS -o build/cagent-bench $SRCS tests/bench_agent.c $LIBS
    ;;
  bench-real)
    echo "[build] cagent-bench-real"
    $CC $CFLAGS -o build/cagent-bench-real $SRCS tests/bench_real.c $LIBS
    ;;
  clean)
    rm -rf build
    ;;
  *)
    echo "usage: $0 [all|cli|test|mock|e2e|bench|bench-real|clean]" >&2
    exit 1
    ;;
esac
echo "[done]"
