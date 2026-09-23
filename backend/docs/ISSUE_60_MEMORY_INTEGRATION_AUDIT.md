# Memory integration audit (issue #60)

Compared with `COGNITIVE_OS_DETAILED_DESIGN_V3.1.md` sections 6.4 and 7.
This audit checks whether the existing memory path is active; it does not claim
that every long-term Memory OS design feature is complete (see issue #65).

| Stage | Runtime path | Verification |
| --- | --- | --- |
| Construction | `init` creates the memory facade, indexes uploads, creates `memory_service_new_default`, and passes it to each reasoning lane. | Unit: `memory_service`; runtime initialization tests. |
| Retrieval | `reasoning.build_context` searches the memory facade and ranks candidate passages with `attention_select`; the global sharing switch gates cross-session retrieval. | Unit: memory search, attention, shared setting across lanes and restart. |
| Completed task write | `reasoning_run_ex` records episodic and working entries through `memory_service_remember` after a successful final answer. | Unit: service types and completed reasoning runs. |
| Unfinished task | Repeated tool failure stays failed even if a final explanatory summary is generated. Stalled/forced summaries do not enter shared memory. IM inbound messages are not promoted before a task completes. | Unit: failed-shell working-memory gate and repeated SSH failure. |
| Persistence and lifecycle | The memory facade persists records under state; optional decay/archive settings and upload reindexing are wired during initialization. | Unit: memory persistence, lifecycle, promotion and retrieval. |

The service interface is now used on the normal task write path. Retrieval
still uses the facade directly because it combines several memory kinds before
attention ranking. The broader design work for richer memory types, evidence,
provenance and distributed coordination remains tracked by issue #65.
