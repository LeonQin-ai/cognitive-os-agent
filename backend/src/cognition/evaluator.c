/* evaluator.c — result verification + heuristic quality scoring. */
#include "cagent/cognition/evaluator.h"

#include <stdlib.h>
#include <string.h>

struct ca_evaluator { int reserved; };

ca_evaluator *ca_evaluator_new(void) { return (ca_evaluator *)calloc(1, sizeof(ca_evaluator)); }
void ca_evaluator_free(ca_evaluator *e) { free(e); }

int ca_evaluator_verify(ca_evaluator *e, int all_actions_ok, int n_actions) {
    (void)e;
    (void)n_actions;
    return all_actions_ok ? 1 : 0;
}

double ca_evaluator_score(ca_evaluator *e, int n_actions, int ok_actions,
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
