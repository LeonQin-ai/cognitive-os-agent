/* architect.h — plugin architecture design.
 * Produces a deterministic component/interface/dataflow plan for a plugin goal.
 * This is a template-based cognitive accelerator, not a real code generator. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Design an architecture for `goal`. Returns a JSON object
 * {goal, components[], interfaces[]} (malloc'd; caller frees). */
char *ca_architect_design(const char *goal);

#ifdef __cplusplus
}
#endif
