# Cognitive OS 架构设计 v2 — 控制面/数据面分离（企业分布式演进版）

> 本文档在两份原始设计（单机分层架构图、自进化全景架构）与 2026-08-31 的企业分布式设计纲要基础上完善，
> 与当前代码实现（`D:\AI\code\cognitive-os\backend`）逐模块对齐，给出差距清单与分阶段路线。
> 前置阅读：`docs/benchmark-report-2026-08-31.md`（评测结论直接驱动本路线图优先级）。

---

## 1. 设计目标

| 目标 | 说明 |
|---|---|
| 控制面/数据面分离 | 控制面负责编排、调度、注册与治理；数据面节点负责安全执行。可单机部署（默认），也可横向扩展 |
| 可视化流程编排 | Web 画布拖拽 DAG → 编译成可执行工作流 → 调度到节点 → trace 回传可视化 |
| 集群治理 | 节点注册/心跳/摘除、能力标签、插件与技能分发部署 |
| 插件自进化闭环 | 缺能力 → AI 生成 → 沙箱测试 → 审核入库 → 中央注册表 → 集群分发（已实现单机闭环） |
| 安全执行 | 事务 + 快照回滚 + 沙箱隔离 + policy 硬拦截（评测暴露的短板） |

## 2. 全局拓扑

```
┌───────────────────────────────────────────────────────────────────────────┐
│  USER LAYER   Web UI(Console)   IM(Telegram/…)   CLI   IDE   Open API      │
└──────────────────────────────────────┬────────────────────────────────────┘
                                       │ REST + WebSocket
┌──────────────────────────────────────▼────────────────────────────────────┐
│                     CONTROL PLANE（控制面）                                │
│                                                                           │
│  ┌─────────────┐  ┌──────────────────┐  ┌──────────────────────────────┐  │
│  │ API Gateway │  │ Visual Workflow  │  │ Cluster & Registry Manager   │  │
│  │ 鉴权/路由/   │  │ Canvas + Flow    │  │ 节点注册/心跳/摘除/能力标签/    │  │
│  │ 多租户配额   │  │ Compiler(DAG)    │  │ 插件&技能分发部署              │  │
│  └─────────────┘  └──────────────────┘  └──────────────────────────────┘  │
│  ┌────────────────────────────┐  ┌─────────────────────────────────────┐  │
│  │ Orchestrator（多agent编排） │  │ Central Plugin & Episode Registry   │  │
│  │ 分解→派发→黑板汇总→合并      │  │ 插件版本库 / 经验episode库 / 市场分发 │  │
│  └────────────────────────────┘  └─────────────────────────────────────┘  │
└──────────────────────────────────────┬────────────────────────────────────┘
                          编译产物/调度指令 │ ↑ 心跳/trace/事件
┌──────────────────────────────────────▼────────────────────────────────────┐
│                     DATA PLANE（数据面节点 = cagent 实例）                  │
│                                                                           │
│  Cognitive Kernel：scheduler / state machine(REASON→PLAN→ACT→VERIFY→LEARN)│
│                    event bus / policy engine / metrics / trace             │
│  Cognitive Services：reasoning(多轮agent循环) / memory(KV+向量+episode)     │
│                      attention / blackboard / orchestrator                 │
│  LLM Runtime：openai / anthropic 适配器、router 多模型路由、usage 计量      │
│  Action Runtime：tools(file/shell/git/mcp/…) + 事务(tx) + 快照(snapshot)   │
│  Sandbox Runtime：wasm3 / 进程沙箱 / capability 授权                       │
│  Plugin Runtime：loader(.so/wasm) + 自进化生成器(generator)                 │
└───────────────────────────────────────────────────────────────────────────┘
```

单机模式 = 控制面与数据面同进程（现状），所有控制面模块内嵌为 cagent_ctx 组件；
集群模式 = 控制面独立进程，多个数据面节点通过 HTTP + 心跳接入。

## 3. 模块映射表（设计 → 代码）

### 3.1 控制面

| 设计模块 | 现有实现 | 状态 |
|---|---|---|
| API Gateway（鉴权/路由） | `src/api/http_server.c`（单线程 HTTP+WS）、`src/api/auth.c`（bearer） | ✅ 已有（单线程是硬约束） |
| 多租户/配额 | `src/llm/router.c`（多模型路由）+ `usage.c`（token 计量） | 🟡 有路由与计量，无租户隔离 |
| Orchestrator 编排 | `src/runtime/orchestrator.c`（分解→agent 执行→黑板→合并）+ `POST /v1/orchestrate` + Web「编排」面板 | ✅ 本次落地（串行执行，WS 实时进度） |
| Visual Workflow Canvas | `apps/web/index.html`（无画布） | ❌ 缺失 |
| Flow Compiler（DAG→计划） | 无 | ❌ 缺失 |
| Cluster Manager | `src/cluster/node.c`（节点注册表雏形）+ `GET /v1/cluster` | 🟡 仅注册表，无心跳/分发 |
| Central Plugin Registry | `src/plugin_runtime/registry.c`（版本化）+ 市场合并（market） | 🟡 单机版本库，无集群分发 |
| Episode Registry | memory episode（`ca_memory_record_experience`） | 🟡 单机，无中央汇聚 |

### 3.2 数据面（节点）

| 设计模块 | 现有实现 | 状态 |
|---|---|---|
| Cognitive Scheduler | `src/runtime/scheduler.c`（优先级/超时/取消/协程 yield） | ✅ |
| State Machine | `src/runtime/state_machine.c`（REASON→…→LEARN） | ✅ |
| Event Bus | `src/runtime/event_bus.c` → WS 广播（`bus_to_ws`） | ✅ |
| Policy Engine | `src/runtime/policy_engine.c` | 🟡 **评测 policy 0/4：需执行侧硬拦截（P0）** |
| Reasoning（多轮循环） | `src/cognition/reasoning.c`（max_rounds、结果回灌、停滞检测、历史防污染约束） | ✅ |
| Memory（KV/向量/episode） | `src/memory/`（含上传 RAG、CJK bigram embedding） | ✅ |
| Graph Memory（Code Index） | `src/retrieval/`（index_search） | 🟡 词法级，无符号图 |
| LLM Runtime | `src/llm/`（openai/anthropic/mock、router、usage、stream） | ✅ |
| Action Runtime + 事务 | `src/action/` + `src/tx/` | ✅ |
| Snapshot Engine | `src/snapshot/`（块存储/COW/上限可配/大文件跳过） | ✅ |
| Sandbox Runtime | `src/plugin_runtime/sandbox.c`（wasm3）+ 进程隔离 | 🟡 wasm 全量跑通，seccomp/eBPF 类 OS 级沙箱缺 |
| 自进化生成器 | `src/plugin_intelligence/generator.c`（生成→测试→注册→持久化→重绑定） | ✅ 单机闭环 |

## 4. 关键流程

### 流程 A：可视化编排（画布 → DAG → 执行 → trace）

```
Web Canvas 拖拽节点(agent/tool/子流程)          [缺失]
   → DAG JSON（nodes+edges）                   [缺失]
   → Flow Compiler：校验环/类型，产出 plan（拓扑序步骤，每步=orchestrator 子任务
     或 tool 动作），落到 /v1/flows             [缺失]
   → 调度：按拓扑序逐 submit 到本机 scheduler 或远端节点 [部分：orchestrator 已可承载单步]
   → 每步 event bus → WS：画布实时着色          [已有事件通道]
   → trace：ca_trace span + orch/trace 黑板键   [已有]
```
落点：新增 `src/flow/`（parser/compiler/runner），`POST /v1/flows`，Canvas 前端。

### 流程 B：节点心跳与部署

```
node start → POST /v1/cluster/join {node_id, caps, url}
control plane ← 每 N 秒 POST /v1/cluster/heartbeat {load, running_tasks}
   超时 3 个周期 → 标记离线，在跑任务转移/重排
部署：POST /v1/cluster/deploy {node, plugin/skill 包} → 节点热加载（plugin loader 已支持）
```
落点：`src/cluster/node.c` 扩展心跳线程；控制面兜底节点= 本机。

### 流程 C：插件自进化闭环（已实现，纳入集群）

缺工具 → planner 报缺 → generator(分析/架构/生成/测试/安全审查) → 注册+持久化
（版本 patch 自增）→ `generated_tools.json` 重绑定 → **[集群化]** 上报中央 Registry → 各节点 deploy。

### 流程 D：多 agent 编排（已实现）

`POST /v1/orchestrate` → LLM 分解（roster 约束）→ 逐 agent 走完整推理循环
→ 黑板 `orch/*` + WS 进度 → LLM 合并答案。无注册 agent 时退化为单 agent。
**[迭代方向]**：子任务并行（需按 agent 隔离 reasoning 实例，规避共享 hist/notes 污染）、
Flow Compiler 接管"分解"产出确定性 DAG（LLM 只做兜底）。

## 5. 差距清单与路线图

### P0（正确性/安全，评测直驱）
1. **policy 硬拦截**：动作执行前过 policy_engine，规则命中→拒绝动作+原因回灌下一轮（tau-policy 0/4 的修复）。
2. **编排并行化 + agent 隔离**：每 worker 独立 ca_reasoning（共享 llm/tools/memory），黑板并发安全已具备。
3. **多租户最小集**：api key → 租户 → usage 配额（router/usage 已有底座）。

### P1（控制面成形）
4. **Flow Compiler MVP**：DAG JSON schema + 校验 + 拓扑执行（复用 orchestrator/scheduler），无 UI。
5. **节点心跳**：join/heartbeat/摘除 + 任务转移语义。
6. **插件/技能分发**：中央 Registry → node deploy（复用 plugin loader 热加载）。

### P2（体验与规模）
7. **Visual Workflow Canvas**：Web DAG 编辑器（节点=agent/工具/子流程，运行态着色）。
8. **OS 级沙箱**：Windows Job Object / Linux seccomp 桥接（现 wasm3 之外的第二档隔离）。
9. **符号级 Code Index（Graph Memory）**：c-tags/clangd 索引替换词法检索。
10. **SWE-bench mini harness**：真仓库+单测判分，接 agent 循环（评测报告路线项）。

## 6. 工程约束（实现侧已知边界）

- **单线程 HTTP**：控制面 handler 一律不阻塞，长任务走 scheduler 异步（chat/orchestrate 模式）。
- **快照保护**：大文件上限可配（UI/env），git 管理文件跳过 pre-capture。
- **历史防污染**：build_context 注入约束（历史仅参考/禁止凭历史宣称完成/结论须有本轮 [tool] 依据）。
- **编码卫生**：全链路 UTF-8 sanitize（GBK 毒化 LLM 上下文的历史教训）。

—— v2 · 2026-08-31
