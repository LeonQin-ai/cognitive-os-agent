/* wasm_runner.h — wasm3-backed Wasm runner for the plugin sandbox.
 * Exposes the sandbox_wasm_fn entry point implemented on top of wasm3. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run `fn_name` in `wasm` with a JSON array or named values in object order.
 * Numbers must match the Wasm signature; strings expand to (i32 ptr,i32 len)
 * after guest allocation through exported coa_alloc(i32 size)->i32 ptr.
 * Buffers contain UTF-8 plus a trailing NUL; len excludes the terminator.
 * For text output use {"args":[...],"result":"utf8"}; the function returns
 * i64 packed as (length << 32) | pointer. Text is capped at 1 MiB and cannot
 * contain embedded NUL. Numeric mode returns a typed number (null for void).
 * Returns malloc'd JSON {"ok":bool,"result":...} or an error object.
 * Matches sandbox_wasm_fn. */
char *wasm3_run(const void *wasm, size_t wasm_len, const char *fn_name, const char *args_json);

/* 1 if the wasm3 interpreter was linked into this build. */
int wasm3_available(void);

#ifdef __cplusplus
}
#endif
