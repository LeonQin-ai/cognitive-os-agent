# Cognitive OS Architecture Baseline v1.0

> C-native Runtime for Autonomous AI Agents
>
> **LLM is the accelerator, not the OS.**

**Status:** Architecture Baseline / Proposal

**Scope:** Personal Edition + Enterprise Edition, single-node first with distributed evolution path

**Primary implementation:** C11; use Rust only where it materially improves safety or protocol/runtime isolation

---

## 1. Executive Summary

Cognitive OS is designed as a systems-oriented runtime for autonomous AI agents. The core idea is to move deterministic control, lifecycle, scheduling, memory management, context management, execution safety, isolation and observability out of the LLM prompt loop and into an explicit runtime.

The architecture is intentionally different from a conventional Agent Framework. A conventional coding agent typically centers on a model-driven loop:

```text
Prompt -> LLM -> Tool -> Observation -> LLM -> Tool -> ...
```

Cognitive OS centers on a runtime-driven loop:

```text
Event
  -> State Machine
  -> Scheduler / Coroutine
  -> Recall
  -> Context MMU
  -> Reasoning
  -> Plan
  -> Policy / Capability
  -> Transaction / Snapshot
  -> Execute
  -> Verify
  -> Commit / Rollback
  -> Reflect
  -> Consolidate Memory
```

The LLM remains replaceable and is accessed through an LLM Bridge. The system is designed to support local models, cloud models and remote GPU inference without coupling the Agent Runtime to a specific model vendor or inference engine.

The long-term architecture supports:

- Personal single-node runtime
- Multi-Agent execution and isolation
- Local / Sandbox / VM / WSL / Remote execution
- Local / cloud / remote GPU inference
- Persistent cognitive memory
- RAG and external knowledge
- AI-generated plugins and skills
- Distributed nodes and enterprise control plane
- Snapshot-based recovery and replay

---

# 2. Design Goals

## 2.1 Primary goals

### Deterministic runtime control

The runtime, rather than the LLM, owns state transitions, retries, timeouts, resource limits, authorization, transaction boundaries and failure recovery.

### Model independence

Agents should not depend on a specific model provider, model family or inference runtime.

### Explicit memory hierarchy

Memory is modeled as a lifecycle and semantic hierarchy, not as a single vector database.

### Context as a managed resource

Context is treated as a bounded working set with explicit budgets, progressive loading and eviction/promotion.

### Safe real-world execution

Side effects must be observable, policy checked and recoverable through runtime-native transactions and snapshots.

### Multi-Agent isolation

Agents must have separate state, memory, context, capabilities and execution environments.

### Extensibility

LLM providers, memory stores, RAG strategies, tools, MCP servers, Skills and execution backends must be replaceable through stable interfaces.

### Distributed evolution

The single-node runtime must be able to evolve into a distributed Agent runtime without redesigning the core Agent model.

---

# 3. Non-Goals

The project is not intended to:

1. Train a foundation model.
2. Replace mature inference engines such as llama.cpp, vLLM or TensorRT-LLM.
3. Replace QEMU/KVM, Linux sandbox primitives or container runtimes.
4. Build a generic database from scratch when an existing storage engine is sufficient.
5. Make every component Rust-based.
6. Put all intelligence into prompting.

---

# 4. Architectural Principles

## 4.1 Mechanism over policy in the kernel

The kernel should provide mechanisms, not application-specific cognition.

```text
Kernel
  - Coroutine
  - Event
  - Task
  - IPC
  - Resource primitive
  - Plugin loading

Runtime Services
  - Agent
  - Memory
  - Context
  - Reasoning
  - Execution
```

The kernel should not contain provider-specific LLM logic or business workflows.

## 4.2 Dependency inversion

Upper layers depend on interfaces, not concrete implementations.

```text
Agent Runtime
      |
      v
 Interface / ABI
      |
 +----+----+----+
 |    |    |    |
 LLM  Tool Memory Executor
```

## 4.3 Hooks as a horizontal extension mechanism

Hooks provide lifecycle interception without forcing feature code into the core.

Examples:

```text
agent.before_run
agent.after_run
agent.on_error

state.before_transition
state.after_transition

llm.before_request
llm.after_response

memory.before_write
memory.after_recall
memory.after_consolidate

execution.before_execute
execution.after_execute
execution.on_failure

snapshot.before_commit
snapshot.after_rollback
```

Hooks may observe, enrich, reject or transform operations according to policy.

## 4.4 Coroutine + Event as the unified asynchronous model

Agent tasks, model streaming, tool execution, I/O waits, timers and inter-Agent messages should converge on one runtime model instead of mixing blocking threads, callback trees and ad-hoc futures.

```text
Event -> Scheduler -> Coroutine -> Wait -> Event -> Resume
```

## 4.5 Source of truth vs derived index

Persistent memory is human-readable and recoverable. Derived indexes are rebuildable.

```text
Markdown / canonical records
          |
          +--------------------+
          |                    |
        BM25                Vector / Graph / Metadata
```

The loss of an index must not imply loss of memory.

---

# 5. System-Level Architecture

```text
                         +----------------------+
                         | User / Application   |
                         | Web / CLI / IM / IDE  |
                         +----------+-----------+
                                    |
                           REST / WebSocket / IPC
                                    |
                         +----------v-----------+
                         |      API Gateway      |
                         +----------+-----------+
                                    |
         +--------------------------+---------------------------+
         |                                                      |
         v                                                      v
+----------------------+                              +----------------------+
| Personal Runtime     |                              | Enterprise Control   |
| Single Node          |                              | Plane                |
|                      |                              |                      |
| Cognitive Kernel     |                              | Tenant / RBAC        |
| Agent Runtime        |                              | Agent Registry       |
| Cognitive Runtime    |                              | Node Manager         |
| Memory / Context     |                              | Scheduler / Placement|
| Execution            |                              | Model / Plugin Reg.  |
+----------+-----------+                              +----------+-----------+
           |                                                     |
           |                                      +--------------+-------------+
           |                                      |                            |
           |                                      v                            v
           |                             +---------------+             +-------------+
           |                             | Cognitive     |             | Inference   |
           |                             | Nodes         |             | Cluster     |
           |                             +-------+-------+             +-------------+
           |                                     |
           +-------------------------------------+
                                                 |
                                                 v
                              +----------------------------------+
                              | Cognitive Runtime                |
                              |                                  |
                              | Agent / Task / Event / Coroutine|
                              | Cognition / Memory / Context     |
                              | LLM Bridge / Action / Execution  |
                              +----------------------------------+
```

---

# 6. Core Layering

The preferred logical stack is:

```text
Layer 0  Interfaces / API
Layer 1  Cognitive Kernel
Layer 2  Agent Runtime
Layer 3  Cognitive Runtime
Layer 4  LLM Bridge + Action Runtime
Layer 5  Execution Environment
Layer 6  OS / Hardware / Remote Infrastructure
```

The cognitive and execution sides are intentionally separated:

```text
                       Agent Runtime
                            |
               +------------+------------+
               |                         |
               v                         v
        Cognitive Runtime          Action Runtime
               |                         |
      +--------+--------+        +-------+--------+
      |        |        |        |       |        |
   Memory   Context  Reasoning  Tool  Transaction Execution
      |        |        |                |          |
      +--------+--------+                +----------+
```

---

# 7. Cognitive Kernel

## Responsibilities

- M:N coroutine scheduling
- Event loop and event dispatch
- Task primitives
- IPC/message primitives
- Base resource accounting
- Plugin loading
- Core lifecycle
- Timer / cancellation primitives

## Explicitly excluded

- Model-specific behavior
- Prompt construction
- RAG strategy
- Memory policy
- Business workflow
- Tool-specific logic

## Core objects

```c
typedef struct cognitive_task cognitive_task_t;
typedef struct cognitive_agent cognitive_agent_t;
typedef struct cognitive_event cognitive_event_t;
typedef struct cognitive_coroutine cognitive_coroutine_t;
```

---

# 8. Agent Runtime

An Agent is a long-lived runtime identity; a Task is a bounded piece of work.

```text
Agent
 |
 +-- Task
 +-- Context Namespace
 +-- Memory Namespace
 +-- Capability Set
 +-- Resource Quota
 +-- Execution Policy
```

This distinction is important for long-lived Agents, Multi-Agent coordination and future migration.

## Agent isolation domains

Each Agent may own:

- state namespace
- memory namespace
- context namespace
- capability namespace
- execution environment
- token budget
- CPU / memory / GPU quota

---

# 9. Cognitive Engine

The cognitive engine represents the semantic decision portion of the runtime.

```text
Perception
    |
Attention
    |
Recall
    |
Reasoning
    |
Planning
    |
Decision
    |
Execution
    |
Evaluation
    |
Reflection
```

Not every stage requires an LLM.

Deterministic code should handle:

- state transitions
- policy checks
- retries
- timeout handling
- scheduling
- transaction boundaries
- validation rules
- resource limits

LLM calls should be concentrated around:

- semantic interpretation
- ambiguous reasoning
- hypothesis generation
- plan generation
- reflection
- generalization

---

# 10. Memory OS

## 10.1 Memory model

Memory is split along semantic type and lifecycle dimensions.

### Lifecycle dimension

```text
L0  Sensory / Perceptual
    Short-lived signals and observations

L1  Working
    Current task state and active reasoning state

L2  Long-Term
    Persistent knowledge and experience
```

### Long-term semantic dimension

```text
Long-Term Memory
├── Semantic
├── Episodic
├── Procedural
├── Preference
├── Project
└── Entity
```

The lifecycle tiers and semantic types are orthogonal.

## 10.2 Long-term memory storage

The preferred v1 implementation is filesystem-native Markdown storage.

```text
~/.cognitive-os/
├── memory/
│   ├── user/
│   ├── agent/
│   ├── projects/
│   └── global/
├── sessions/
├── context/
└── index/
```

**Markdown is the canonical source of truth.** Indexes are derived.

Possible indexes:

- SQLite metadata index
- BM25 / keyword index
- vector index
- relation / graph index

## 10.3 Memory record

Each long-term memory record should carry:

```text
Identity
Type
Scope
Content
Importance
Confidence
Utility
Timestamp
Source / Provenance
Relations
Emotion / Salience
Lifecycle State
Version
```

Example:

```yaml
id: mem_xxxx
type: procedural
scope: agent/status
status: active
importance: 0.94
confidence: 0.96
utility: 0.91
emotion:
  valence: 0.15
  arousal: 0.72
source:
  type: experience
relations:
  derived_from: episode_xxxx
```

## 10.4 Emotion model

Emotion is a memory attribute, not a separate storage category.

Recommended minimal representation:

```text
valence
arousal
salience
labels
```

Its purpose is to affect salience, reinforcement and retrieval priority rather than directly control behavior.

---

# 11. Memory Lifecycle

The memory lifecycle is:

```text
EVENT
  |
  v
EXPERIENCE
  |
  v
REFLECTION
  |
  v
MEMORY CANDIDATE
  |
  v
SCORE + VERIFY
  |
  +----------+------------------+
  |                             |
  v                             v
REJECT / MERGE               PROMOTE
                                |
                                v
                          LONG-TERM MEMORY
                                |
                                v
                         CONSOLIDATE / DECAY
```

A raw tool call should not automatically become a long-term memory.

A candidate should be considered for promotion when it has novelty, user-level importance, repeated utility, explicit instruction, a verified successful procedure, or a meaningful failure lesson.

---

# 12. Correct vs Incorrect Experience

An Experience record represents what happened, not what the model thinks happened.

```text
Episode
 |
 +-- Actions
 +-- Observations
 +-- Test / Build Results
 +-- User Feedback
 +-- Outcome
 +-- Evidence
```

Outcome should be machine-checkable where possible:

```text
SUCCESS
FAILURE
PARTIAL_SUCCESS
UNKNOWN
```

### Evidence examples

- process exit code
- test result
- build result
- benchmark result
- rollback result
- user confirmation
- repeated validation
- external authoritative source

### Important rule

Do not promote an LLM-generated conclusion directly to trusted procedural memory without evidence.

A failure is not discarded merely because it is negative. It can become **negative procedural memory**:

```text
"Avoid method X under condition Y because test Z fails."
```

---

# 13. Experience -> Skill -> Plugin Evolution

Memory and plugins have different roles.

```text
Memory
  = What happened / What was learned

Procedure
  = How to perform a repeatable operation

Skill
  = A reusable procedure exposed to the Agent

Plugin
  = Executable capability with an interface, permissions and lifecycle
```

Recommended evolution path:

```text
Episode
   |
Reflection
   |
Lesson
   |
Procedural Memory
   |
Repeated / Verified
   |
Skill
   |
Executable / Tool-Oriented
   |
AI-generated Plugin
```

Not every experience becomes a plugin.

Plugin generation should require evidence of repeatability, stability, interface clarity and meaningful utility.

---

# 14. Context MMU

## 14.1 Purpose

Context MMU is the runtime layer that decides what memory should become the Agent's current working set.

It must be kept separate from Memory storage semantics.

```text
Memory OS
    |
    | Recall candidates
    v
Context MMU
    |
    +-- budget
    +-- ranking
    +-- loading
    +-- promotion
    +-- eviction
    +-- compression
    |
    v
LLM Context Window
```

## 14.2 Unified L0/L1/L2 representation

Do **not** maintain a second HOT/WARM/COLD taxonomy.

Use one progression:

```text
L0 = Abstract / routing summary
L1 = Overview / key facts
L2 = Full detail
```

Context MMU decides how deeply to load a record.

```text
Task
 |
 v
L0 recall
 |
 +-- irrelevant -> stop
 |
 +-- relevant -> L1
              |
              +-- enough -> use L1
              |
              +-- needs detail -> L2
```

This avoids a combinatorial `HOT/WARM/COLD x L0/L1/L2` state space.

## 14.3 Context page

```c
typedef enum {
    MEMORY_L0,
    MEMORY_L1,
    MEMORY_L2
} memory_level_t;

typedef struct {
    uint64_t id;
    char uri[256];
    memory_level_t level;
    uint32_t token_cost;
    float relevance;
    float importance;
    float confidence;
    float utility;
    uint64_t last_access;
    uint32_t access_count;
    bool pinned;
} context_page_t;
```

## 14.4 Context responsibilities

- token budget accounting
- relevant-memory selection
- progressive loading
- lazy loading
- compression
- eviction
- prefetch where justified
- prompt/context assembly
- context checkpointing

---

# 15. RAG

RAG is external knowledge acquisition, not Agent memory.

```text
Memory OS
  = user / agent / project experience

RAG
  = external knowledge / resources
```

RAG pipeline:

```text
Document
  -> Parse
  -> Chunk
  -> Metadata
  -> Embedding
  -> Index
  -> Retrieve
  -> Rerank
  -> Context Builder
  -> Context MMU
  -> LLM
```

Supported retrieval strategies can include:

- vector search
- keyword/BM25
- metadata filtering
- graph retrieval
- hybrid retrieval
- multi-query expansion
- HyDE
- reranking

A major architectural rule is to reuse the embedding and storage interfaces where practical, but keep RAG lifecycle separate from personal/agent memory lifecycle.

---

# 16. LLM Bridge

The LLM Bridge is a protocol and capability abstraction layer.

```text
Reasoning
   |
   v
LLM Bridge
   |
   +-- Model Manager
   +-- Model Router
   +-- Capability Registry
   +-- Provider Adapter
   |
   +---- Local
   +---- Cloud
   +---- Remote GPU
```

Supported families:

```text
Cloud
  OpenAI-compatible
  Anthropic-compatible
  DeepSeek / Qwen / other providers

Local
  llama.cpp
  Ollama / compatible local runtime

Remote
  vLLM
  TensorRT-LLM
  other inference clusters
```

The Agent Runtime must never hard-code one provider.

---

# 17. Action Runtime

Action Runtime translates plans into controlled side effects.

```text
Plan
 |
 v
Action
 |
 v
Capability Check
 |
 v
Policy Check
 |
 v
Transaction
 |
 v
Executor
 |
 v
Observation
```

---

# 18. Transaction + Snapshot

Runtime-side file safety uses snapshots, not Git commits as the transaction primitive.

```text
BEGIN
  |
SNAPSHOT
  |
MODIFY / EXECUTE
  |
VERIFY
  |
+------ PASS ------> COMMIT
  |
+------ FAIL ------> ROLLBACK
                           |
                         REPLAN
```

The Snapshot Engine may use:

- file-level metadata
- block-level COW
- content-addressed blocks
- delta storage
- lazy restore

Git may remain useful for repository workflows and eventual publication, but it is not the runtime transaction mechanism.

---

# 19. Execution Environment Abstraction

Agent code should not know whether it runs locally or remotely.

```text
Execution API
      |
Execution Driver
      |
+-----+--------+---------+----------+
|     |        |         |          |
Local Sandbox VM        WSL       Remote
```

### v1

Implement only Local Execution.

### Future

- Linux namespace / seccomp sandbox
- WSL
- QEMU/KVM VM
- container-backed execution
- remote execution node
- distributed workload execution

Interface example:

```c
typedef struct execution_ops {
    int  (*create)(execution_env_t *env);
    int  (*start)(execution_env_t *env);
    int  (*execute)(execution_env_t *env, action_t *action);
    int  (*stop)(execution_env_t *env);
    void (*destroy)(execution_env_t *env);
    int  (*snapshot)(execution_env_t *env, snapshot_t *snap);
    int  (*restore)(execution_env_t *env, snapshot_t *snap);
} execution_ops_t;
```

---

# 20. Multi-Agent

Multi-Agent is a runtime primitive, not only a prompt pattern.

```text
                    Supervisor
                        |
                  Task / Flow DAG
                        |
         +--------------+--------------+
         |              |              |
      Agent A        Agent B        Agent C
         |              |              |
      Memory A       Memory B       Memory C
      Context A      Context B      Context C
      Policy A       Policy B       Policy C
```

Agents communicate through messages/events rather than direct function coupling.

Required primitives:

- mailbox / message bus
- Agent namespace
- per-Agent resources
- per-Agent policy
- task cancellation
- child Agent lifecycle
- result aggregation
- failure isolation

---

# 21. Plugin / Skill / MCP Architecture

```text
Capability
├── Built-in Tool
├── Skill
├── MCP
└── Plugin
```

Plugin lifecycle:

```text
Requirement
  -> Architecture
  -> Code Generation
  -> Build
  -> Test
  -> Security Scan
  -> Sandbox Validation
  -> Package
  -> Registry
  -> Install
  -> Capability Registration
```

The generated code must never be loaded directly into a privileged runtime without verification.

Rust is a strong candidate for the plugin sandbox, MCP runtime and security boundary; the core Cognitive Kernel remains C11.

---

# 22. AI-Generated Plugin System

The plugin generator is itself an Agent capability.

```text
User requirement
      |
Capability Analyzer
      |
Plugin Architect
      |
Code Generator
      |
Builder
      |
Tests
      |
Security Reviewer
      |
Sandbox
      |
Registry
      |
Agent Installation
```

### When a Memory-derived procedure should become a Plugin

Recommended criteria:

```text
repeatable
+ verified
+ stable
+ interfaceable
+ permission-bounded
+ useful enough to automate
```

This prevents the system from turning every successful conversation into code.

---

# 23. Resource Management

Resource management should cover:

```text
CPU
Memory
GPU
Token
Storage
Network
Execution slots
```

Agent quotas should resemble OS resource controls.

Example:

```c
typedef struct {
    uint32_t cpu_limit;
    uint64_t memory_limit;
    uint32_t gpu_slots;
    uint64_t token_budget;
    uint64_t storage_limit;
} resource_quota_t;
```

---

# 24. Security Model

The LLM is never the security boundary.

```text
LLM Plan
   |
Policy
   |
Capability
   |
Hook
   |
Sandbox / Executor
   |
Audit
```

Three basic decisions:

```text
ALLOW
DENY
ASK / APPROVAL REQUIRED
```

Security must cover:

- file paths
- shell commands
- network access
- plugin permissions
- model usage
- execution environment
- resources
- secrets

---

# 25. Observability

Every autonomous operation should emit structured events.

```text
Task
 |
 +-- state transition
 +-- memory recall
 +-- context load
 +-- LLM request
 +-- tool call
 +-- policy decision
 +-- snapshot
 +-- execution
 +-- verification
 +-- commit / rollback
 +-- reflection
```

Required outputs:

- metrics
- structured logs
- trace spans
- audit records
- WebSocket event stream

---

# 26. Journal and Replay

A Cognitive Journal records enough state to reconstruct or inspect an Agent run.

Recommended entries:

```text
Task Event
State Change
Memory Change
LLM Request/Response Metadata
Tool Invocation
Tool Result
Policy Decision
Snapshot Event
Execution Result
```

Replay should not promise byte-for-byte reproduction of remote model behavior unless the relevant model output is captured. Instead, the system should distinguish:

```text
Deterministic Replay
Simulation Replay
Historical Trace Replay
```

---

# 27. Distributed Architecture

The distributed system is split into Control Plane and Data Plane.

## Control Plane

- tenant management
- Agent registry
- node registry
- model registry
- plugin registry
- global scheduling
- placement
- deployment
- cluster state
- enterprise policy

## Data Plane

Each Cognitive Node runs:

```text
Node Agent
  |
Cognitive Kernel
  |
Agent Runtime
  |
Memory / Context
  |
LLM Bridge
  |
Execution
```

This separation keeps enterprise orchestration out of the core runtime.

---

# 28. Personal vs Enterprise

Both products share the core runtime.

```text
                    Cognitive Core
                          |
             +------------+------------+
             |                         |
          Personal                 Enterprise
             |                         |
       Single Node              Control Plane
       Local-first              Multi Node
       Personal Memory          RBAC / Tenant
       Local Models             Fleet Management
```

### Personal Edition

Optimized for:

- developers
- local workstations
- single-node execution
- local-first models
- personal memory
- plugin experimentation

### Enterprise Edition

Optimized for:

- multiple deployment nodes
- RBAC / tenancy
- enterprise knowledge
- model pools
- Agent fleets
- audit and observability
- centralized policy
- deployment and upgrades

---

# 29. Frontend Architecture

## Personal Console

```text
Chat
Agents
Tasks
Memory
RAG
Models
MCP
Skills
Plugins
Terminal
Monitor
```

## Enterprise Console

```text
Dashboard
Organization
Users / RBAC
Agents
Agent Fleet
Nodes
GPU / Compute
Models
Plugins
Deployments
Memory / Knowledge
Audit
Observability
```

Recommended stack:

```text
React + TypeScript
Vite
Zustand
React Flow
ECharts
xterm.js
REST + WebSocket
```

Frontend never talks directly to internal C modules; it communicates through stable API boundaries.

---

# 30. Language Strategy

## C11

Use C for:

- Cognitive Kernel
- coroutine runtime
- scheduler
- event runtime
- task/state machine
- memory manager primitives
- snapshot primitives
- low-level OS integration
- performance-sensitive infrastructure

## Rust

Use Rust only where safety or protocol complexity clearly justifies it:

- plugin sandbox runtime
- MCP protocol/runtime
- capability/security boundary
- potentially distributed control-plane components later

## C/C++

Keep existing ecosystems where appropriate:

- QEMU
- KVM interfaces
- SPDK
- low-level Linux APIs
- hardware / virtualization integrations

Rule:

> Do not rewrite working C/C++ code in Rust without a concrete architectural benefit.

---

# 31. Recommended Repository Structure

```text
cognitive-os-agent/
├── backend/
│   ├── include/
│   │   └── cognitive-os-agent/
│   │       ├── runtime/
│   │       ├── agent/
│   │       ├── memory/
│   │       ├── context/
│   │       ├── cognition/
│   │       ├── llm/
│   │       ├── execution/
│   │       ├── plugin/
│   │       └── protocol/
│   │
│   ├── src/
│   │   ├── runtime/
│   │   │   ├── coroutine/
│   │   │   ├── event/
│   │   │   ├── scheduler/
│   │   │   ├── task/
│   │   │   └── hooks/
│   │   │
│   │   ├── agent/
│   │   │   ├── lifecycle/
│   │   │   ├── namespace/
│   │   │   ├── task/
│   │   │   ├── multi_agent/
│   │   │   └── migration/
│   │   │
│   │   ├── cognition/
│   │   │   ├── perception/
│   │   │   ├── attention/
│   │   │   ├── reasoning/
│   │   │   ├── planning/
│   │   │   ├── evaluation/
│   │   │   ├── reflection/
│   │   │   └── blackboard/
│   │   │
│   │   ├── memory/
│   │   │   ├── infrastructure/
│   │   │   ├── sensory/
│   │   │   ├── working/
│   │   │   ├── longterm/
│   │   │   │   ├── semantic/
│   │   │   │   ├── episodic/
│   │   │   │   ├── procedural/
│   │   │   │   ├── preference/
│   │   │   │   ├── project/
│   │   │   │   └── entity/
│   │   │   ├── lifecycle/
│   │   │   ├── consolidation/
│   │   │   ├── retrieval/
│   │   │   ├── storage/
│   │   │   └── provenance/
│   │   │
│   │   ├── context/
│   │   │   ├── mmu/
│   │   │   ├── page/
│   │   │   ├── budget/
│   │   │   ├── loader/
│   │   │   ├── compression/
│   │   │   └── builder/
│   │   │
│   │   ├── rag/
│   │   │   ├── document/
│   │   │   ├── parser/
│   │   │   ├── chunker/
│   │   │   ├── retrieval/
│   │   │   ├── rerank/
│   │   │   └── index/
│   │   │
│   │   ├── llm/
│   │   │   ├── bridge/
│   │   │   ├── router/
│   │   │   ├── adapters/
│   │   │   └── streaming/
│   │   │
│   │   ├── action/
│   │   │   ├── tool/
│   │   │   ├── mcp/
│   │   │   ├── skill/
│   │   │   └── capability/
│   │   │
│   │   ├── execution/
│   │   │   ├── api/
│   │   │   ├── local/
│   │   │   ├── sandbox/
│   │   │   ├── vm/
│   │   │   ├── wsl/
│   │   │   └── remote/
│   │   │
│   │   ├── transaction/
│   │   ├── snapshot/
│   │   ├── journal/
│   │   ├── replay/
│   │   ├── plugin/
│   │   ├── security/
│   │   ├── observability/
│   │   ├── protocol/
│   │   └── os/
│   │
│   ├── tests/
│   │   ├── runtime/
│   │   ├── agent/
│   │   ├── memory/
│   │   ├── context/
│   │   ├── rag/
│   │   ├── llm/
│   │   ├── execution/
│   │   └── distributed/
│   │
│   └── docs/
│
├── frontend/
│   ├── shared/
│   ├── personal/
│   └── enterprise/
│
├── control-plane/
│   ├── tenant/
│   ├── registry/
│   ├── scheduler/
│   ├── placement/
│   ├── node-manager/
│   └── policy/
│
├── rust-services/
│   ├── mcp-runtime/
│   ├── plugin-sandbox/
│   └── capability-security/
│
├── docs/
│   ├── architecture/
│   ├── memory/
│   ├── context/
│   ├── agent/
│   └── deployment/
│
└── deploy/
    ├── personal/
    └── enterprise/
```

The exact physical tree can evolve, but logical boundaries must remain stable.

---

# 32. Dependency Rules

Allowed dependency direction:

```text
API / UI
   |
   v
Application / Agent
   |
   v
Cognitive Runtime Services
   |
   v
ABI / Interfaces
   |
   +---- LLM Adapter
   +---- Memory Backend
   +---- Tool / Plugin
   +---- Execution Driver
   +---- OS Driver
```

Forbidden patterns:

```text
Kernel -> concrete LLM provider
Memory -> concrete Agent implementation
Agent -> QEMU internals
Context -> database-specific implementation
Plugin -> private Kernel data structures
```

---

# 33. Strengths of This Architecture

## 33.1 Clear system boundary

The runtime owns deterministic execution while the LLM provides semantic reasoning. This makes the architecture easier to reason about than a system in which every control decision lives inside a prompt.

## 33.2 Stronger execution safety

Snapshot + transaction + policy + capability is a stronger systems-level safety model than relying primarily on textual instructions.

## 33.3 Long-lived memory model

The separation between sensory, working and long-term memory plus semantic/episodic/procedural types supports persistent user and Agent knowledge without conflating it with RAG.

## 33.4 Efficient context use

Progressive L0/L1/L2 loading and a Context MMU provide an explicit mechanism for reducing unnecessary context transmission.

## 33.5 Model independence

The LLM Bridge avoids binding the architecture to one provider.

## 33.6 Multi-Agent as a runtime concept

Agent isolation, lifecycle, quotas and message passing are first-class concepts rather than only prompt orchestration.

## 33.7 Natural systems fit

The architecture maps well to existing systems engineering concepts:

```text
Process        -> Agent
Thread         -> Task / Coroutine
Scheduler      -> Agent Scheduler
MMU            -> Context MMU
Filesystem     -> Memory / Resource Space
Journal        -> Cognitive Journal
Snapshot       -> Transactional Snapshot
Driver         -> Model / Tool / Execution Adapter
IPC            -> Agent Message Bus
```

## 33.8 Good fit for systems-oriented engineering

The design creates a coherent bridge from Linux / virtualization / low-level runtime engineering to AI infrastructure.

---

# 34. Weaknesses and Risks

The architecture is deliberately ambitious and therefore carries real risks.

## 34.1 Over-engineering risk

A local coding assistant can work with a far simpler architecture. If the project implements every subsystem before proving one workflow, development will slow down dramatically.

**Mitigation:** build a narrow end-to-end path first:

```text
Task -> Memory Recall -> LLM -> Tool -> Snapshot -> Verify
```

## 34.2 Memory complexity

Semantic, episodic, procedural, project and preference memories can become difficult to reconcile.

**Mitigation:** keep a single canonical Memory record format and use metadata/type fields rather than independent storage code for every semantic category.

## 34.3 Incorrect self-learning

An Agent can turn a hallucinated conclusion into a bad long-term memory.

**Mitigation:** explicit evidence, verification, confidence, utility and lifecycle states.

## 34.4 Markdown scalability

Markdown is excellent as a canonical human-readable store, but a huge number of records can create filesystem and indexing overhead.

**Mitigation:** raw high-volume events go to append-only session/event logs; only consolidated memories become durable Markdown records. Derived indexes handle retrieval.

## 34.5 Context MMU complexity

If the design introduces too many competing notions of hotness, importance and memory level, the system becomes difficult to reason about.

**Mitigation:** L0/L1/L2 is the only progressive representation mechanism. Token budget, relevance and lifecycle are separate metadata/algorithms.

## 34.6 Plugin generation security

Self-generated native code is a high-risk capability.

**Mitigation:** sandbox, static checks, build isolation, signed packages, explicit capabilities, staged installation and enterprise approval.

## 34.7 Distributed consistency

Memory, Agent state, snapshots and events become much harder once multiple nodes are involved.

**Mitigation:** keep distributed state boundaries explicit; do not pretend every state needs strong global consistency. Define authoritative ownership per object.

## 34.8 C runtime implementation complexity

C provides control but increases the burden of manual memory safety, ownership and concurrency correctness.

**Mitigation:** strict ownership conventions, sanitizers, fuzzing, bounded APIs, opaque handles and selective Rust usage at trust boundaries.

## 34.9 Enterprise scope explosion

Tenant, RBAC, deployment, scheduler, model registry, observability and control-plane concerns can consume more engineering effort than the core runtime.

**Mitigation:** keep enterprise control plane out of the core; develop it only after the personal/single-node runtime demonstrates real utility.

---

# 35. Comparison With Other Agent Systems

This comparison is about architectural boundaries, not absolute product quality.

Important terminology note: the official DeepSeek Harness is open source and uses an "everything is a plugin" architecture. The official OpenAI Codex CLI is open source under Apache-2.0. Claude Code has a public GitHub repository, but the repository does not contain the actual Claude Code CLI source; it functions as a public project surface for distribution/documentation/plugins/issues. Treating Claude Code itself as an open-source implementation would be inaccurate.

## 35.1 Claude Code

Claude Code is a mature agentic coding product centered on terminal-based software engineering. Its public repository describes codebase understanding, routine task execution and Git workflows; plugins are also provided in the repository. The core CLI implementation is not actually present in that repository. [Official repository](https://github.com/anthropics/claude-code)

### Strengths

- Highly polished coding workflow
- Strong model quality through tight integration with Anthropic models
- Excellent terminal-first UX
- Mature software-engineering workflows
- Strong ecosystem and developer adoption

### Limitations relative to Cognitive OS

- The core CLI implementation is not an inspectable open-source runtime.
- It is optimized as a coding product rather than as a vendor-neutral systems substrate.
- The architecture is not intended to be a general distributed Agent OS with C-native runtime primitives.
- Deep OS-level experimentation, custom scheduling and custom memory/runtime semantics are outside the product's core value proposition.

### Cognitive OS difference

```text
Claude Code
   = mature coding Agent product

Cognitive OS
   = vendor-neutral runtime substrate
```

Cognitive OS should not try to beat Claude Code at immediate coding UX. It should expose a lower-level runtime surface that could theoretically host Claude Code-like coding Agents.

---

# 36. DeepSeek Harness

DeepSeek Harness (`dsh`) is an open-source Agent harness developed by DeepSeek AI and currently described as a developer preview. Its architecture is explicitly "everything is a plugin," powered by Cordis; the model adapter, tool registry, session log and Agent loop are implemented as replaceable plugins. [Official repository](https://github.com/deepseek-ai/deepseek-harness) [Architecture](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/architecture.md)

The project also supports subagents, including in-process and external providers, and can connect to real Codex / Claude Code children. [Subagent subsystem](https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/subagent/README.md)

### Strengths

- Excellent plugin-oriented extensibility
- Strong composability through Cordis
- Open source and easy to experiment with
- Agent loop, tools, sessions, model adapters and subagents are replaceable
- Good fit for rapidly evolving Agent behavior

### Limitations relative to Cognitive OS

- The central abstraction remains an Agent harness/plugin runtime rather than a C-native OS-like runtime.
- Its plugin architecture is powerful, but it does not by itself define the same explicit Memory OS + Context MMU + Snapshot/COW execution model proposed here.
- The project is currently in developer preview and explicitly warns about compatibility-breaking changes.
- The runtime is primarily TypeScript/Node-oriented, whereas Cognitive OS intentionally explores a lower-level C runtime boundary.

### Cognitive OS difference

```text
DeepSeek Harness
    = Everything is a Plugin

Cognitive OS
    = Everything important is a managed Runtime Resource
      + Pluginized extension points
```

DeepSeek Harness is actually one of the closest reference architectures. Cognitive OS should borrow its plugin composition ideas rather than treat it as a simple competitor.

---

# 37. OpenAI Codex

OpenAI Codex CLI is an open-source coding Agent that runs locally and is released under Apache-2.0. The current project is implemented around a Rust-based workspace and provides sandboxing, approvals, MCP support, memory/project docs, tracing and related coding-agent features. [Official repository](https://github.com/openai/codex)

### Strengths

- Very strong coding-Agent productization
- Strong coding-model integration
- Open source CLI implementation
- Mature sandbox and approval model
- MCP support
- Good local developer workflow
- Active ecosystem

### Limitations relative to Cognitive OS

- It is fundamentally optimized around coding tasks.
- Its runtime is not designed as a vendor-neutral C11 Agent OS kernel.
- The architecture is not centered on persistent filesystem-native cognitive memory, procedural memory consolidation or memory-to-plugin evolution.
- It does not aim to provide the same personal/enterprise control-plane split described here.
- Its execution model is optimized for an Agent product rather than serving as a general substrate for arbitrary Agent workloads.

### Cognitive OS difference

```text
Codex
   = highly capable open-source coding Agent

Cognitive OS
   = infrastructure layer on which coding Agents are one workload
```

Codex should be treated as an implementation and UX reference, not as something Cognitive OS must clone.

---

# 38. Comparative Matrix

| Dimension | Cognitive OS | DeepSeek Harness | OpenAI Codex CLI | Claude Code |
|---|---|---|---|---|
| Primary positioning | Agent Runtime / Cognitive OS | Agent Harness | Coding Agent | Coding Agent |
| Open-source core | Yes, intended | Yes | Yes | Core CLI not in public repo |
| C11 runtime core | **Yes** | No | No | No |
| Runtime-owned state machine | **Core design** | Yes | Yes | Yes |
| Coroutine-first runtime | **Core design** | Different composition model | Rust async/runtime | Internal / not public |
| Everything extensible via plugins | Yes, via ABI/hooks | **Core strength** | Extensions/MCP | Plugins/MCP |
| Memory OS | **Core** | Session/context oriented | Project/memory features | Project memory features |
| Sensory / Working / LT memory | **Explicit** | Not primary abstraction | Not primary abstraction | Not primary abstraction |
| Procedural memory | **Explicit** | Not primary abstraction | Not primary abstraction | Not primary abstraction |
| Markdown canonical memory | **Yes** | Not central | Project docs/memory, not same model | Project memory / files |
| L0/L1/L2 progressive memory | **Core design** | Not primary model | Context/compaction mechanisms | Context management |
| Context MMU | **Core design** | Context plugins | Strong context management, but different abstraction | Strong context management, but different abstraction |
| RAG as separate subsystem | **Yes** | Pluggable | Tools/search-oriented | Tools/MCP-oriented |
| Transactional COW Snapshot | **Core** | Not primary | Sandbox/approval, not same snapshot model | Git/worktree/sandbox oriented |
| Multi-Agent isolation | **Core future direction** | Subagents | Multi-agent/task workflows | Subagents |
| Local / Cloud / Remote LLM | **Core** | Yes | Provider/product controlled | Anthropic centered |
| Remote GPU inference abstraction | **Core design** | Possible via adapters | Not primary | Not primary |
| Local / Sandbox / VM / WSL / Remote execution | **Core execution abstraction** | Sandbox/tools | Local sandbox / WSL support | Local sandbox / terminal workflows |
| AI-generated plugin lifecycle | **Core design** | Plugin ecosystem | Extensible but not core self-generation model | Plugin ecosystem |
| Enterprise Control Plane | **Planned core edition** | Not primary | Product ecosystem/cloud direction | Team/enterprise product, not open runtime |
| Distributed Node Runtime | **Core roadmap** | Not primary | Cloud product direction | Cloud product direction |
| OS-level extensibility | **High** | Medium | Medium | Low/medium |

---

# 39. Where Cognitive OS Is Stronger

## 39.1 Runtime abstraction

The strongest differentiation is not another prompt abstraction. It is the explicit decision that Agent state, scheduling, memory, context and execution are runtime-managed resources.

## 39.2 Memory + Context as first-class infrastructure

The combination:

```text
Memory OS
   +
Context MMU
   +
Recall / Reflection
   +
Consolidation
```

provides a deeper foundation for long-lived Agents than simple conversation history.

## 39.3 Transactional execution

Snapshot-first execution creates a strong foundation for autonomous code changes and operational automation.

## 39.4 Model/vendor neutrality

The same Agent can target:

```text
Local LLM
Cloud LLM
Remote GPU cluster
```

without changing the core Agent abstraction.

## 39.5 Systems-engineering fit

The architecture naturally incorporates concepts familiar from operating systems, databases, virtualization and distributed runtimes.

## 39.6 Self-evolving capability pipeline

The chain:

```text
Experience
 -> Procedural Memory
 -> Skill
 -> Plugin
 -> New Capability
```

is a meaningful long-term differentiator if it can be made safe and evidence-driven.

---

# 40. Where Cognitive OS Is Weaker

The project should be honest about these weaknesses.

### Compared with Claude Code

Cognitive OS will initially have much weaker coding UX, model integration and polished workflow support.

### Compared with Codex

Cognitive OS will initially have weaker model-specific optimization, coding performance and product polish.

### Compared with DeepSeek Harness

DeepSeek Harness already has a mature plugin-composition model and a rapidly evolving developer ecosystem. Cognitive OS must prove that its lower-level runtime model provides measurable benefits rather than architectural novelty alone.

### General weakness

A larger architecture creates a larger implementation surface. If the runtime abstractions do not improve real tasks, the architecture becomes a liability.

---

# 41. Performance and Complexity Risks

The project should benchmark the parts that are actually on the non-LLM hot path.

Recommended benchmarks:

```text
Event dispatch latency
Coroutine switch latency
Task scheduling throughput
Memory recall latency
Context build latency
L0/L1/L2 promotion overhead
Snapshot write amplification
Rollback latency
Tool execution overhead
Plugin load/unload time
Agent message latency
```

Do not claim that C is faster simply because it is C. Measure the runtime.

The key hypothesis is:

```text
LLM inference is expensive and slow.
Runtime control is frequent and relatively cheap.

Therefore runtime efficiency and predictability matter.
```

---

# 42. V1 Development Strategy

Do not implement the entire architecture before the first useful workflow exists.

## Phase 1: Minimal runtime

```text
C11
  -> Coroutine
  -> Event Loop
  -> Task / State Machine
  -> Local Executor
```

## Phase 2: First real Agent

```text
Agent
  -> LLM Bridge
  -> Tool Runtime
  -> Working Memory
  -> Context Builder
```

## Phase 3: Differentiation

```text
Snapshot
Context MMU
Long-term Markdown Memory
Recall / Reflection
Procedural Memory
```

## Phase 4: Capability evolution

```text
Skill
Plugin Generator
Plugin Sandbox
Capability Security
```

## Phase 5: Multi-Agent

```text
Agent Namespace
IPC / Message Bus
DAG Scheduler
Resource Quotas
Isolation
```

## Phase 6: Distributed

```text
Control Plane
Node Agent
Placement
Distributed Memory
Remote Execution
Migration
Enterprise Console
```

---

# 43. Architecture Acceptance Criteria

The baseline architecture should not be considered validated by the existence of interfaces alone.

## Runtime

- [ ] Coroutine scheduler executes Agent tasks correctly.
- [ ] Event-driven wake/suspend works without blocking worker threads unnecessarily.
- [ ] State machine transitions are deterministic and observable.
- [ ] Hooks can intercept lifecycle events without core modifications.

## Memory

- [ ] Raw events do not automatically become long-term memory.
- [ ] Memory candidates carry provenance and confidence.
- [ ] Successful and failed experiences can be distinguished by evidence.
- [ ] Memory consolidation produces stable Markdown records.
- [ ] Memory indexes can be rebuilt from canonical storage.

## Context

- [ ] L0 recall can decide whether deeper loading is needed.
- [ ] L1/L2 promotion respects token budgets.
- [ ] Lazy loading prevents unnecessary context growth.
- [ ] Context building is observable.

## Execution

- [ ] File modification creates a recoverable baseline.
- [ ] Failed validation triggers rollback.
- [ ] Policy and capability checks occur before execution.
- [ ] Snapshot storage does not require Git commits.

## Agent evolution

- [ ] Reflection can generate candidate lessons.
- [ ] Verified procedures can become Skills.
- [ ] Plugin generation is isolated and auditable.

## Distributed

- [ ] Agent identity can outlive a node process.
- [ ] Node failure semantics are explicit.
- [ ] Control plane and data plane have clear ownership.

---

# 44. Key Architectural Decisions to Freeze

The following decisions should be treated as v1 baseline constraints:

1. **C11 is the primary Cognitive Kernel implementation language.**
2. **Rust is selective, primarily for security/protocol boundaries.**
3. **LLM is accessed through an LLM Bridge and is not part of the kernel.**
4. **Memory is distinct from RAG.**
5. **Markdown is canonical long-term memory storage in v1.**
6. **Indexes are derived and rebuildable.**
7. **L0/L1/L2 is the only progressive memory representation scheme.**
8. **HOT/WARM/COLD is not maintained as a second taxonomy.**
9. **Experience and evidence are separate from trusted long-term memory.**
10. **Procedural memory can evolve into Skill/Plugin only after verification/repetition.**
11. **Execution is transactional and snapshot-based.**
12. **Git is not the runtime transaction mechanism.**
13. **Multi-Agent uses isolated namespaces and message passing.**
14. **Personal and Enterprise are separate product editions over a shared core.**
15. **Distributed operation is a first-class architectural goal, but does not block single-node v1.**
16. **Core modules communicate through interfaces and hooks, not concrete implementation dependencies.**

---

# 45. Final Architectural Position

The intended architecture can be summarized as:

```text
                         Cognitive OS
                              |
                 +------------+------------+
                 |                         |
          Cognitive Runtime          Execution Runtime
                 |                         |
       +---------+---------+        +------+-------+
       |         |         |        |              |
    Memory    Context   Reasoning  Snapshot     Executor
       |         |         |        |              |
       +---------+---------+        +------+-------+
                 |                         |
                 +------------+------------+
                              |
                         Agent Runtime
                              |
                         LLM Bridge
                              |
              +---------------+---------------+
              |               |               |
            Local           Cloud         Remote GPU
                              |
                              v
                       OS / Hardware
```

The cognitive loop becomes:

```text
Perceive
   -> Recall
   -> Context MMU
   -> Reason
   -> Plan
   -> Policy
   -> Snapshot
   -> Execute
   -> Verify
   -> Commit / Rollback
   -> Reflect
   -> Consolidate
   -> Learn
```

The long-term evolution becomes:

```text
Experience
    -> Memory
    -> Procedure
    -> Skill
    -> Plugin
    -> Capability
    -> Better Future Execution
```

The platform evolution becomes:

```text
Personal Single Node
        -> Multi-Agent
        -> Sandbox / VM / Remote
        -> Distributed Nodes
        -> Enterprise Control Plane
```

The key architectural thesis is therefore:

> **Cognitive OS is not another LLM wrapper. It is an operating environment for autonomous agents, where cognition, memory, context and execution are explicit runtime resources.**

---

# 46. References

1. DeepSeek AI, **DeepSeek Harness**, official repository: https://github.com/deepseek-ai/deepseek-harness
2. DeepSeek AI, **DeepSeek Harness Architecture**: https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/architecture.md
3. DeepSeek AI, **DeepSeek Harness Subagent subsystem**: https://github.com/deepseek-ai/deepseek-harness/blob/master/packages/subagent/README.md
4. Anthropic, **Claude Code** public repository: https://github.com/anthropics/claude-code
5. OpenAI, **Codex CLI**, official repository: https://github.com/openai/codex
6. OpenAI, **Codex installation/build documentation**: https://github.com/openai/codex/blob/main/docs/install.md
7. Volcengine, **OpenViking**: https://github.com/volcengine/OpenViking
8. OpenViking concepts: L0/L1/L2 progressive context loading and filesystem-oriented context management are described in its README.

---

# Appendix A: Recommended First Implementation Slice

For a practical v1, implement only:

```text
core/
  coroutine/
  event/
  scheduler/
  task/

agent/
  lifecycle/
  task/

llm/
  bridge/
  one provider adapter

memory/
  working/
  longterm/
  markdown store/

context/
  mmu/
  l0_l1_l2/

execution/
  local/

snapshot/
  baseline/
  rollback/

action/
  file/
  shell/
```

The first end-to-end demonstration should be:

```text
User
 -> Agent Task
 -> Recall
 -> LLM Plan
 -> Snapshot
 -> File Modify
 -> Build/Test
 -> Rollback on Failure
 -> Reflection
 -> Markdown Memory
```

If this path is reliable, the rest of the architecture has a foundation. If this path is not reliable, adding more Agents, models, databases or enterprise services will only increase complexity without validating the central design.
