/* wasm_runner.h — wasm3-backed Wasm runner for the plugin sandbox.
 * Exposes the ca_sandbox_wasm_fn entry point implemented on top of wasm3. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run `fn_name` in `wasm` with JSON args (array [..] or object {k:v} of
 * numbers). Returns a malloc'd JSON {"ok":bool,"result":N} or an error
 * object. Matches ca_sandbox_wasm_fn. */
char *ca_wasm3_run(const void *wasm, size_t wasm_len,
                   const char *fn_name, const char *args_json);

/* 1 if the wasm3 interpreter was linked into this build. */
int ca_wasm3_available(void);

#ifdef __cplusplus
}
#endif
