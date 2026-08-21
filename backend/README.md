# c-agent — Cognitive OS Runtime

A small, self-contained "cognitive OS runtime" written in C11. It wires together a
cognitive kernel (event bus, scheduler, state machine, policy engine), a reasoning
service with memory, an LLM runtime that speaks **both OpenAI-compatible and
Anthropic** protocols, an action runtime (file / shell / git / MCP tools), a
transaction + snapshot layer with rollback, a knowledge index, and a user layer
(HTTP API + embedded web UI + interactive CLI).

The design follows the principle that the LLM is a *cognitive accelerator*, not the
control center: every model call goes through the same policy-checked, transactional
pipeline as any other action.

## Architecture

```
User Layer          CLI · Web UI · HTTP API (/v1/tasks, /v1/tools, /v1/memory,
                    /v1/blackboard, /v1/agents, /metrics) · optional bearer-token auth
Cognitive Kernel    Event Bus · M:N Scheduler (coroutine tasks on a worker thread pool)
                    · State Machine (RECEIVE→UNDERSTAND→REASON→PLAN→ACT→VERIFY→LEARN)
                    · Policy Engine (allow/deny/ask + risk)
Coordination        Multi-agent pool (named agents + roles) · thread-safe Blackboard
Service Layer       Reasoning (planner→actor loop) · Memory (working / long-term /
                    experience / vector-lite)
LLM Runtime         unified vtable: mock · openai (POST /v1/chat/completions) ·
                    anthropic (POST /v1/messages) — both non-streaming + SSE stream
Action Runtime      Tool Manager: file_read · file_write · shell · git · mcp
Transaction Layer   BEGIN → Snapshot → Execute → Validate → COMMIT / ROLLBACK
Snapshot Engine     COW content store (state/snapshots) + delta + metadata
Knowledge System    keyword inverted index (scan → token → file/symbol → line)
API Primitives      HTTP/1.1 server · RFC6455 WebSocket (SHA1+base64 handshake +
                    frame build/parse) · API-key auth (constant-time compare)
Plugin Runtime      dynamic loader (dlopen / LoadLibrary)
OS Abstraction      threads · coroutines (ucontext/Fiber) · sockets · files · processes
                    · time · HTTP/1.1 client
Infrastructure      logging · config · metrics · audit (JSONL) · persistence
```

## Build

The repo bundles a portable **Zig** toolchain under `tools/zig/` so `zig cc` acts as a
gcc-compatible C compiler with no system compiler required.

- **Windows / MSYS2-Git-Bash**: `./build.sh` (or `build.bat`)
- **Linux**: `gcc -D_GNU_SOURCE -std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON $(find src third_party/cJSON -name "*.c") cli/main.c -lpthread -ldl -lm -o build/cagent`
- `CMakeLists.txt` is provided as an alternative if you have CMake.

`build.sh` targets: `all` (default), `cli`, `test`, `mock`, `e2e`, `bench`, `clean`.

## Concurrency model

The scheduler is **M:N**: M coroutine tasks run cooperatively on a pool of N OS
threads. Tasks are cheap (a coroutine is lazily created only when a worker first
dequeues it, with a 256 KB stack), and a task can yield back to the scheduler with
`ca_scheduler_yield()` to let another task run. Stack-switching is implemented in
`src/os/os_coro.c` — `ucontext` on Linux, `Fiber` on Windows — behind a single
`ca_coro` primitive. The reasoning pipeline yields between tool actions for a
fine-grained cancellation/timeout checkpoint.

## Usage

```
./build/cagent                      # interactive CLI
run 创建 note.txt 写入内容为 hello   # run a task through the full pipeline
tools                              # list registered tools
memory                             # show memory state
snapshot list                      # list snapshots
snapshot rollback <id>             # undo a transaction (file-level rollback)
serve <port>                       # start the HTTP API + web UI
events                             # print event bus traffic
help / exit
```

`./build/cagent serve 8080` then `curl`:

```
curl -X POST localhost:8080/v1/tasks -d '{"prompt":"创建 a.txt 写入 hi"}'
curl localhost:8080/v1/tasks/0
curl localhost:8080/v1/tools
curl localhost:8080/v1/memory
curl localhost:8080/v1/blackboard        # shared blackboard snapshot
curl localhost:8080/v1/agents            # registered agents + facts
curl localhost:8080/metrics
```

Open `http://localhost:8080/` for the embedded web console.

### Auth

If an API key is configured (`CA_AUTH_KEY=<secret>` or `auth.key` in config),
the `/v1/*` routes require `Authorization: Bearer <secret>`. Without a key the
API is open (the default). The `ca_auth` module also provides standalone
`ca_auth_check` / `ca_auth_check_header` for use outside the HTTP layer.

## LLM providers

Configure via JSON config (`state/<name>/cagent.json`), CLI flags, or env vars with a
`CA_` prefix (e.g. `CA_LLM_PROVIDER=openai`, `CA_LLM_BASE_URL=http://localhost:11434`).

- `mock` — offline fake provider (end-to-end tests, no network)
- `openai` — OpenAI-compatible endpoints (also Ollama / llama.cpp / vLLM / DeepSeek…)
- `anthropic` — Anthropic `/v1/messages` with `x-api-key`

HTTP is plain `http://` via the bundled socket client; `https://` can be enabled by
building with libcurl (`CA_WITH_CURL`).

## Tests

```
./build/mock-llm-server 9000 &       # start the mock LLM server
./build/cagent-test                  # unit suite (util/core/coro/M:N/snapshot/llm/…)
./build/test-adapters                # both adapters, chat + SSE stream
./build/cagent-e2e                   # full pipeline, both providers
```

Current results (verified on Windows + Linux):

```
unit:       483 passed, 0 failed
adapters:   ADAPTER PASS (openai + anthropic, chat + stream)
e2e:        E2E PASS (openai + anthropic)
bench:      --mock tool-selection accuracy 100%
```

## Agent + LLM benchmark

`./build/cagent-bench` measures agent capability against ground-truth tasks
(tool selection, end-to-end success, file side effects, multi-step completion,
and latency).

```
./build/cagent-bench --mock          # offline, deterministic mock planner
CA_LLM_PROVIDER=openai CA_LLM_BASE_URL=… CA_LLM_MODEL=… CA_LLM_API_KEY=… \
  ./build/cagent-bench --real        # real endpoint: accuracy + latency + success
```

`--mock` exercises the offline planner (tool-selection accuracy 100%).
`--real` drives a live LLM — e.g. an OpenAI-compatible endpoint such as DeepSeek
or Ollama — and reports tool-selection accuracy, success rate, per-task latency
(avg/p50/p90/max), and a one-line JSON summary for scripting.

Because the bundled HTTP client is plain-TCP only, a real Anthropic HTTPS
endpoint can be reached through a local TLS-terminating proxy (see
`build/llm_proxy.js`):

```
node build/llm_proxy.js &             # 127.0.0.1:8000 -> api.deepseek.com/anthropic
CA_LLM_PROVIDER=anthropic CA_LLM_BASE_URL=http://127.0.0.1:8000 \
  CA_LLM_MODEL=deepseek-v4-pro CA_LLM_API_KEY=dummy ./build/cagent-bench --real
```

Verified against the DeepSeek Anthropic-compatible endpoint (`deepseek-v4-pro`):
tool-selection 5/5, end-to-end 5/5, side-effect 2/2, multi-step 1/1, avg latency
~2.1 s/task.

## Layout

```
include/cagent/      public headers (one per module, by layer)
src/runtime/         event bus · scheduler · state machine · policy · agent · task
src/cognition/       reasoning · planner · evaluator · blackboard
src/memory/          facade + kv (facts) · vector · graph · episode sub-stores
src/retrieval/       knowledge index · embeddings · context builder
src/plugin_runtime/  loader (dlopen / LoadLibrary) · sandbox · capability tokens
src/plugin_intelligence/  analyzer · architect · codegen · testing · security
src/llm/             mock · openai · anthropic adapters · SSE
src/action/          tools (file / shell / git / mcp)
src/tx/ src/snapshot/  transactions + COW snapshots
src/api/             http server · REST · auth · websocket
src/os/ src/infra/   os abstraction + logging/config/metrics/audit/persist
cli/main.c           interactive CLI
tests/               unit + adapter + e2e + benchmark
tools/               mock-llm-server
third_party/cJSON/   vendored cJSON (MIT)
state/               runtime data (generated): logs, memory, snapshots, audit
```
