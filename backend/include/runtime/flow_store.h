/* flow_store.h — persistent registry of multi-agent collaboration tasks.
 * Each Flow DAG run (POST /v1/flows) is recorded with its task id, name,
 * original input and DAG definition so runs can be listed, cancelled,
 * modified and resumed — including recovery after a server restart
 * (records left RUNNING are marked INTERRUPTED on load). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flow_store flow_store;

typedef struct flow_record {
    int64_t id;         /* scheduler task id of the latest run */
    char *name;         /* optional label ("" = none) */
    char *input;        /* original task text ("" = none) */
    char *dag_json;     /* Flow DAG definition (serialized object) */
    char *status;       /* RUNNING|DONE|FAILED|CANCELLED|TIMEOUT|INTERRUPTED */
    long long created_ms;
    long long updated_ms;
} flow_record;

flow_store *flow_store_new(void);
void flow_store_free(flow_store *fs);

/* Bind the persistence file (<state_root>/flows.json) and load records from
 * it; entries still RUNNING are marked INTERRUPTED (process died mid-run).
 * Returns the number of restored records, -1 on error. */
int flow_store_init(flow_store *fs, const char *state_root);

/* Register a new run. Returns 0 ok, -1 on bad args. */
int flow_store_add(flow_store *fs, int64_t id, const char *name, const char *input, const char *dag_json);

/* Update the terminal status of a run (no-op if unknown id). */
void flow_store_mark(flow_store *fs, int64_t id, const char *status);

/* Edit name/input/dag of a run (NULL = keep). Refused (-1) while RUNNING;
 * the id keeps pointing at the original task. */
int flow_store_modify(flow_store *fs, int64_t id, const char *name, const char *input, const char *dag_json);

/* Find a record by task id (borrowed). NULL when unknown. */
const flow_record *flow_store_find(flow_store *fs, int64_t id);

size_t flow_store_count(flow_store *fs);
const flow_record *flow_store_at(flow_store *fs, size_t i);

/* JSON array, newest first (caller frees). */
char *flow_store_json(flow_store *fs);

#ifdef __cplusplus
}
#endif
