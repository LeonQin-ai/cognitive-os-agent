/* flow.h — Flow Compiler MVP: explicit DAG workflows.
 *
 * A flow is a JSON DAG: {"nodes":[{"id","agent","task"}...],
 * "edges":[{"from","to"}...]}. Task strings may reference upstream results
 * with "{{<nodeid>}}" placeholders (capped per reference).
 *
 * flow_validate parses and checks the DAG (unique ids, known edges, no
 * cycles via Kahn's algorithm). flow_run executes it: nodes are layered
 * topologically, each layer runs in parallel with one isolated reasoning
 * instance per node (same isolation rules as the orchestrator), results land
 * on the blackboard under "flow/<nodeid>/result", and the trace JSON goes to
 * "flow/trace". */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cognitive-os-agent.h"

/* Validate a DAG JSON document. On failure returns -1 and (if err != NULL)
 * sets *err to a malloc'd human-readable message. Returns 0 on success. */
int flow_validate(const char *dag_json, char **err);

/* Compile + execute a validated DAG. On success returns 0, sets *answer
 * (malloc'd; sink-node results joined) and, if trace_json != NULL, *trace_json
 * to the per-node trace array. Returns -1 and sets *err on failure.
 * task_id: scheduler task id owning this run (used as the progress-registry
 * key so /v1/tasks/<id> can report per-node status); pass -1 when the run is
 * not bound to a scheduler task (tests, orchestrator-internal flows). */
int flow_run(runtime_ctx *ctx, const char *dag_json, int64_t task_id, char **answer, char **trace_json);

/* Per-node progress for the flow run owned by scheduler task <task_id>.
 * Returns a malloc'd JSON array
 *   [{"id","agent","layer","status":"queued|running|ok|error","start_ms","end_ms"}...]
 * or NULL when no run is registered under that id. Caller frees. */
char *flow_progress_json(int64_t task_id);

#ifdef __cplusplus
}
#endif
