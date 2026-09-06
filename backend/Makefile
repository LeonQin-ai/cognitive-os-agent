# cognitive-os-agent Makefile — used on Linux (gcc) and optionally on Windows with make+gcc.
# Also works with the bundled zig via:  make CC="tools/zig/zig cc"
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3
LDLIBS  := $(shell uname | grep -qi 'mingw\|msys\|cygwin' && echo '-lws2_32 -lwinhttp -lm' || echo '-lpthread -ldl -lm')

SRCS := $(shell find src third_party/cJSON -name '*.c' | sort) third_party/wasm3/wasm3_all.c

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
