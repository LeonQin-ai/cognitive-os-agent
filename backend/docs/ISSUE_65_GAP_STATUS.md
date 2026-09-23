# Design-gap report status (issue #65)

The issue is an architecture audit, not a single failing test. The following
rechecks its claims against the current code and distinguishes corrected
behavior from remaining design work.

| Report item | Current status |
| --- | --- |
| Planner tool catalog | Runtime prompts enumerate registered, policy-allowed tools. The older static prompt remains a fallback when no registry is provided. |
| Plugin architect | The generator requests an LLM architecture after a deterministic `architect_design` fallback. The standalone architect API remains heuristic. |
| Attention | Candidate ranking now blends lexical matching with deterministic local vector similarity, including CJK bigrams. This is a lightweight reranker, not a learned attention model. |
| WASM text/linear memory | Numeric arguments remain the supported path; typed text/host memory ABI needs a separate design and tests. |
| Policy risk analysis | Keyword checks remain; a parsed command policy is still needed. |
| Flow size | The compiler accepts up to 64 nodes. DAG storage is heap allocated and each layer runs at most eight worker threads at once. The per-node task template remains limited to 8192 bytes; arbitrarily large graphs still need a dynamic representation. |
| Context MMU | Existing token budgeting and hot/warm/cold context logic works, but full page eviction and prefetch are not implemented. |
| Cluster | Node registry/heartbeat exists; distributed task ownership and failure recovery are not implemented. |
| Embedding | A deterministic local 256-dimensional hashing provider exists, so the report's “external service only” concern is outdated. Learned local model support remains an enhancement. |
| IM | Channel routing exists; presence and richer delivery guarantees remain outside the current implementation. |
| Snapshot size | The 64 MB default is configurable; incremental capture for very large files is still open. |
| Model catalog | The bundled quick-start presets are static, while `catalog_provider_models_json` already fetches the configured provider's live `/models` endpoint. User-defined presets still lack file-backed catalog loading. |

The report also mentions logging and sandbox hardening. Those require focused
threat models and regression tests before claiming completion. Issue #65 stays
open as the parent design-gap tracker; issue #63 tracks the separate measured
million-agent capacity work.
