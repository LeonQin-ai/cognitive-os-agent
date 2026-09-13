/* platform.h — OS platform hook registry.
 *
 * Platform-specific backends live in per-OS directories
 * (src/os/linux/, src/os/windows/) and register themselves as hook
 * tables instead of littering shared code with #ifdefs. The build
 * compiles exactly one platform directory; each provides the same
 * provider symbol `os_http_platform_hooks()`, which the dispatcher
 * binds at first use. New OS support = new directory implementing the
 * hook tables + provider function + one build rule.
 *
 * The registry is also open at runtime: embedders or tests can swap a
 * backend via os_http_register(). */
#pragma once
#include "os/http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- HTTP backend hooks -------------------------------------------------- */

typedef struct os_http_hooks {
    const char *name; /* backend name, e.g. "winhttp", "posix+curl" */

    http_response *(*post)(const char *base_url, const char *path,
                           const char *body, const char *content_type,
                           strmap *extra_headers, int timeout_ms);
    http_response *(*get)(const char *base_url, const char *path,
                          strmap *extra_headers, int timeout_ms);
    void (*response_free)(http_response *r);

    http_stream *(*stream_open)(const char *base_url, const char *method,
                                const char *path, const char *body,
                                const char *content_type,
                                strmap *extra_headers, int timeout_ms);
    int (*stream_status)(http_stream *h);
    int (*stream_read_line)(http_stream *h, char *out, size_t cap);
    int (*stream_read)(http_stream *h, char *out, size_t cap);
    void (*stream_close)(http_stream *h);
} os_http_hooks;

/* Register a backend as the active implementation (replaces any previous
 * registration; NULL is ignored). */
void os_http_register(const os_http_hooks *hooks);

/* Currently active backend (NULL until the platform provider has bound). */
const os_http_hooks *os_http_active(void);

/* Provided by the compiled platform directory (one of the per-OS impls). */
extern const os_http_hooks *os_http_platform_hooks(void);

/* Name of the active backend ("" before first use). */
const char *os_http_backend_name(void);

#ifdef __cplusplus
}
#endif
