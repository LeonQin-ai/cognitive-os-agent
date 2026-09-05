/* persist.h — runtime state directory management (memory, snapshots, logs). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the state root directory (default "./state"). Must be called before
 * other persist functions take effect. Returns 0 ok. */
int ca_persist_set_root(const char *dir);
const char *ca_persist_root(void);

/* Build a path under the state root: <root>/<sub>[/file]. Result in out[0..n). */
void ca_persist_path(char *out, size_t n, const char *sub, const char *file);

/* Ensure <root>/<sub> exists. 0 ok, -1 error. */
int ca_persist_ensure(const char *sub);

/* Read/write a JSON text file under the state root. */
char *ca_persist_read(const char *sub, const char *file);
int ca_persist_write(const char *sub, const char *file, const char *text);

#ifdef __cplusplus
}
#endif
