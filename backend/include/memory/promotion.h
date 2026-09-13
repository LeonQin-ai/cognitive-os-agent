/* promotion.h — V3.1 memory promotion gate (Detailed Design v1.1 §8).
 *
 * A candidate is a proposed long-term memory. Nothing reaches the record
 * store without passing this gate:
 *   CANDIDATE -> score -> verify (evidence check) -> PROMOTE | MERGE | REJECT
 *
 * DDD v1.1 §8.3: outcomes are machine-checkable classes. An LLM conclusion
 * with no evidence cannot cross the gate (score is capped). FAILURE
 * outcomes are first-class evidence (negative procedural memory is as
 * valuable as positive memory).
 *
 * Lifecycle transitions (DDD §8.1):
 *   PROMOTE -> record enters as PROVISIONAL (evidence collected)
 *   MERGE   -> repeated evidence matures the record:
 *              PROVISIONAL -> TRUSTED (evidenced merge)
 *              TRUSTED -> VERIFIED (strong evidence class: SUCCESS/FAILURE)
 *
 * Merging: a candidate whose origin_id matches a live record
 * (provisional/trusted/verified) reinforces it (bump version/confidence/
 * utility) instead of creating a duplicate. Consolidation/decay remain in
 * the memory facade. */
#pragma once
#include <stddef.h>
#include "memory/record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- candidate --------------------------------------------------------- */

/* A proposed memory awaiting the gate. Same attribute shape as a record,
 * minus identity/lifecycle (assigned on promotion). */
typedef struct mem_candidate {
    mem_type_t type;
    char scope[MEM_SCOPE_MAX];
    char *content;                /* malloc'd; owned by the candidate */
    double importance;            /* [0,1] asserted */
    double confidence;            /* [0,1] asserted */
    double valence;               /* [-1,1] */
    double arousal;               /* [0,1] */
    double salience;              /* [0,1] */
    char source[MEM_SOURCE_MAX];  /* experience|instruction|reflection|external */
    char origin_id[MEM_ID_MAX];   /* episode/task signature (dedup key) */
    mem_outcome_t outcome;        /* evidence class of the origin experience */
    char evidence[MEM_EVIDENCE_MAX]; /* machine-checkable evidence line */
} mem_candidate;

/* Zero the candidate and set defaults (type=semantic, source=experience,
 * outcome=unknown). */
void mem_candidate_init(mem_candidate *c);
/* Frees content and zeroes the candidate. */
void mem_candidate_free(mem_candidate *c);

/* --- gate configuration ------------------------------------------------ */

typedef struct mem_gate_config {
    double promote_threshold;  /* score >= this -> new record (default 0.55) */
    double merge_threshold;    /* score >= this and origin match -> merge (0.35) */
    int require_evidence;      /* cap score when evidence line empty (1) */
    double no_evidence_cap;    /* cap for unevidenced candidates (0.50) */
    double reinforcement;      /* confidence bump per merge, clamp [0,1] (0.05) */
    double utility_bump;       /* utility bump per merge, clamp [0,1] (0.05) */
    long long now_ms;          /* <=0 -> time_now_ms() */
} mem_gate_config;

/* Fill cfg with the defaults listed above. */
void mem_gate_config_default(mem_gate_config *cfg);

/* --- scoring ----------------------------------------------------------- */

/* Composite gate score in [0,1]:
 *   0.40*importance + 0.30*confidence + 0.20*outcome_weight + 0.10*salience
 * outcome_weight: SUCCESS/PARTIAL 1.0/0.7; FAILURE 0.9 for procedural
 * (negative procedural memory) else 0.6; UNKNOWN 0.4. When cfg requires
 * evidence and the candidate has none, the score is capped at
 * no_evidence_cap. */
double mem_gate_score(const mem_candidate *c, const mem_gate_config *cfg);

/* --- gate -------------------------------------------------------------- */

typedef enum {
    MEM_GATE_REJECT = 0,  /* below thresholds, or malformed candidate */
    MEM_GATE_PROMOTE,     /* new record persisted */
    MEM_GATE_MERGE        /* existing record reinforced (same origin) */
} mem_gate_decision_t;

typedef struct mem_gate_result {
    mem_gate_decision_t decision;
    char id[MEM_ID_MAX];  /* new/merged record id ("" on reject) */
    double score;         /* composite score the decision was based on */
    int version;          /* record version after promote/merge (0 reject) */
} mem_gate_result;

/* Run the gate on one candidate.
 *   score < merge_threshold                     -> REJECT
 *   active record with same origin_id           -> MERGE (version+1,
 *                                                 confidence/utility bumped,
 *                                                 updated_ms refreshed;
 *                                                 stronger evidence class
 *                                                 replaces outcome/evidence)
 *   score >= promote_threshold                  -> PROMOTE (new "mem_<hex>"
 *                                                 id, persisted)
 *   otherwise                                   -> REJECT
 * On PROMOTE/MERGE the record store is updated and persisted; when vs is
 * non-NULL the "r:<id>" vector mirror is refreshed too. out is always
 * filled (NULL out allowed). Returns the decision. */
mem_gate_decision_t mem_gate_apply(mem_records *rs, void *vs,
                                   const mem_gate_config *cfg,
                                   const mem_candidate *c,
                                   mem_gate_result *out);

#ifdef __cplusplus
}
#endif
