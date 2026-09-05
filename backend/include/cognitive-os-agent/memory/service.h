/* service.h — Memory Service interface (architecture v1.0 §6).
 *
 * Decouples the memory TYPES the cognitive layer thinks in (working /
 * episodic / semantic / procedural) from the BACKEND that implements them
 * (the default coa_memory facade today; a remote memory service, SQLite or
 * a cluster-shared store tomorrow). Consumers code against the interface;
 * backends plug in behind coa_memory_service_ops. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory types (architecture v1.0: working / episodic / semantic / procedural). */
typedef enum {
    COA_MEM_WORKING = 0,   /* short-term scratchpad, newest-first ring */
    COA_MEM_EPISODIC,      /* task->result experiences */
    COA_MEM_SEMANTIC,      /* long-term facts (key -> text) */
    COA_MEM_PROCEDURAL     /* how-to facts ("procedure.*" keys) */
} coa_mem_type;

#define COA_MEM_TYPE_COUNT 4

const char *coa_mem_type_name(coa_mem_type t);           /* "working" ... */
int coa_mem_type_parse(const char *s, coa_mem_type *out); /* 0 ok, -1 unknown */

typedef struct coa_memory_service coa_memory_service;

/* Backend vtable. All functions return 0 ok, -1 unsupported/error.
 * Text outputs are malloc'd (caller frees). impl is the backend's state. */
typedef struct coa_memory_service_ops {
    const char *name; /* backend name, e.g. "default" */
    int (*remember)(void *impl, coa_mem_type t, const char *key, const char *text);
    int (*forget)(void *impl, coa_mem_type t, const char *key);
    int (*recall_key)(void *impl, coa_mem_type t, const char *key, char **text);
    /* relevance-ordered recall; JSON array of {kind,text,score,...} */
    int (*recall_query)(void *impl, coa_mem_type t, const char *query, int k,
                        char **json);
    /* stats object covering all types, e.g. [{"type":"working","count":3},...] */
    int (*stats)(void *impl, char **json);
    void (*destroy)(void *impl);
} coa_memory_service_ops;

coa_memory_service *coa_memory_service_new(const coa_memory_service_ops *ops, void *impl);
void coa_memory_service_free(coa_memory_service *ms);
const char *coa_memory_service_backend(const coa_memory_service *ms);

int coa_memory_service_remember(coa_memory_service *ms, coa_mem_type t,
                               const char *key, const char *text);
int coa_memory_service_forget(coa_memory_service *ms, coa_mem_type t, const char *key);
int coa_memory_service_recall_key(coa_memory_service *ms, coa_mem_type t,
                                 const char *key, char **text);
int coa_memory_service_recall_query(coa_memory_service *ms, coa_mem_type t,
                                   const char *query, int k, char **json);
int coa_memory_service_stats(coa_memory_service *ms, char **json); /* array of 4 */

/* Default backend: delegates to the existing coa_memory facade (borrowed). */
struct coa_memory;
coa_memory_service *coa_memory_service_new_default(struct coa_memory *m);

#ifdef __cplusplus
}
#endif
