/* testing.h — plugin test planning + smoke execution. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Generate a deterministic JSON test plan from a plugin spec
 * ({name, description}). Returns a JSON object {name, cases[]} (caller frees). */
char *ca_testing_plan(const char *spec_json);

/* Run a build/test command in a sandboxed subprocess. Returns a JSON object
 * {ok, exit_code, timed_out, output} (malloc'd; caller frees). */
char *ca_testing_run(const char *cmd, int timeout_ms);

#ifdef __cplusplus
}
#endif
