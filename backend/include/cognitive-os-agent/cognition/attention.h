/* attention.h — salience scoring and top-k selection.
 * A deterministic, offline "attention" primitive: given a query and a set of
 * candidate items (retrieved memories, tools, episodes), rank them by how
 * relevant they are. Scoring is keyword-overlap over tokenized query words plus
 * an explicit prior (boost), so the planner/retrieval can focus on the most
 * salient context. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_attention coa_attention;

typedef struct coa_attention_candidate {
    const char *text;   /* content to match against */
    const char *tags;   /* optional space/comma-separated keywords */
    double boost;       /* explicit prior (e.g. recency, frequency) */
} coa_attention_candidate;

typedef struct coa_attention_result {
    int index;          /* index into the candidate array */
    double score;
} coa_attention_result;

coa_attention *coa_attention_new(void);
void coa_attention_free(coa_attention *a);

/* Score a single candidate against the query. Higher = more salient. */
double coa_attention_score(coa_attention *a, const char *query,
                          const coa_attention_candidate *c);

/* Rank candidates and write up to `topk` results (best-first) into `out`.
 * Returns the number of results written (0..topk). `out` must have room for
 * `topk` entries. */
int coa_attention_select(coa_attention *a, const char *query,
                        const coa_attention_candidate *cands, size_t n,
                        coa_attention_result *out, size_t topk);

#ifdef __cplusplus
}
#endif
