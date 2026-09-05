/* evaluator.h — result verification + heuristic quality scoring.
 * Extracted from the reasoning engine's VERIFY stage: decides whether a run
 * succeeded and scores the quality of its output. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_evaluator coa_evaluator;

coa_evaluator *coa_evaluator_new(void);
void coa_evaluator_free(coa_evaluator *e);

/* Verify a completed run: 1 if at least one action succeeded (or none were
 * planned); a run where every action failed is treated as a failure. */
int coa_evaluator_verify(coa_evaluator *e, int all_actions_ok, int n_actions,
                        int ok_actions);

/* Heuristic 0..1 quality score from structured run inputs. */
double coa_evaluator_score(coa_evaluator *e, int n_actions, int ok_actions,
                          int all_actions_ok, const char *answer);

#ifdef __cplusplus
}
#endif
