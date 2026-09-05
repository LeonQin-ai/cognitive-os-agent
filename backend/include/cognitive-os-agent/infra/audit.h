/* audit.h — append-only JSONL audit trail of significant actions. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_audit ca_audit;

/* Open (append mode) an audit log file. NULL if the file cannot be opened. */
ca_audit *ca_audit_open(const char *path);

/* Record an entry. detail is an optional JSON string (may be NULL). */
void ca_audit_log(ca_audit *a, const char *action, const char *subject,
                  const char *result, const char *detail_json);

/* Flush and close. */
void ca_audit_close(ca_audit *a);

#ifdef __cplusplus
}
#endif
