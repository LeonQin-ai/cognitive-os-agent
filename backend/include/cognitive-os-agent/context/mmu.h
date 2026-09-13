/* mmu.h — Context MMU (Detailed Design v1.1 §9, §7.4/§7.5/§7.7).
 *
 * Manages the active model context as a runtime working set over the
 * long-term record store.
 *
 * Two orthogonal dimensions (DDD §4, DDR-003):
 *   L0/L1/L2      representation granularity: abstract / overview / detail
 *   HOT/WARM/COLD residency: L2 loaded / L1 loaded / metadata only
 * A page is e.g. HOT+L2 (deep reasoning), WARM+L1 (ranking) or COLD
 * (only metadata + L0 in the index).
 *
 * Recall flow (§9.4):
 *   query -> vector candidates (L0-ish) -> rank (§7.7 score)
 *         -> promote to WARM, materialize L1 -> re-rank
 *         -> promote top-K to HOT, materialize L2
 *         -> evict to fit the token budget (§9.5 keep_score)
 *
 * Representations are derived deterministically from record content and
 * cached by content hash (§7.4): a record is re-summarized only when its
 * content changed. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cognitive-os-agent/memory/record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- configuration ----------------------------------------------------- */

typedef struct ctx_mmu_config {
    size_t token_budget;      /* max tokens of materialized context */
    /* §7.7 factor weights; the score is normalized by their sum, so they
     * do not need to add up to 1. */
    double w_relevance;
    double w_importance;
    double w_confidence;
    double w_utility;
    double w_recency;
    double w_salience;
    double w_scope;
    double recency_halflife_ms; /* recency factor decay constant */
    int l2_top_k;             /* records promoted to HOT/L2 per recall */
    int candidate_k;          /* vector candidate pool size */
    const char *scope;        /* optional scope filter for scope_match */
    long long now_ms;         /* <=0 -> time_now_ms() */
} ctx_mmu_config;

void ctx_mmu_config_default(ctx_mmu_config *cfg);

/* --- MMU ---------------------------------------------------------------- */

typedef struct ctx_mmu ctx_mmu;

/* New MMU with the default config. NULL on OOM. */
ctx_mmu *ctx_mmu_new(void);
void ctx_mmu_free(ctx_mmu *mmu);

void ctx_mmu_set_config(ctx_mmu *mmu, const ctx_mmu_config *cfg);
/* Current residency: token_used / token_budget / page count as JSON. */
char *ctx_mmu_stats_json(ctx_mmu *mmu);

/* Pin a record's page (task state; never evicted). 0 ok, -1 not found. */
int ctx_mmu_pin(ctx_mmu *mmu, const char *record_id, int pinned);

/* --- recall ------------------------------------------------------------- */

/* Run the §9.4 recall/promotion flow for `query` over the record store `rs`
 * with the vector mirror `vs` (a vectorstore*, borrowed; may be NULL to
 * rank by attributes only). Returns a JSON array of selected context items:
 *   [{"id","title","type","scope","status","level","residency",
 *     "score", "factors":{relevance,importance,confidence,utility,
 *     recency,salience,scope},"tokens","text"}...]
 * Factor values are logged per item so retrieval decisions stay
 * explainable (§7.7). Caller frees. */
char *ctx_mmu_recall(ctx_mmu *mmu, mem_records *rs, void *vs,
                     const char *query, const ctx_mmu_config *cfg);

#ifdef __cplusplus
}
#endif
