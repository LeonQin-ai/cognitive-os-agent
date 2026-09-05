/* security.h — static security audit of plugin source/spec.
 * Scans text for dangerous patterns and returns a ranked findings report. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Audit `text` (plugin source or spec). Returns a JSON object
 * {findings:[{pattern,severity,message}]} (malloc'd; caller frees). */
char *ca_security_audit(const char *text);

#ifdef __cplusplus
}
#endif
