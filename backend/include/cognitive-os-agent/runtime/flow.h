/* flow.h — Flow Compiler MVP: explicit DAG workflows.
 *
 * A flow is a JSON DAG: {"nodes":[{"id","agent","task"}...],
 * "edges":[{"from","to"}...]}. Task strings may reference upstream results
 * with "{{<nodeid>}}" placeholders (capped per reference).
 *
 * ca_flow_validate parses and checks the DAG (unique ids, known edges, no
 * cycles via Kahn's algorithm). ca_flow_run executes it: nodes are layered
 * topologically, each layer runs in parallel with one isolated reasoning
 * instance per node (same isolation rules as the orchestrator), results land
 * on the blackboard under "flow/<nodeid>/result", and the trace JSON goes to
 * "flow/trace". */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cagent/cagent.h"

/* Validate a DAG JSON document. On failure returns -1 and (if err != NULL)
 * sets *err to a malloc'd human-readable message. Returns 0 on success. */
int ca_flow_validate(const char *dag_json, char **err);

/* Compile + execute a validated DAG. On success returns 0, sets *answer
 * (malloc'd; sink-node results joined) and, if trace_json != NULL, *trace_json
 * to the per-node trace array. Returns -1 and sets *err on failure. */
int ca_flow_run(cagent_ctx *ctx, const char *dag_json, char **answer,
                char **trace_json);

#ifdef __cplusplus
}
#endif
