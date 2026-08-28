#!/usr/bin/env bash
# coverage.sh — function-coverage measurement (Windows / zig toolchain).
#
# Builds the test suite with -finstrument-functions + tools/cov_rt.c, runs it,
# then resolves the recorded PCs against the exe's CodeView symbols via
# tools/cov_resolve.c (DbgHelp) and prints per-source-file coverage.
#
# Usage: ./tools/coverage.sh
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG=""
if   [ -x tools/zig/zig.exe ]; then ZIG="tools/zig/zig.exe"
elif [ -x tools/zig/zig     ]; then ZIG="tools/zig/zig"
elif [ -x ../../c-agent/tools/zig/zig.exe ]; then ZIG="../../c-agent/tools/zig/zig.exe"
elif [ -x ../../c-agent/tools/zig/zig     ]; then ZIG="../../c-agent/tools/zig/zig"
elif command -v zig >/dev/null 2>&1; then ZIG="zig"
else
  echo "ERROR: zig not found." >&2
  exit 1
fi

SRCS="$(find src third_party/cJSON -name '*.c' | sort) third_party/wasm3/wasm3_all.c tools/cov_rt.c tests/test_all.c"

echo "[cov] building instrumented test binary"
"$ZIG" cc -std=c11 -Wall -Wextra -O0 -g -finstrument-functions \
    -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3 \
    -o build/cagent-cov.exe $SRCS -lws2_32 -lwinhttp -lm

echo "[cov] running tests (records cov_hits.txt)"
./build/cagent-cov.exe >/dev/null

echo "[cov] building symbol resolver"
"$ZIG" cc -std=c11 -O1 -o build/cov_resolve.exe tools/cov_resolve.c -ldbghelp

echo "[cov] report"
ROOT="$(pwd)"
if command -v cygpath >/dev/null 2>&1; then ROOT="$(cygpath -w "$(pwd)")"; fi
# overall (includes vendored third_party cJSON + wasm3)
./build/cov_resolve.exe build/cagent-cov.exe build/cov_hits.txt "$ROOT"
echo
# first-party only (src/) — the target number. Vendored wasm3/cJSON are
# third-party and drag the raw total down via macro-expanded opcode handlers.
./build/cov_resolve.exe build/cagent-cov.exe build/cov_hits.txt "$ROOT\\src"
