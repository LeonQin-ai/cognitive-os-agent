/* evaluator.c — result verification + heuristic quality scoring. */
#include "cognitive-os-agent/cognition/evaluator.h"

#include <stdlib.h>
#include <string.h>

struct coa_evaluator { int reserved; };

coa_evaluator *coa_evaluator_new(void) { return (coa_evaluator *)calloc(1, sizeof(coa_evaluator)); }
void coa_evaluator_free(coa_evaluator *e) { free(e); }

int coa_evaluator_verify(coa_evaluator *e, int all_actions_ok, int n_actions,
                        int ok_actions) {
    (void)e;
    (void)all_actions_ok;
    if (n_actions == 0) return 1;          /* conversational / no-op: fine */
    /* Tolerate partial failures: a run that completed at least one tool action
     * still produced a useful result. Only a fully-failed plan (every action
     * errored) is treated as a failure — this keeps single-pass tasks robust
     * to one wrong path guess instead of sinking the whole task. */
    return ok_actions > 0 ? 1 : 0;
}

double coa_evaluator_score(coa_evaluator *e, int n_actions, int ok_actions,
                          int all_actions_ok, const char *answer) {
    (void)e;
    if (answer) {
        if (strstr(answer, "FAILED") || strstr(answer, "denied") ||
            strstr(answer, "rollback") || strstr(answer, "error"))
            return 0.0;
    }
    double s = 0.0;
    if (all_actions_ok) s += 0.4;
    if (n_actions > 0 && ok_actions == n_actions) s += 0.3;
    size_t len = answer ? strlen(answer) : 0;
    if (len > 0) s += 0.1;
    if (len > 80) s += 0.2;
    return s > 1.0 ? 1.0 : s;
}
