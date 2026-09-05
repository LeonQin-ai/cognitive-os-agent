/* evaluator.h — result verification + heuristic quality scoring.
 * Extracted from the reasoning engine's VERIFY stage: decides whether a run
 * succeeded and scores the quality of its output. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_evaluator ca_evaluator;

ca_evaluator *ca_evaluator_new(void);
void ca_evaluator_free(ca_evaluator *e);

/* Verify a completed run: 1 if at least one action succeeded (or none were
 * planned); a run where every action failed is treated as a failure. */
int ca_evaluator_verify(ca_evaluator *e, int all_actions_ok, int n_actions,
                        int ok_actions);

/* Heuristic 0..1 quality score from structured run inputs. */
double ca_evaluator_score(ca_evaluator *e, int n_actions, int ok_actions,
                          int all_actions_ok, const char *answer);

#ifdef __cplusplus
}
#endif
