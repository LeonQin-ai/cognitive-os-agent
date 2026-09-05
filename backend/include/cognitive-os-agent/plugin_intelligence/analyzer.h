/* analyzer.h — plugin spec analysis.
 * Infers complexity, required capabilities and suggested built-in tools from a
 * plugin spec ({name, description}). Deterministic and offline. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Analyze a JSON spec object. Returns a JSON report object
 * {name, complexity, capabilities[], tools[]} (malloc'd; caller frees). */
char *coa_analyzer_analyze(const char *spec_json);

#ifdef __cplusplus
}
#endif
