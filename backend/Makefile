# c-agent Makefile — used on Linux (gcc) and optionally on Windows with make+gcc.
# Also works with the bundled zig via:  make CC="tools/zig/zig cc"
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON -Ithird_party/wasm3
LDLIBS  := $(shell uname | grep -qi 'mingw\|msys\|cygwin' && echo '-lws2_32 -lm' || echo '-lpthread -ldl -lm')

SRCS := $(shell find src third_party/cJSON -name '*.c' | sort) third_party/wasm3/wasm3_all.c

all: build/cagent build/cagent-test build/mock-llm-server build/test-adapters build/cagent-e2e build/cagent-bench

build/cagent: $(SRCS) cli/main.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) cli/main.c $(LDLIBS)

build/cagent-test: $(SRCS) tests/test_all.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_all.c $(LDLIBS)

build/mock-llm-server: $(SRCS) tools/mock_llm_server.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tools/mock_llm_server.c $(LDLIBS)

build/test-adapters: $(SRCS) tests/test_adapters.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_adapters.c $(LDLIBS)

build/cagent-e2e: $(SRCS) tests/test_e2e.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/test_e2e.c $(LDLIBS)

build/cagent-bench: $(SRCS) tests/bench_agent.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/bench_agent.c $(LDLIBS)

build/cagent-bench-real: $(SRCS) tests/bench_real.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) tests/bench_real.c $(LDLIBS)

bench: build/cagent-bench
	./build/cagent-bench --mock

clean:
	rm -rf build

.PHONY: all clean bench
