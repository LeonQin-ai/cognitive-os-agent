# cognitive-os-agent — Cognitive OS Runtime

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
Cognitive Kernel    Event Bus · M:N Scheduler (priority heap, elastic bounded worker pool)
                    · State Machine (RECEIVE→UNDERSTAND→REASON→PLAN→ACT→VERIFY→LEARN)
                    · Policy Engine (allow/deny/ask + risk)
Coordination        Multi-agent pool (named agents + roles) · thread-safe Blackboard
Service Layer       Reasoning (planner→actor loop) · Memory (working / long-term /
                    experience / vector-lite)
LLM Runtime         unified vtable: mock · openai (POST /v1/chat/completions) ·
                    anthropic (POST /v1/messages) — both non-streaming + SSE stream
Action Runtime      Tool Manager: file_read · file_write · file_edit · shell · ssh · git · mcp · glob · grep · skill
Transaction Layer   BEGIN → Snapshot → Execute → Validate → COMMIT / ROLLBACK
Snapshot Engine     COW content store (state/snapshots) + delta + metadata
Knowledge System    keyword inverted index (scan → token → file/symbol → line)
API Primitives      HTTP/1.1 server · RFC6455 WebSocket (SHA1+base64 handshake +
                    frame build/parse) · API-key auth (constant-time compare)
Plugin Runtime      dynamic loader (dlopen / LoadLibrary) · wasm3 sandbox (wasm)
OS Abstraction      threads · coroutines (ucontext/Fiber) · sockets · files · processes
                    · time · HTTP/1.1 client
Infrastructure      logging · config · metrics · audit (JSONL) · persistence
```

## Build

The repo bundles a portable **Zig** toolchain under `tools/zig/` so `zig cc` acts as a
gcc-compatible C compiler with no system compiler required.

- **Windows**: `.\build.ps1 all` (or `build.bat` / `./build.sh`)
- **Linux / macOS**: `make`; macOS can additionally run `bash package-macos.sh`
- `CMakeLists.txt` is provided as an alternative if you have CMake.

`build.sh` targets: `all` (default), `cli`, `test`, `mock`, `e2e`, `bench`, `clean`.

## Concurrency model

The scheduler is **M:N**: M coroutine tasks run cooperatively on a pool of N OS
threads. Tasks are cheap (a coroutine is lazily created only when a worker first
dequeues it, with a 256 KB stack), and a task can yield back to the scheduler with
`coa_scheduler_yield()` to let another task run. Stack-switching is implemented in
`src/os/os_coro.c` — `ucontext` on Linux, `Fiber` on Windows — behind a single
`coa_coro` primitive. The reasoning pipeline yields between tool actions for a
fine-grained cancellation/timeout checkpoint.

## Usage

The CLI reads `state/cognitive-os-agent.json` from the current directory by
default. To use another file, pass `--config FILE`; its directory becomes the
state directory unless `--state DIR` is supplied. The `workspace` field in the
JSON file controls where tools operate; `--workspace DIR` overrides it.

### Local Skills and MCP

The runtime creates two folders inside its state directory at startup. Copy a
skill folder containing `SKILL.md` to `<state>/skills/<skill-name>/SKILL.md`, or
put a single `.md` file directly in `<state>/skills/`. Its YAML `name` and
`description` fields are used when present; the entire file is registered as a
prompt skill for the agent. For example, with the default state directory:

```text
state/skills/code-review/SKILL.md
```

Copy one JSON file per MCP connection into `<state>/mcp/`. A file may contain a
single connection (`name` defaults to the filename), or a standard
`mcpServers` object:

```json
{"mcpServers":{"local":{"command":"node","args":["/path/to/server.js"]}}}
```

For HTTP, use `{"name":"local","transport":"http","url":"http://127.0.0.1:3000/mcp"}`.
The MCP server executable and any runtime it needs must be installed locally.
JSON `args` arrays preserve paths with spaces; plain string arguments still use
space separation. Per-server `env` maps are not supported, so set environment
variables before launching the agent. Secrets in MCP connection files remain local to the
backend and are not sent to the LLM as configuration text.

Restart to load copied files automatically, or click **重新加载** on the Skills
or MCP page. The pages display the effective state folder paths. A custom
`--state` directory works the same way.

```json
{
  "llm.provider": "openai",
  "llm.model": "your-model",
  "llm.base_url": "https://your-provider.example/v1",
  "llm.api_key": "your-key",
  "workspace": "."
}
```

```sh
./build/cognitive-os-agent --config state/cognitive-os-agent.json
# At the prompt, enter a task directly. Commands: /help, /config, /tools,
# /memory, /snapshot list, /exit.

./build/cognitive-os-agent --config state/cognitive-os-agent.json run "分析项目中的 bug"
./build/cognitive-os-agent --config state/cognitive-os-agent.json "分析项目中的 bug"
./build/cognitive-os-agent --config state/cognitive-os-agent.json config
./build/cognitive-os-agent --config state/cognitive-os-agent.json serve 8080
```

`config` prints an effective summary and only reports whether an API key is
configured. `COA_*` environment values override file values. The terminal
shows each agent stage while a task runs. On Windows, the CLI decodes Unicode
command-line arguments before running a task, so Chinese text and paths work.

`./build/cognitive-os-agent serve 8080` then `curl`:

```
curl -X POST localhost:8080/v1/tasks -d '{"prompt":"创建 a.txt 写入 hi"}'
curl localhost:8080/v1/tasks/0
curl -X POST localhost:8080/v1/tasks/0/messages -d '{"message":"Add tests, then continue"}'
curl localhost:8080/v1/tools
curl localhost:8080/v1/memory
curl localhost:8080/v1/blackboard        # shared blackboard snapshot
curl localhost:8080/v1/agents            # registered agents + facts
curl localhost:8080/metrics
```

Open `http://localhost:8080/` for the embedded web console.

### Skills / MCP / Plugin 广场

The console plaza surfaces curated catalogs over the HTTP API:

- `GET /v1/catalog/mcp` — MCP 服务器目录（含 GitHub 热门 MCP 应用：fetch、memory、
  sequential-thinking、puppeteer、notion 等，附仓库链接）
- `GET /v1/skills/market` — 技能市场模板 + GitHub 热门应用列表
- `GET /v1/plugins/market` — 插件市场模板 + GitHub 热门应用列表
- `POST /v1/plugins/generate` — AI 插件自动生成（分析→架构→生成→安全审计→注册→可执行）
- `GET /v1/market/status` — 联网市场状态：`{"configured":…,"url":…,"online":…}`
- `POST /v1/skills/publish` / `POST /v1/plugins/publish` — 发布技能/插件，并尝试推送到
  联网市场（`pushed_to_market` 字段表示是否推送成功）

GitHub 条目带 `repo` 字段（仓库 URL），Web 控制台渲染为可点击链接，便于一键跳转
到对应开源项目。

#### 联网市场（marketplace）

市场（market）模块可让本地广场与一个远程市场服务器**双向互通**：

- **拉取**：`GET /v1/skills/market` / `GET /v1/plugins/market` 会向远程市场
  `GET /v1/skills/market` / `GET /v1/plugins/market`，把远端 `templates` / `github`
  条目合入本地列表（每条标记 `"source":"remote"`），并设置 `market_online` 字段。
- **推送**：`POST /v1/skills/publish` / `POST /v1/plugins/publish` 在本地注册后，
  会把请求体原样 `POST` 到远程市场的同路径（best-effort，失败不影响本地结果）。

配置方式（任选其一）：

```
# 配置文件 state/<name>/cognitive-os-agent.json
{ "market.url": "http://market.example.com:9000" }

# 环境变量
COA_MARKET_URL=http://market.example.com:9000

# CLI / 编程接口
coa_config.market_url = "http://market.example.com:9000"
```

未配置时本地广场保持纯离线（`market_online=false`、`configured=false`），行为与之前一致。
市场模块仅依赖本机 HTTP 客户端（Windows: WinHTTP，Linux: 裸 socket），不引入额外依赖。

### Auth

If an API key is configured (`COA_AUTH_KEY=<secret>` or `auth.key` in config),
the `/v1/*` routes require `Authorization: Bearer <secret>`. Without a key the
API is open (the default). The `coa_auth` module also provides standalone
`coa_auth_check` / `coa_auth_check_header` for use outside the HTTP layer.

The server binds to `127.0.0.1` by default. Keep that loopback boundary for
local use. If the API must be exposed to another machine, terminate HTTPS at a
reverse proxy such as Caddy or Nginx, configure an auth key, and proxy only to
the loopback listener. The embedded server intentionally does not advertise
plain HTTP plus bearer tokens as a secure remote deployment mode.

### SSH environments

The `ssh` tool accepts either a one-off `host` (with optional `user`, `port`)
or a named `environment`. Environment profiles keep non-secret connection
settings out of prompts and support separate development, staging and
production hosts:

```json
{
  "environments": {
    "staging": {
      "host": "10.0.0.8",
      "user": "deploy",
      "port": 22,
      "identity_file": "C:/keys/staging_ed25519",
      "proxy_jump": "jump.example.com",
      "known_hosts": "C:/keys/known_hosts"
    }
  }
}
```

Then invoke `ssh` with `{"environment":"staging","command":"uname -a"}`.
Save this JSON as `<state_root>/ssh/environments.json`. The tool supports
non-interactive key/agent authentication or a one-off `password` argument.
Password authentication uses the local OpenSSH askpass mechanism and a
short-lived local secret file; the password is never placed on the ssh command
line. The tool requires host-key verification, bounds the command timeout to
60 seconds, and never publishes an in-flight session task into shared memory.

On Windows, `shell` accepts `{"shell":"powershell","command":"Get-ChildItem"}`
or `"shell":"cmd"`; the default remains automatic. `file_read` returns up to
7000 bytes per call and accepts `offset_bytes` for the next page of a large
text file. The chat's deep-thinking choice is per session, while the shared
memory switch applies to every session.

## Execution capacity

The scheduler starts with `scheduler.workers` threads (default 8) and expands
up to `scheduler.max.workers` (default at least 32, maximum 256). Configure
`scheduler.max.active` to bound queued plus running tasks (default 10,000,
allowed 1–1,000,000). These are startup settings, for example:

```json
{"scheduler.workers":4,"scheduler.max.workers":16,"scheduler.max.active":1000}
```

Environment overrides are `COA_SCHEDULER_WORKERS`, `COA_SCHEDULER_MAX_WORKERS`
and `COA_SCHEDULER_MAX_ACTIVE`. `GET /v1/scheduler` reports current workers,
active/queued tasks, configured limits and rejected submissions. Task, chat,
orchestration and Flow submissions return HTTP 429 with `code:SCHEDULER_FULL`
at capacity; retry after existing work completes. Scheduled jobs retry a
rejected submission on a later tick without consuming that occurrence.

Virtual agent registrations do not allocate execution threads. Completed task
history is still retained in memory until shutdown; the active-task limit is
not a bound on total process memory, and expanded worker threads remain until
shutdown.

## LLM providers

Configure via JSON config (`state/<name>/cognitive-os-agent.json`), CLI flags, or env vars with a
`COA_` prefix (e.g. `COA_LLM_PROVIDER=openai`, `COA_LLM_BASE_URL=http://localhost:11434`).

- `mock` — offline fake provider (end-to-end tests, no network)
- `openai` — OpenAI-compatible endpoints (also Ollama / llama.cpp / vLLM / DeepSeek…)
- `anthropic` — Anthropic `/v1/messages` with `x-api-key`

HTTP transport is provided by the bundled client: on **Windows** the native
WinHTTP backend (`src/os/http_winhttp.c`, supports `https://`); on **Linux** a
plain-TCP HTTP/1.1 client (`src/os/http.c`, `http://` only). The two are
platform-guarded and compiled together (the inactive one compiles to nothing).

### 免费 key 与本地模型（一键获取 / 一键启动）

`GET /v1/catalog/models` 返回的每个预设都带 `signup_url`（免费 key 申请页）与
`local` 标志。Web 控制台的「免费模型广场」会把 `signup_url` 渲染成可点击的
「申请 key」链接，用户复制 key 后走 `POST /v1/config/llm` 一键配置并设为当前。

可在状态目录的 `models.json` 增加或覆盖预设；刷新模型目录即可生效，无需重新编译。
文件是 JSON 数组，每项填写 `id`、`name`、`provider`（`openai`/`anthropic`）、
`base_url` 和 `model`，可选 `key_hint`、`note`、`signup_url`、`local`。
同 `id` 会覆盖内置预设，无效条目会跳过。例如：

```json
[{"id":"local-qwen","name":"本地 Qwen","provider":"openai","base_url":"http://127.0.0.1:1234/v1","model":"qwen","local":true}]
```

本地模型（Ollama / llama.cpp，无需 key）支持一键检测与启动：

```
GET  /v1/local/status            # 探测本机运行时：{"ollama":{running,installed},"llamacpp":{running}}
POST /v1/local/start             # body {"engine":"ollama"} 或 {"engine":"llamacpp"}
```

- **Ollama**：自动在 PATH / 常见安装目录找 `ollama`，找不到时报错引导安装；找到则
  `ollama serve` 后台拉起（非阻塞，立即返回，UI 轮询状态）。
- **llama.cpp / vLLM**：默认不自动启动，需在配置里给出启动命令
  `local.llamacpp_cmd`（如你的 server 可执行文件路径）；配置后同样后台拉起。
- 底层使用 `coa_proc_spawn_detached`（Windows `CreateProcess` 分离进程组 / POSIX
  `fork+setsid`），不会占用当前会话或阻塞 HTTP 服务。

## Tests

```
./build/mock-llm-server 9000 &       # start the mock LLM server
./build/cognitive-os-agent-test                  # unit suite (util/core/coro/M:N/snapshot/llm/…)
./build/test-adapters                # both adapters, chat + SSE stream
./build/cognitive-os-agent-e2e                   # full pipeline, both providers
```

Current unit result (verified locally on Windows):

```
unit:       1622 passed, 0 failed
adapters:   ADAPTER PASS (openai + anthropic, chat + stream)
e2e:        E2E PASS (openai + anthropic)
bench:      --mock tool-selection accuracy 100%
```

## Agent + LLM benchmark

`./build/cognitive-os-agent-bench` measures agent capability against ground-truth tasks
(tool selection, end-to-end success, file side effects, multi-step completion,
and latency).

```
./build/cognitive-os-agent-bench --mock          # offline, deterministic mock planner
COA_LLM_PROVIDER=openai COA_LLM_BASE_URL=… COA_LLM_MODEL=… COA_LLM_API_KEY=… \
  ./build/cognitive-os-agent-bench --real        # real endpoint: accuracy + latency + success
```

`--mock` exercises the offline planner (tool-selection accuracy 100%).
`--real` drives a live LLM — e.g. an OpenAI-compatible endpoint such as DeepSeek
or Ollama — and reports tool-selection accuracy, success rate, per-task latency
(avg/p50/p90/max), and a one-line JSON summary for scripting.

On **Linux** the bundled client is plain-TCP only, so a real Anthropic HTTPS
endpoint can be reached through a local TLS-terminating proxy (see
`build/llm_proxy.js`); on **Windows** the WinHTTP backend connects over
`https://` directly (no proxy needed):

```
node build/llm_proxy.js &             # 127.0.0.1:8000 -> api.deepseek.com/anthropic
COA_LLM_PROVIDER=anthropic COA_LLM_BASE_URL=http://127.0.0.1:8000 \
  COA_LLM_MODEL=deepseek-v4-pro COA_LLM_API_KEY=dummy ./build/cognitive-os-agent-bench --real
```

Verified against the DeepSeek Anthropic-compatible endpoint (`deepseek-v4-pro`):
tool-selection 5/5, end-to-end 5/5, side-effect 2/2, multi-step 1/1, avg latency
~2.1 s/task.

## WASM plugin arguments

The built-in wasm3 runner accepts numeric arguments as an array or named values
in object order. Numbers must match the function signature; extra, fractional
integer and unsupported arguments return errors. JSON integer inputs must fit
the exact range of their type (i64 inputs are limited to ±9,007,199,254,740,991).

UTF-8 strings expand to two `i32` arguments: a pointer and a byte length. The
module must export `coa_alloc(i32 size) -> i32 pointer`; zero means allocation
failure. The host copies the string plus a NUL terminator, with the length
excluding that terminator. Each invocation has its own runtime and memory.

For text output, pass `{"args":["你好"],"result":"utf8"}`. The target function
returns an `i64` containing `(uint64_t(length) << 32) | pointer`. The runner
validates the guest memory range and returns `{"ok":true,"result":"你好"}`.
Input/output text is limited to 1 MiB each and cannot contain embedded NUL.
Numeric returns preserve their Wasm type; void functions return JSON `null`.

## Layout

```
include/cognitive-os-agent/      public headers (one per module, by layer)
src/runtime/         event bus · scheduler · state machine · policy · agent · task
src/cognition/       reasoning · planner · evaluator · blackboard · attention
src/memory/          facade + kv (facts) · vector · graph · episode sub-stores
src/retrieval/       knowledge index · embeddings · context builder
src/plugin_runtime/  loader (dlopen / LoadLibrary) · sandbox · capability tokens · wasm3 runner
src/plugin_intelligence/  analyzer · architect · codegen · generator · testing · security
src/llm/             mock · openai · anthropic adapters · SSE · router
src/action/          tools (file / shell / git / mcp) · skills · mcp connections
src/im/              IM channels · telegram poll bridge
src/tx/ src/snapshot/  transactions + COW snapshots
src/api/             http server · REST · auth · websocket framing · ws_server hub
src/os/              threads · coroutines (ucontext/Fiber) · sockets · files · processes
                     · time · HTTP clients (WinHTTP on Windows / POSIX on Linux and macOS)
src/infra/           logging · config · metrics · audit · persist · ringbuf · catalog
cli/main.c           interactive CLI
tests/               unit + adapter + e2e + benchmark
tools/               mock-llm-server · desktop shell (WebView2) · cov_rt/cov_resolve/coverage.sh
apps/web/            embedded web UI (regenerated into include/api/web_ui.h)
third_party/         cJSON (MIT) · wasm3 (MIT)
state/               runtime data (generated): logs, memory, snapshots, audit, skills, plugins
```
