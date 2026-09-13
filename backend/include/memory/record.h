/* record.h — V3.1 long-term memory records (Detailed Design v1.1 §7/§8/§20).
 *
 * A record is a PROMOTED long-term memory. Raw events and episodes stay in
 * the episodic experience log; only candidates that pass the promotion gate
 * (evidence + score) become records.
 *
 * Canonical storage is human-readable Markdown: YAML frontmatter carries the
 * record attributes, the body carries the content. All derived indexes (the
 * vector store mirror, the Context MMU pages) are rebuildable from the
 * Markdown files.
 *
 * Lifecycle states follow DDD §8.1: CANDIDATE -> PROVISIONAL -> TRUSTED ->
 * VERIFIED, with DEPRECATED / CONTRADICTED / ARCHIVED as exits. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Long-term semantic dimension (Baseline §10.1). */
typedef enum {
    MEMR_SEMANTIC = 0,
    MEMR_EPISODIC,
    MEMR_PROCEDURAL,
    MEMR_PREFERENCE,
    MEMR_PROJECT,
    MEMR_ENTITY
} mem_type_t;

/* Lifecycle state (DDD v1.1 §7.2/§8.1). A record enters the store as
 * PROVISIONAL (evidence collected) and matures: TRUSTED after repeated
 * evidence, VERIFIED under strong machine-checkable evidence. DEPRECATED
 * (obsolete), CONTRADICTED (conflicting evidence) and ARCHIVED (retention
 * policy) are exits. */
typedef enum {
    MEM_STATUS_PROVISIONAL = 0,
    MEM_STATUS_TRUSTED,
    MEM_STATUS_VERIFIED,
    MEM_STATUS_DEPRECATED,
    MEM_STATUS_CONTRADICTED,
    MEM_STATUS_ARCHIVED
} mem_status_t;

/* Machine-checkable outcome of the originating experience (Baseline §12). */
typedef enum {
    MEM_OUTCOME_UNKNOWN = 0,
    MEM_OUTCOME_SUCCESS,
    MEM_OUTCOME_FAILURE,
    MEM_OUTCOME_PARTIAL
} mem_outcome_t;

#define MEM_ID_MAX 40
#define MEM_SCOPE_MAX 64
#define MEM_SOURCE_MAX 32
#define MEM_EVIDENCE_MAX 160
#define MEM_TITLE_MAX 128

typedef struct mem_record {
    char id[MEM_ID_MAX];          /* "mem_<hex>" (serves as the uri) */
    mem_type_t type;
    char title[MEM_TITLE_MAX];    /* short human-readable title */
    char scope[MEM_SCOPE_MAX];    /* e.g. "agent/status", "user/global" */
    mem_status_t status;
    double importance;            /* [0,1] user-level importance */
    double confidence;            /* [0,1] evidence-backed trust */
    double utility;               /* [0,1] repeated usefulness */
    double valence;               /* emotion: [-1,1] negative..positive */
    double arousal;               /* emotion: [0,1] calm..exciting */
    double salience;              /* emotion: [0,1] attention weight */
    char source[MEM_SOURCE_MAX];  /* experience|instruction|reflection|external */
    char origin_id[MEM_ID_MAX];   /* derived_from: episode task signature hash */
    mem_outcome_t outcome;        /* evidence class of the origin experience */
    char evidence[MEM_EVIDENCE_MAX]; /* human-readable evidence line */
    int version;                  /* bumped on merge/reinforce */
    long long created_ms;
    long long updated_ms;
    long long last_access_ms;
    unsigned access_count;
    char *content;                /* malloc'd body (the memory itself) */
} mem_record;

/* Enum name helpers ("semantic"..., "provisional"...). */
const char *mem_record_type_name(mem_type_t t);
const char *mem_record_status_name(mem_status_t s);

/* --- Markdown (de)serialization ------------------------------------- */

/* Serialize to "onward-parseable" Markdown: YAML frontmatter + body.
 * Returns malloc'd text (caller frees), NULL on bad args/OOM. */
char *mem_record_to_md(const mem_record *r);

/* Parse Markdown created by mem_record_to_md. Returns malloc'd record
 * (caller frees with mem_record_free), NULL on parse failure. */
mem_record *mem_record_from_md(const char *md);

void mem_record_free(mem_record *r);

/* --- record store ---------------------------------------------------- */

typedef struct mem_records mem_records;

/* Open the store rooted at <root>/records (root is typically
 * <state_root>/memory). Loads every *.md file. NULL on OOM. */
mem_records *mem_records_new(const char *root);
void mem_records_free(mem_records *rs);

/* Insert or replace (by id) a record and persist it as Markdown.
 * The store keeps its own copy. Returns 0 ok, -1 bad args/OOM/io. */
int mem_records_put(mem_records *rs, const mem_record *r);

/* Borrowed i-th record (0 = oldest insertion). Do not free. */
const mem_record *mem_records_at(mem_records *rs, int i);
int mem_records_count(mem_records *rs);

/* Find by id (borrowed) or by origin_id (first match). NULL if absent. */
const mem_record *mem_records_find(mem_records *rs, const char *id);
const mem_record *mem_records_find_origin(mem_records *rs, const char *origin_id);

/* Remove a record (file deleted). Returns 1 removed, 0 not found, -1 error. */
int mem_records_remove(mem_records *rs, const char *id);

/* --- derived index (rebuildable) -------------------------------------- */

/* Rebuild the derived vector index from canonical Markdown: mirrors every
 * non-exited record (not deprecated/contradicted/archived) into `v` under
 * id "r:<record-id>" with meta "record". Wipes previous "r:" entries first
 * (idempotent). Returns entries mirrored. */
int mem_records_rebuild_index(mem_records *rs, void *v);

/* Touch (access bookkeeping) — bumps access_count/last_access on the store
 * copy. Cheap; persistence of counters happens on next put. */
void mem_records_touch(mem_records *rs, const char *id, long long now_ms);

/* All records as a JSON array of attribute objects (caller frees). */
char *mem_records_json(mem_records *rs);

#ifdef __cplusplus
}
#endif
