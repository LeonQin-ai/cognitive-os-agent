/* service.h — Memory Service interface (architecture v1.0 §6).
 *
 * Decouples the memory TYPES the cognitive layer thinks in (working /
 * episodic / semantic / procedural) from the BACKEND that implements them
 * (the default ca_memory facade today; a remote memory service, SQLite or
 * a cluster-shared store tomorrow). Consumers code against the interface;
 * backends plug in behind ca_memory_service_ops. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory types (architecture v1.0: working / episodic / semantic / procedural). */
typedef enum {
    CA_MEM_WORKING = 0,   /* short-term scratchpad, newest-first ring */
    CA_MEM_EPISODIC,      /* task->result experiences */
    CA_MEM_SEMANTIC,      /* long-term facts (key -> text) */
    CA_MEM_PROCEDURAL     /* how-to facts ("procedure.*" keys) */
} ca_mem_type;

#define CA_MEM_TYPE_COUNT 4

const char *ca_mem_type_name(ca_mem_type t);           /* "working" ... */
int ca_mem_type_parse(const char *s, ca_mem_type *out); /* 0 ok, -1 unknown */

typedef struct ca_memory_service ca_memory_service;

/* Backend vtable. All functions return 0 ok, -1 unsupported/error.
 * Text outputs are malloc'd (caller frees). impl is the backend's state. */
typedef struct ca_memory_service_ops {
    const char *name; /* backend name, e.g. "default" */
    int (*remember)(void *impl, ca_mem_type t, const char *key, const char *text);
    int (*forget)(void *impl, ca_mem_type t, const char *key);
    int (*recall_key)(void *impl, ca_mem_type t, const char *key, char **text);
    /* relevance-ordered recall; JSON array of {kind,text,score,...} */
    int (*recall_query)(void *impl, ca_mem_type t, const char *query, int k,
                        char **json);
    /* stats object covering all types, e.g. [{"type":"working","count":3},...] */
    int (*stats)(void *impl, char **json);
    void (*destroy)(void *impl);
} ca_memory_service_ops;

ca_memory_service *ca_memory_service_new(const ca_memory_service_ops *ops, void *impl);
void ca_memory_service_free(ca_memory_service *ms);
const char *ca_memory_service_backend(const ca_memory_service *ms);

int ca_memory_service_remember(ca_memory_service *ms, ca_mem_type t,
                               const char *key, const char *text);
int ca_memory_service_forget(ca_memory_service *ms, ca_mem_type t, const char *key);
int ca_memory_service_recall_key(ca_memory_service *ms, ca_mem_type t,
                                 const char *key, char **text);
int ca_memory_service_recall_query(ca_memory_service *ms, ca_mem_type t,
                                   const char *query, int k, char **json);
int ca_memory_service_stats(ca_memory_service *ms, char **json); /* array of 4 */

/* Default backend: delegates to the existing ca_memory facade (borrowed). */
struct ca_memory;
ca_memory_service *ca_memory_service_new_default(struct ca_memory *m);

#ifdef __cplusplus
}
#endif
