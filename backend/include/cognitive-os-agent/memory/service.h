/* service.h — Memory Service interface (architecture v1.0 §6).
 *
 * Decouples the memory TYPES the cognitive layer thinks in (working /
 * episodic / semantic / procedural) from the BACKEND that implements them
 * (the default memory facade today; a remote memory service, SQLite or
 * a cluster-shared store tomorrow). Consumers code against the interface;
 * backends plug in behind memory_service_ops. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory types (architecture v1.0: working / episodic / semantic / procedural). */
typedef enum {
    MEM_WORKING = 0, /* short-term scratchpad, newest-first ring */
    MEM_EPISODIC,    /* task->result experiences */
    MEM_SEMANTIC,    /* long-term facts (key -> text) */
    MEM_PROCEDURAL   /* how-to facts ("procedure.*" keys) */
} mem_type;

#define MEM_TYPE_COUNT 4

const char *mem_type_name(mem_type t);            /* "working" ... */
int mem_type_parse(const char *s, mem_type *out); /* 0 ok, -1 unknown */

typedef struct memory_service memory_service;

/* Backend vtable. All functions return 0 ok, -1 unsupported/error.
 * Text outputs are malloc'd (caller frees). impl is the backend's state. */
typedef struct memory_service_ops {
    const char *name; /* backend name, e.g. "default" */
    int (*remember)(void *impl, mem_type t, const char *key, const char *text);
    int (*forget)(void *impl, mem_type t, const char *key);
    int (*recall_key)(void *impl, mem_type t, const char *key, char **text);
    /* relevance-ordered recall; JSON array of {kind,text,score,...} */
    int (*recall_query)(void *impl, mem_type t, const char *query, int k, char **json);
    /* stats object covering all types, e.g. [{"type":"working","count":3},...] */
    int (*stats)(void *impl, char **json);
    void (*destroy)(void *impl);
} memory_service_ops;

memory_service *memory_service_new(const memory_service_ops *ops, void *impl);
void memory_service_free(memory_service *ms);
const char *memory_service_backend(const memory_service *ms);

int memory_service_remember(memory_service *ms, mem_type t, const char *key, const char *text);
int memory_service_forget(memory_service *ms, mem_type t, const char *key);
int memory_service_recall_key(memory_service *ms, mem_type t, const char *key, char **text);
int memory_service_recall_query(memory_service *ms, mem_type t, const char *query, int k, char **json);
int memory_service_stats(memory_service *ms, char **json); /* array of 4 */

/* Default backend: delegates to the existing memory facade (borrowed). */
struct memory;
memory_service *memory_service_new_default(struct memory *m);

#ifdef __cplusplus
}
#endif
