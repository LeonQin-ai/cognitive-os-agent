# cognitive-os-agent Makefile — used on Linux (gcc) and optionally on Windows with make+gcc.
# Also works with the bundled zig via:  make CC="tools/zig/zig cc"
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3
LDLIBS  := $(shell uname | grep -qi 'mingw\|msys\|cygwin' && echo '-lws2_32 -lwinhttp -lm' || echo '-lpthread -ldl -lm')

# per-OS platform backends (issue #19): one OS directory + the shared POSIX
# base on non-Windows. sockets/fs/proc/thread/coro/time live in
# src/os/posix/, HTTP in src/os/<linux|macos>/, everything Windows-specific
# in src/os/windows/. Adding an OS = new directory implementing the headers
# under include/os/ + a selection rule here.
ifeq ($(shell uname | grep -qiE 'mingw|msys|cygwin' && echo win),win)
PLAT  := src/os/windows
POSIX :=
else ifeq (Darwin,$(shell uname -s))
PLAT  := src/os/macos
POSIX := src/os/posix
LDLIBS := -lpthread -lm
else
PLAT  := src/os/linux
POSIX := src/os/posix
endif

SRCS := $(shell find src third_party/cJSON -name '*.c' | grep -v -E '^src/os/(linux|macos|windows|posix)/' | sort) $(wildcard $(POSIX)/*.c) $(wildcard $(PLAT)/*.c) third_party/wasm3/wasm3_all.c

include/api/web_ui.h: apps/web/index.html tools/gen_web_ui.py
	python3 tools/gen_web_ui.py

$(SRCS): | include/api/web_ui.h
.DEFAULT_GOAL := all

all: build/cognitive-os-agent build/cognitive-os-agent-test build/cognitive-os-agent-scenario build/mock-llm-server build/test-adapters build/cognitive-os-agent-e2e build/cognitive-os-agent-bench

build/cognitive-os-agent: $(SRCS) cli/main.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) cli/main.c $(LDLIBS)

build/cognitive-os-agent-test: $(SRCS) tests/test_all.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_all.c $(LDLIBS)

build/mock-llm-server: $(SRCS) tools/mock_llm_server.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tools/mock_llm_server.c $(LDLIBS)

build/test-adapters: $(SRCS) tests/test_adapters.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_adapters.c $(LDLIBS)

build/cognitive-os-agent-e2e: $(SRCS) tests/test_e2e.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_e2e.c $(LDLIBS)

build/cognitive-os-agent-bench: $(SRCS) tests/bench_agent.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/bench_agent.c $(LDLIBS)

build/cognitive-os-agent-bench-real: $(SRCS) tests/bench_real.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/bench_real.c $(LDLIBS)

build/cognitive-os-agent-scenario: $(SRCS) tests/test_scenario.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_scenario.c $(LDLIBS)

test: build/cognitive-os-agent-test
	./build/cognitive-os-agent-test

scenario: build/cognitive-os-agent-scenario
	./build/cognitive-os-agent-scenario

bench: build/cognitive-os-agent-bench
	./build/cognitive-os-agent-bench --mock

clean:
	rm -rf build

.PHONY: all test scenario clean bench
