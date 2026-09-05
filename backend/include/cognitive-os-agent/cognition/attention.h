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

typedef struct ca_attention ca_attention;

typedef struct ca_attention_candidate {
    const char *text;   /* content to match against */
    const char *tags;   /* optional space/comma-separated keywords */
    double boost;       /* explicit prior (e.g. recency, frequency) */
} ca_attention_candidate;

typedef struct ca_attention_result {
    int index;          /* index into the candidate array */
    double score;
} ca_attention_result;

ca_attention *ca_attention_new(void);
void ca_attention_free(ca_attention *a);

/* Score a single candidate against the query. Higher = more salient. */
double ca_attention_score(ca_attention *a, const char *query,
                          const ca_attention_candidate *c);

/* Rank candidates and write up to `topk` results (best-first) into `out`.
 * Returns the number of results written (0..topk). `out` must have room for
 * `topk` entries. */
int ca_attention_select(ca_attention *a, const char *query,
                        const ca_attention_candidate *cands, size_t n,
                        ca_attention_result *out, size_t topk);

#ifdef __cplusplus
}
#endif
