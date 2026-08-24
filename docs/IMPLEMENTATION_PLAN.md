# c-agent — 设计文档缺口落地实现规划 (Implementation Plan)

> 依据《Cognitive OS: 自进化认知操作系统全景架构与流程设计》对照实际代码后的差距清单，逐项落地。
> 每项均包含：目标 / 涉及文件 / 接口 / 验证方式。

## 差距清单与实现顺序

| # | 设计文档描述 | 实际现状 | 本次落地 |
|---|---|---|---|
| P1 | 无锁环形缓冲区 Event Bus | `event_bus.c` 互斥锁订阅分发 | ✅ `src/infra/ringbuf.c` MPMC 无锁环形缓冲（Vyukov 有界队列），事件入队无锁 + 原子批量分发；`test_ringbuf`+`test_ringbuf_mpmc`（4 生产/1 消费 8000 条，无丢无重） |
| P2 | Embedding / Rerank 接入 | `embedding.c` 仅 hashing-trick BOW | ✅ Embedding Provider：`local`（默认）+ `remote`（OpenAI 兼容 `/embeddings`，mock server 实测 norm=1.0）+ `ca_embed_rerank`；配置 `embedding.provider/base_url/api_key/model` |
| P3 | WebSocket 实时 Event 通信 | 仅有 RFC6455 原语，未接服务器 | ✅ `src/api/ws_server.c`：101 握手、客户端注册表、每客户端读线程、出站队列、`/ws` 广播；Node WebSocket 客户端实测 `im.send→im.message` 全双工回环 |
| P4 | IM 即时通讯 | 不存在 | ✅ `src/im/im.c`：会话/消息 JSON 持久化、`/v1/im/sessions*` REST、新消息 WS 广播 + 记忆沉淀、Console 聊天面板；`test_im`（含持久化重载） |
| P5 | 插件智能平面（AI 自生成） | analyzer 等为关键词启发式 | ✅ `generator.c` 自进化闭环：LLM/mock 生成 → 安全审计（security+sandbox 黑名单）→ 注册中心 + skill 注入 + 脚本落盘；`POST /v1/plugins/generate`；`test_plugin_generate`（生成→注册→skill 可执行） |
| P6 | Wasm 沙箱 | 仅 Wasm 运行器接缝 | ✅ 内嵌 wasm3（`third_party/wasm3/`，amalgamation + `-Weverything` 压制告警），`ca_sandbox_run_wasm` 实测 `add(2,40)=42`；`test_sandbox_wasm` |
| P7 | Tauri/React Console | 空目录；实际嵌入式单文件 HTML | ✅ 保持嵌入式架构（单二进制优势），Console 重组成设计文档结构：Dashboard/Agents/Plugins 中心(自生成)/IM/Execution/Monitor；WebSocket 客户端实时收 IM |
| P8 | 回归验证与打包 | — | ✅ 单测 582 passed / 0 failed（3 连跑稳定）；真实 LLM 基准：cagent-bench 5/5 tool + 5/5 e2e，bench-real ToolBench 5/5 + AgentBench 4/4（样本波动 8~9/9）；dist 重新打包 |

## P1 无锁环形缓冲 Event Bus

- `include/cagent/infra/ringbuf.h` / `src/infra/ringbuf.c`
  - `ca_ringbuf_new(cap)`（cap 为 2 的幂）、`push` / `pop`（C11 atomics，Vyukov 有界 MPMC）
- 重写 `src/runtime/event_bus.c`：`ca_event_bus_publish` 无锁入队 + 原子 `draining` 标志批量分发；订阅注册表仅在增删时加锁（低频操作）
- 验证：`test_all.c` 新增 `test_ringbuf`（多线程压入/取出、容量满/空、不丢不重）+ 既有 `test_event_bus` 保持通过

## P2 Embedding Provider + Rerank 抽象

- `include/cagent/retrieval/embedding.h` / `src/retrieval/embedding.c`
  - `ca_embedding_set_remote(base_url, api_key, model)`：`local` 默认；`remote` 走 `POST {base}/embeddings`（OpenAI 兼容）
  - `ca_embed_rerank(query, docs[], n, scores_out)`：默认 token 重叠分（local），远程可扩展
  - 配置项：`embedding.provider` / `embedding.base_url` / `embedding.api_key` / `embedding.model`
- 验证：`test_embedding`（local 余弦一致性 + remote 用 mock embeddings server 实测）

## P3 WebSocket 服务器

- `include/cagent/api/ws_server.h` / `src/api/ws_server.c`
  - `ca_ws_server_new/accept/broadcast/count/on_message`
  - accept：101 握手 → 按客户端读线程（200ms 轮询出站队列 + 读帧），支持 text/ping/pong/close
- `http_server.c`：识别 `Upgrade: websocket` 与注册的 WS 路径（`/ws`），握手移交
- 验证：`test_ws_server`（起服务器→握手→收发→广播）

## P4 IM 即时通讯模块

- `include/cagent/im/im.h` / `src/im/im.c`（facade + 持久化）
  - 会话 CRUD：`ca_im_create_session/list_sessions/delete_session`
  - 消息：`ca_im_send(session, role, content)`，写入 `state/im/sessions.json`
  - 新消息触发 `CA_EV_SYSTEM` 事件 → `ca_http_server_ws_broadcast` 推送 `{"type":"im.message",...}`
- `api_rest.c` 新增：`GET/POST /v1/im/sessions`、`GET/POST /v1/im/sessions/{id}/messages`、`DELETE /v1/im/sessions/{id}`
- `cagent_ctx` 增加 `ca_im *im`，init/shutdown 接好
- 验证：`test_im`（会话+消息+持久化重载）

## P5 插件智能平面 LLM 化（自进化闭环）

- `include/cagent/plugin_intelligence/generator.h` / `src/plugin_intelligence/generator.c`
  - `ca_plugin_generate(ctx, description)` 流水线：
    1. **Analyzer**：provider!=mock → LLM 生成 `{name, description, capabilities[], steps[]}`；mock → 关键词回退
    2. **Architect**：LLM 生成能力描述/依赖
    3. **Codegen**：生成 shell 脚本（能力实现体）
    4. **Security**：`ca_security_review` + `ca_sandbox_forbidden` 扫描
    5. **Register**：写入插件注册中心（签名=hash） + 注册为 skill（可直接 `skills/run` 执行）
    6. 返回 `{plugin, script}` JSON
- `api_rest.c`：`POST /v1/plugins/generate {description}`
- 验证：`test_plugin_generate`（mock 模式确定性生成→注册→skill 可执行）；真实 LLM 手动复测

## P6 Wasm 沙箱运行器

- `third_party/wasm3/`：内嵌 wasm3 单文件（wasm3.c / wasm3.h）
- `src/plugin_runtime/wasm_runner.c`：`#pragma clang diagnostic ignored "-Weverything"` 包裹 include，实现 `ca_sandbox_wasm_fn`
  - `ca_sandbox_run_wasm(bytes, len, "add", "{\"a\":2,\"b\":40}")` → `{"result":42}`
  - 参数经 args_json 解析，调用导出函数，返回值 JSON
- `sandbox.c`：启动时注册 runner（`ca_sandbox_set_wasm_runner`）
- 验证：`test_sandbox_wasm`（手编最小 wasm 模块 `add(i32,i32)->i32`）

## P7 Console 按设计文档重分组

- `apps/web/index.html` 重写：
  - 侧栏：Dashboard / Tasks / Agents / Models / MCP / Skills / Plugins / Memory / Execution / Monitor / IM / Cluster / Metrics
  - Plugins：Installed（`/v1/plugins`）+ Create Plugin（描述→`/v1/plugins/generate`→展示脚本+注册结果）
  - IM：会话列表 + 聊天线程 + 输入框（`/v1/im/*`）+ WebSocket 实时收新消息
  - Dashboard：系统卡片（agent/内存/插件/指标统计）
  - Agents / Execution(Snapshots) / Monitor(Trace) 独立面板
  - WebSocket 客户端（`ws://host/ws`）：事件实时 toast / IM 消息实时上屏
- 重新生成 `include/cagent/api/web_ui.h`，重建 `dist`
- 验证：起 `serve` 后逐端点 smoke test

## P8 回归与打包

- `zig cc` 全量编译 + `cagent-test`（含新增测试）全绿
- 真实 LLM 基准复测（DeepSeek anthropic 端点，ToolBench+AgentBench 9 任务）
- `python tools/gen_web_ui.py` + 重建 `build/cagent` + WebView2 壳 + `dist/c-agent-setup.exe`
