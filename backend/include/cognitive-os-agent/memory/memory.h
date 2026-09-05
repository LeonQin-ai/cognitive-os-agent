/* memory.h — cognitive memory facade.
 * Composes fine-grained sub-stores (see the cognitive-os-agent/memory headers):
 *  - working memory:  short-term ring buffer of recent items (inline)
 *  - long-term facts: coa_kvstore (memory/kv.h)
 *  - episodes:        coa_episodic (memory/episode.h)
 *  - vector-lite:     coa_vectorstore over working + episodes (memory/vector.h)
 * Long-term facts are persisted as JSON under the state root. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_memory coa_memory;

/* Load (or create) memory in <state_root>/memory. NULL on failure. */
coa_memory *coa_memory_new(const char *state_root);
void coa_memory_free(coa_memory *m);

/* Working memory (bounded ring buffer of recent strings). */
void coa_memory_working_push(coa_memory *m, const char *text);
int coa_memory_working_count(coa_memory *m);
/* Borrowed i-th working item (0 = newest). Do not free. */
const char *coa_memory_working_at(coa_memory *m, int i);

/* Long-term facts. value may be NULL to delete the key. */
void coa_memory_remember(coa_memory *m, const char *key, const char *value);
/* Returns a borrowed pointer, or NULL. */
const char *coa_memory_recall(coa_memory *m, const char *key);

/* Experience: record a completed episode. */
void coa_memory_record_experience(coa_memory *m, const char *task, const char *result);

/* Index an arbitrary document chunk into the vector store (RAG); it becomes
 * recallable via coa_memory_retrieve / the "## Retrieved context" prompt
 * section. id and meta may be NULL. Returns 0 ok, -1 bad args/OOM. */
int coa_memory_index_document(coa_memory *m, const char *id, const char *text,
                             const char *meta);
/* Split `text` into ~600-char chunks at paragraph boundaries and index each
 * with id "<base>#<i>" / meta "upload". Returns the number of chunks added. */
int coa_memory_index_text(coa_memory *m, const char *base, const char *text);
/* (Re)index the text files under `dir` (the uploads folder) into the vector
 * store — called at startup so uploaded documents survive restarts. A missing
 * directory is not an error. Returns the number of chunks indexed. */
int coa_memory_index_uploads(coa_memory *m, const char *dir);

/* Knowledge graph over session entities (task -used-> tool -touched-> file).
 * Nodes are created on demand (id = label); duplicate nodes/edges are folded. */
void coa_memory_record_edge(coa_memory *m, const char *from, const char *to,
                           const char *relation);
/* Whole graph as {nodes:[{id,label}],edges:[...]} (caller frees). */
char *coa_memory_graph_json(coa_memory *m);
/* Edges whose endpoint labels share a token with the query, as a JSON array
 * of {text:"<from> -<relation>-> <to>"} (caller frees). */
char *coa_memory_graph_related(coa_memory *m, const char *query, int limit);

/* Consolidation engine: distill recurring episode themes into long-term
 * facts ("topic.<token>: seen in N tasks"). Returns facts written. */
int coa_memory_consolidate(coa_memory *m);

/* Automatic consolidation (memory lifecycle): runs when >= threshold_eps new
 * episodes accumulated since the last pass AND interval_ms elapsed (first
 * call always eligible; interval 0 = no time gate). Rule-engine distillation
 * (no LLM needed): recurring episode themes -> SEMANTIC facts ("topic.*"),
 * recurring used_tool graph edges -> PROCEDURAL facts ("procedure.*").
 * Returns 1 = pass ran, 0 = skipped (threshold/interval not met), -1 bad args. */
int coa_memory_maybe_consolidate(coa_memory *m, int threshold_eps, long long interval_ms);
/* Number of automatic consolidation passes run so far. */
int coa_memory_consolidation_count(coa_memory *m);

/* --- memory lifecycle: reinforce / decay / forget / archive ---
 * Episodes carry a strength: +1 per re-experience (reinforce), halved once per
 * half_life_ms of age (decay), and dropped+archived when below a threshold
 * (forget+archive). Configure the automatic pass (runs together with
 * automatic consolidation); half_life_ms <= 0 disables the auto pass. */
void coa_memory_set_lifecycle(coa_memory *m, long long half_life_ms,
                             double min_strength, int archive);

/* Explicit lifecycle pass: decay by age, then archive (append the below-
 * threshold episodes to <state_root>/memory/archive.jsonl) and forget them.
 * cfg may be NULL (defaults: now, no decay, no drop). Returns the number of
 * episodes dropped, -1 on bad args. */
typedef struct {
    long long now_ms;        /* 0 = current time */
    long long half_life_ms;  /* <= 0 = skip decay */
    double min_strength;     /* <= 0 = no forget/archive */
    int archive;             /* 1 = append dropped entries to archive.jsonl */
} coa_memory_lifecycle_cfg;
int coa_memory_lifecycle_pass(coa_memory *m, const coa_memory_lifecycle_cfg *cfg);

/* Explicitly reinforce an episode (find by task string; +1 strength). */
void coa_memory_reinforce(coa_memory *m, const char *task);

/* Current episode count (observability / tests). */
int coa_memory_episode_count(coa_memory *m);

/* Keyword search across working + episode items (JSON array of {kind,text,score}). */
char *coa_memory_search(coa_memory *m, const char *query, int limit);

/* Vector nearest-neighbor recall across mirrored working + episode items.
 * Returns a JSON array of {id,text,meta,score} (caller frees). */
char *coa_memory_retrieve(coa_memory *m, const char *query, int k);

/* Two-stage retrieval upgrade: (1) hybrid recall of 3k candidates with
 * score = w_vec*cosine + (1-w_vec)*keyword, (2) rerank the candidates
 * (token+bigram relevance) and return the top k by the blended score.
 * w_vec clamped to [0,1]; 0.7 is a sensible default. Same JSON shape. */
char *coa_memory_retrieve_ex(coa_memory *m, const char *query, int k, float w_vec);

/* Multi-query (MQE) retrieval: retrieve per query variant and merge per entry
 * with the max score. Same JSON shape. */
char *coa_memory_retrieve_mqe(coa_memory *m, const char *const *queries, int nq, int k);

/* Persist long-term facts to disk. */
void coa_memory_flush(coa_memory *m);

/* Render working memory as a JSON array of strings (caller frees). */
char *coa_memory_working_json(coa_memory *m);
/* Render long-term store as a JSON object (caller frees). */
char *coa_memory_longterm_json(coa_memory *m);
/* Render all episodes as a JSON array of {task,result} (caller frees). */
char *coa_memory_episodes_json(coa_memory *m);

#ifdef __cplusplus
}
#endif
