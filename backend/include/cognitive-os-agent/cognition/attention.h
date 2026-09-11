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

typedef struct attention attention;

typedef struct attention_candidate {
    const char *text; /* content to match against */
    const char *tags; /* optional space/comma-separated keywords */
    double boost;     /* explicit prior (e.g. recency, frequency) */
} attention_candidate;

typedef struct attention_result {
    int index; /* index into the candidate array */
    double score;
} attention_result;

attention *attention_new(void);
void attention_free(attention *a);

/* Score a single candidate against the query. Higher = more salient. */
double attention_score(attention *a, const char *query, const attention_candidate *c);

/* Rank candidates and write up to `topk` results (best-first) into `out`.
 * Returns the number of results written (0..topk). `out` must have room for
 * `topk` entries. */
int attention_select(attention *a, const char *query, const attention_candidate *cands, size_t n,
                         attention_result *out, size_t topk);

#ifdef __cplusplus
}
#endif
