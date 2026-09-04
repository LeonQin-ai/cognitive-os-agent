# Cognitive OS 总体架构 v1.0（基线）

> 状态：**v1.0 架构基线**（2026-09-01）。本文档是后续正式开发的总纲。
> 项目定义：**Cognitive OS = Cognitive Runtime + Memory OS + Agent Runtime + Execution Runtime + Extension System + Distributed Control Plane**
> 开发优先级（总纲）：core/runtime → agent → context → memory → llm → execution → plugin，**禁止** UI 与企业管理逻辑反向绑架核心 Runtime。

---

## 一、总体架构图

```mermaid
flowchart TB
    subgraph CONSOLE["前端控制台（React + TypeScript）"]
        PC["Personal Console<br/>Chat/Agents/Memory/RAG/Skills/MCP/Plugins/Models"]
        EC["Enterprise Console<br/>Dashboard/Org/RBAC/Fleet/Nodes/Audit"]
    end

    PC --> GW
    EC --> GW
    GW["API Gateway<br/>REST / WebSocket / SSE"]

    subgraph CP["Enterprise Control Plane（可拆出，低耦合）"]
        TENANT["Tenant / RBAC"]
        AR["Agent Registry"]
        NM["Node Manager<br/>join / heartbeat / caps"]
        MSCHED["Scheduler"]
        MR["Model Registry"]
        PR["Plugin Registry"]
        DEPLOY["Deployment"]
        AUDIT["Audit / Monitor"]
    end

    GW --> CR

    subgraph CR["Cognitive Runtime（C 内核）"]
        CE["Cognitive Engine<br/>Attention/Reasoning/Planning/Decision/Evaluation"]
        AG["Agent Runtime<br/>Lifecycle/Planning/Task/Multi-Agent"]
        PLUG["Plugin Runtime<br/>MCP/Skills/Plugins/动态加载"]
        MMU["Context MMU<br/>Hot/Warm/Cold · KV State · Lazy Load · Budget"]
        HOOK["Hook System（横向）<br/>Agent/Memory/Execution/LLM/Snapshot Hooks"]
    end

    MMU --> MEM
    MMU --> RAG

    subgraph MEM["Memory OS"]
        direction TB
        M1["Sensory / Working / Episodic / Semantic / Procedural"]
        M2["Lifecycle: Encode→Recall→Consolidate→Reinforce→Decay→Forget→Archive"]
        M3["Storage: KV / Vector / Document / Graph / Object"]
    end

    subgraph RAG["RAG（与 Memory 生命周期分离）"]
        direction TB
        R1["Parser→Chunk→Embed→Index→Retrieve→Rerank"]
    end

    MEM & RAG --> BRIDGE

    subgraph BRIDGE["LLM Bridge（统一计算加速器）"]
        ROUTER["Model Router + Policy<br/>Cost/Latency/Capability/Privacy"]
        P1["Cloud: OpenAI/DeepSeek/Claude/Qwen"]
        P2["Local: llama.cpp/Ollama"]
        P3["Remote GPU: vLLM/TensorRT"]
    end

    BRIDGE --> EXEC

    subgraph EXEC["Execution Runtime"]
        E1["LocalExecutor（已实现）"]
        E2["Sandbox / VM / WSL / Remote / Cluster（接口预留）"]
        SNAP["Snapshot Manager<br/>File/Process/State · Baseline→Execute→Verify→Commit/Rollback"]
    end

    EXEC --> OS["OS Abstraction<br/>Linux / Windows / macOS"]
    CR -.注册/心跳.-> CP
```

**核心思想**：LLM 是 Cognitive OS 的**计算加速器，而不是 OS 本身**。类脑思考用 `代码流 + 规则引擎 + 状态机 + LLM` 混合实现，LLM 只在需要泛化判断时介入。

---

## 二、核心设计哲学：认知闭环（代码逻辑主环）

```mermaid
flowchart TB
    USER[USER] --> INTENT[INTENT] --> PERCEPTION[PERCEPTION<br/>context_builder]
    PERCEPTION --> ATTENTION[ATTENTION<br/>attention.c 显著性打分]
    ATTENTION --> WM[WORKING MEMORY<br/>hist ring + session notes + blackboard]
    WM --> REASON[REASONING<br/>reasoning.c 状态机]
    WM --> RAG[RETRIEVAL<br/>engine.c 向量/关键词召回]
    REASON --> PLANNER[PLANNER<br/>planner.c 工具目录+策略过滤]
    PLANNER --> AGENT[AGENT<br/>agent loop ≤N 轮]
    AGENT --> SKILL & TOOL[SKILL / TOOL<br/>tools.c file/shell/git/mcp/skill]
    SKILL & TOOL --> EXEC2[EXECUTION<br/>本地执行 + policy 硬拦截 + snapshot 事务]
    EXEC2 --> OBS[OBSERVATION<br/>结果回灌下一轮上下文]
    OBS --> EVAL[EVALUATION<br/>evaluator.c 校验]
    EVAL --> CONSOLIDATE[MEMORY CONSOLIDATION<br/>memory.c episode/semantic/procedural]
    CONSOLIDATE --> EP[EPISODIC] & SEM[SEMANTIC] & PROC[PROCEDURAL]
    PROC --> SKILLGEN[技能沉淀<br/>generator.c 自动生成 tool→skill]
    SKILLGEN --> NEWCAP[NEW CAPABILITY<br/>plugin 注册]
    NEWCAP -.增强.-> AGENT
```

对应现有代码主入口：`cagent_run()` → `ca_reasoning_run()`（src/cognition/reasoning.c，REASON→PLAN→ACT→VERIFY→LEARN 状态机，src/runtime/state_machine.c）。

---

## 三、模块依赖规则（比目录更重要）

```mermaid
flowchart TB
    UI["UI（前端）"] --> API["API（REST/WS/SSE）"] --> APP["Application"] --> AGENT["Agent"]
    AGENT --> COG["Cognitive"] & CTX["Context"]
    COG & CTX --> MEMRAG["Memory / RAG"] --> LLM["LLM Bridge"] --> EXEC3["Execution / Plugin"] --> OS2["OS"]
```

**铁律：底层绝不反向依赖上层。**
- `memory/` 不得 `#include "agent.h"`、`"llm.h"`
- 依赖倒置通过四个接口族实现：`MemoryBackend` / `LLMProvider` / `Executor` / `Plugin`
- 现有代码已遵守的示例：`ca_reasoning_config`（reasoning.h）只持有接口指针（llm/tools/memory/policy/bus 均为不透明 struct），memory 层不知道 agent 层存在。

---

## 四、Memory OS（类型与服务分离）

```mermaid
flowchart LR
    subgraph TYPES["Memory Types（语义分类）"]
        SENS[Sensory] --> WORK[Working] --> LT[Long-Term]
        LT --> EPI[Episodic] & SEM2[Semantic] & PROC2[Procedural]
    end
    subgraph SVC["Memory Service（生命周期）"]
        ENC[Encode] --> STORE[Store] --> RECALL2[Recall] --> CONS[Consolidate] --> REINF[Reinforce] --> DECAY[Decay] --> FORG[Forget] --> ARCH[Archive]
    end
    subgraph BE["Storage Backend（物理实现，可插拔）"]
        KV[KV kv.c] & VEC[Vector vector.c] & DOC[Document] & GR[Graph graph.c] & OBJ[Object]
    end
    TYPES -->|"Type ≠ Backend"| SVC --> BE
```

**禁止绑定**：`Semantic = Vector DB`、`Episodic = Document DB` 这类绑定不存在——任何 Memory Type 可以落到任何 Backend。
现有代码：`src/memory/{memory,kv,episode,vector,graph}.c`，embedding 本地哈希+CJK bigram 回退（embedding.c），cloud/本地 fallback 在 `ca_memory` 内部选择。
RAG 与 Memory **共享** Embedding/VectorStore/Reranker，但**生命周期不混**（外部知识 vs 用户/Agent 经验）。

### Memory 自进化闭环（差异化能力）

```mermaid
flowchart LR
    A[Agent] --> E[Execution] --> O[Observation] --> EV[Evaluation] --> ME[Memory Encoding] --> EPI2[Episodic] --> C[Consolidation]
    C --> SEM3[Semantic] & PROC3[Procedural/Episode]
    PROC3 --> SK[Skill] --> PG[Plugin] --> NC[New Capability] --> A2[Agent 增强]
```

现状：自进化闭环已有骨架（失败→generator.c 生成工具→注册→generated_tools.json 持久化→启动回绑）；巩固（Consolidation，episodic→semantic/procedural 的自动沉淀）是下一步重点。

---

## 五、Context MMU

```mermaid
flowchart TB
    subgraph TIER["Context 层级"]
        HOT["Hot：当前任务/本轮对话<br/>hist ring（reasoning 内）"]
        WARM["Warm：最近经验/会话笔记<br/>sn_* session notes、blackboard"]
        COLD["Cold：长期知识<br/>long-term memory、RAG 索引、uploads"]
    end
    TIER --> LAZY["Lazy Loading（按需召回）"] --> BUDGET["Context Budget（预算控制）"] --> LLM2["LLM"]
    MMU2["Context MMU 同时维护"] --- KVST["KV State / Task State / Agent State / Memory Index / Tool State"]
```

目的：**LLM 不需要每次重读全部历史**。现有部分实现：hist ring + compact_history（压缩）、attention 显著性、RAG 召回、chat history JSON；✅ 显式 budget 配额与跨层自动降级已于 2026-09-02 落地（`context.budget_*`）。

---

## 六、Agent Runtime 与生命周期

```mermaid
stateDiagram-v2
    [*] --> CREATE
    CREATE --> INIT
    INIT --> READY
    READY --> RUNNING
    RUNNING --> WAITING
    WAITING --> RUNNING
    RUNNING --> SUSPENDED
    SUSPENDED --> RUNNING: RESUME
    RUNNING --> COMPLETED
    COMPLETED --> [*]
```

Agent 组成：Identity / State / Policy / Memory / Context / Skills / Tools / Permissions / Scheduler / Lifecycle。
Multi-Agent 默认隔离五项：Namespace、Memory、Context、Tool、Permission（现有实现：每 agent 独立 `ca_reasoning` 实例 = 历史/会话笔记隔离；共享 llm/tools/memory/policy/bus/metrics 均有锁；无锁的 index/snapshot 明确不共享）。

```mermaid
flowchart TB
    SUP["Agent Supervisor（orchestrator.c）"] --> A1["Agent A"] & A2["Agent B"] & A3["Agent C"]
    A1 & A2 & A3 --- ISO["各自隔离的 reasoning 实例<br/>共享黑板 blackboard + agent pool"]
```

三种多 agent 形态（已收敛为一套引擎）：
| 形态 | 计划由谁定 | 执行引擎 |
|---|---|---|
| 单 agent（对话） | LLM 自主 | reasoning loop |
| 自动编排（orchestrate） | LLM 分解 → DAG | ca_flow_run |
| 手写 Flow | 用户显式 DAG | ca_flow_run |

---

## 七、Hook 系统（横向能力，第三方零侵入扩展）

```mermaid
flowchart LR
    subgraph SOURCES["触发点（Runtime 内置）"]
        AH["Agent Hook<br/>before_run/after_run/on_error/on_state_change"]
        MH["Memory Hook<br/>before_store/after_recall/on_consolidate/on_forget"]
        EH["Execution Hook<br/>before_execute/after_execute/on_failure/on_rollback"]
        LH["LLM Hook"] & PH["Plugin Hook"] & RH["RAG Hook"] & SH["Snapshot Hook"] & SECH["Security Hook"] & SCH["Scheduler Hook"]
    end
    SOURCES --> BUS2["ca_hook_dispatch(event, payload_json)"]
    BUS2 --> REG["Hook Registry（按事件名注册回调链）"]
    REG --> H1["Builtin: audit 落盘"]
    REG --> H2["Builtin: metrics 计数"]
    REG --> H3["第三方回调（不改核心代码）"]
```

现有实现：`src/runtime/hook.c`（注册/派发/拦截语义）+ reasoning、tool 执行接入点；REST `GET/POST/DELETE /v1/hooks` 供外部（含插件）注册。

---

## 八、Coroutine 统一异步模型

```mermaid
flowchart TB
    EL["Event Loop"] --> SCHED["Scheduler（scheduler.c）"]
    SCHED --> C1["Agent Task 协程"] & C2["LLM Request"] & C3["Tool Call"] & C4["MCP Request"] & C5["File/Net IO"] & C6["Memory Retrieval"]
    C1 & C2 & C3 & C4 & C5 & C6 --> IO["I/O / Event 完成点（ca_scheduler_yield 让出）"]
```

原则：**异步一律协程，不再新增 pthread_create 长驻线程**（例外：与外部世界的边界——HTTP server、WS、channel poller、cluster heartbeat）。
现有实现：`src/os/os_coro.c` + scheduler 合作式调度；HTTP 路由内严禁阻塞，长任务走 `ca_scheduler_submit`（chat/orchestrate/flow 均此模式）。

---

## 九、Execution Runtime 与 Snapshot

```c
/* include/cagent/execution/executor.h —— 稳定接口，未来 VM/Sandbox 不改 Agent Runtime */
typedef struct ca_executor_ops {
    int  (*create)(void *impl, const char *config_json);
    int  (*start)(void *impl);
    int  (*execute)(void *impl, const char *tool, const char *args_json, char **result);
    int  (*stop)(void *impl);
    void (*destroy)(void *impl);
    int  (*snapshot)(void *impl, char **snapshot_id);
    int  (*restore)(void *impl, const char *snapshot_id);
} ca_executor_ops;
```

现状：`LocalExecutor` 已落地（execution/executor.c，实现 `ca_executor_ops`，reasoning 的 ACT 阶段经它调 tool registry）；`SandboxExecutor`（wasm3）已有独立沙箱（plugin_runtime/sandbox）。沙箱已内置 **FileTracker 文件访问追踪**（plugin_runtime/filetracker）：配置 workspace 后，每次沙箱运行记录命令读取的路径 token（READ）、前后快照对比出的新建/改写（WRITE）与删除（DELETE），以 `files_json` 随结果返回——支撑"这个插件动了哪些文件"审计与后续自动回滚。

Snapshot 哲学：**Runtime 原生能力，不是 git**。Baseline→Execute→Verify→Commit/Rollback；现有 `snapshot/`+`tx/`（use_transaction 时 ACT 失败自动回滚）。

---

## 十、代码逻辑图：三条关键时序

### 10.1 单 agent 主循环（cagent_run）

```mermaid
sequenceDiagram
    participant FE as 前端(对话)
    participant API as api_rest (POST /v1/chat)
    participant SCH as scheduler(协程)
    participant RE as reasoning(状态机)
    participant PL as planner
    participant TL as tools(policy拦截)
    participant MM as memory/blackboard
    participant LL as llm bridge

    FE->>API: {message}
    API->>SCH: submit(userdata=0)
    SCH->>RE: ca_reasoning_run(prompt)
    RE->>MM: build_context(历史+notes+RAG+重要约束)
    RE->>PL: plan(工具目录, deny已隐藏)
    PL->>LL: chat(plan prompt)
    LL-->>PL: JSON 动作数组
    loop 每个动作 (agent loop ≤8轮)
        RE->>RE: ca_policy_check → DENY? 跳过(非致命)
        RE->>TL: ca_tool_execute
        TL-->>RE: 结果/失败(事务回滚判定)
        RE->>MM: 结果写 hist/notes/metrics
    end
    RE->>LL: verify(评估)
    RE->>MM: LEARN 巩固
    RE-->>SCH: answer
    SCH-->>API: task.output
    API-->>FE: 轮询/WS 事件
```

### 10.2 Flow 编译执行（自动编排与手写 DAG 同路径）

```mermaid
sequenceDiagram
    participant FE as 前端(Flow)
    participant API as api_rest
    participant OR as orchestrator
    participant LL as llm bridge
    participant FL as flow(Kahn分层)
    participant WI as 每节点 worker 线程
    participant BB as blackboard

    FE->>API: POST /v1/flows/decompose {task}
    API->>OR: cagent_flow_decompose
    OR->>LL: 分解(可用agent roster)
    LL-->>OR: [{agent,task}...]
    OR-->>FE: {dag:{nodes,edges}}  ← 可修改
    FE->>API: POST /v1/flows {dag}
    API->>API: ca_flow_validate(判环/重复id/未知agent)
    API->>FL: scheduler submit(marker=2) → ca_flow_run
    loop 每个拓扑层
        FL->>WI: 层内并行(隔离reasoning实例, {{上游}}替换)
        WI->>BB: flow/<id>/result + result:<agent> + WS 事件
    end
    FL-->>BB: flow/trace, 综合答案(编排模式再走LLM merge)
```

### 10.3 分布式节点生命周期（企业版雏形）

```mermaid
sequenceDiagram
    participant N as Node(worker)
    participant CP as Control Plane(本机 coordinator)
    N->>CP: POST /v1/cluster/join {id,host,port,role,caps}
    loop 每 5s
        N->>CP: POST /v1/cluster/heartbeat {id}
        CP->>CP: 3 个周期未到 → mark_down
    end
    N->>CP: DELETE /v1/cluster/nodes/<id> (leave)
```

---

## 十一、设计模块 → 现有代码映射（代码逻辑图·静态视图）

| 设计模块 | 现有实现 | 状态 |
|---|---|---|
| core/runtime（event loop/scheduler/lifecycle） | src/runtime/scheduler.c, state_machine.c, task.c, os/os_coro.c | ✅ |
| event bus | src/runtime/event_bus.c → WS 广播 | ✅ |
| hook 系统 | src/runtime/hook.c（v1.0 新增） | ✅ 本轮 |
| cognitive engine（attention/reasoning/planning/evaluation） | src/cognition/{attention,reasoning,planner,evaluator}.c | ✅（reflection 部分=自进化） |
| agent runtime（lifecycle/multi-agent） | src/cagent.c(agent_run/run), runtime/{agent,orchestrator,flow}.c | ✅ |
| context MMU | hist ring + compact + session notes + attention + retrieval | ✅（budget 显式配额+自动降级 2026-09-02；Context 状态槽 KV/Task/Agent State 收拢到 runtime/state_store.c + REST /v1/state 2026-09-04） |
| memory OS | src/memory/{memory,kv,episode,vector,graph}.c | ✅（lifecycle reinforce/decay/forget/archive + consolidation 自动化 ✅ 2026-09-04） |
| memory service 接口 | include/cagent/memory/service.h + src/memory/service.c（type↔backend 解耦：working/episodic/semantic/procedural 四类型 vtable；default backend = ca_memory facade；REST GET /v1/memory/service） | ✅ 2026-09-04 |
| RAG | src/retrieval/{engine,context_builder,embedding}.c | ✅ 两阶段检索（hybrid 召回→rerank 重排→混合排序）、MQE、HyDE（retrieval.hyde 配置，默认关）✅ 2026-09-04 |
| llm bridge + router | src/llm/{llm,router,openai,anthropic,mock,sse,usage}.c | ✅（caps 查询 ca_llm_capabilities + cancel 中断 ✅ 2026-09-04；policy 路由 🟡） |
| capability（tool/skill/mcp/plugin） | src/action/tools.c + tool_*.c, skill.c, mcp_conn.c, plugin_runtime/* | ✅ |
| 自进化 generator | src/plugin_intelligence/generator.c + generated_tools.json | ✅ |
| execution runtime | execution/executor.c LocalExecutor + WSL/Remote 路由执行器（shell 经 `wsl.exe`/`ssh` 重写，非 shell 工具透传；config `execution.backend`/`execution.remote_host`） | ✅（ExecutorOps 收拢完成；VM 沙箱已有 wasm3 sandbox） |
| snapshot/tx | src/snapshot, src/tx | ✅ |
| process/state snapshot | ca_cagent_state_export/import（cagent.c：state store + long-term facts + agent roster + llm config 打包为单 JSON）；REST POST /v1/state/snapshot、/v1/state/restore | ✅ 2026-09-04 |
| policy/权限 | src/runtime/policy_engine.c（规划隐藏+执行硬拦截+持久化） | ✅ |
| cluster/心跳 | src/cluster/node.c + cagent.c heartbeat_loop | ✅ |
| protocol（HTTP/WS） | src/api/{http_server,websocket,api_rest}.c + web_ui.h | ✅ |
| 前端 | apps/web/index.html（单文件 React 风格，构建期嵌入） | 🟡 个人版功能齐全；React 拆分 ⬜ |
| enterprise control plane | 多租户/RBAC/部署 ⬜（cluster 心跳为最早雏形） | ⬜ |

---

## 十二、开发路线（依总纲优先级）

1. **core/runtime**：✅ 协程调度器/事件总线/状态机已稳；✅ Hook 横向层已落地（src/runtime/hook.c：注册/派发/拦截语义 + agent.*/exec.* 事件接入 reasoning 与状态机 + 内置审计 hooks.jsonl + REST GET/POST/DELETE /v1/hooks）
2. **agent**：✅ 生命周期 + 隔离多 agent + Flow 统一引擎；待做：agent 持久化身份/权限（企业 RBAC 前置）
3. **context**：✅ Context Budget 显式配额（hot/warm/cold 字节预算 + 自动降级，2026-09-02）；✅ Context 状态槽收拢（runtime/state_store.c：KV/Task/Agent State 统一存储 + /v1/state REST + state.json 持久化，2026-09-04）
4. **memory**：✅ Consolidation 自动化（2026-09-02）；✅ lifecycle（reinforce/decay/forget/archive，2026-09-04）；✅ Memory Service 接口层（type↔backend 解耦 vtable，2026-09-04）
5. **llm**：✅ 路由策略（cost/latency/capability，2026-09-02）；✅ caps 查询 + cancel（2026-09-04）
6. **execution**：✅ tool 执行收拢到 `ca_executor_ops`（Local，2026-09-02）；✅ WSL/Remote 路由执行器 + 进程/状态快照（2026-09-04）
7. **plugin**：✅ 生成/注册/持久化闭环；待做：分发/签名/灰度（企业）

**P2（暂缓）**：可视化画布、OS 级沙箱、符号代码索引、SWE-bench harness、React 前端拆分。（MQE/HyDE/Rerank 已于 2026-09-04 完成，移出 P2）
