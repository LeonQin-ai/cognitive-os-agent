/* audit.h — append-only JSONL audit trail of significant actions. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audit audit;

/* Open (append mode) an audit log file. NULL if the file cannot be opened. */
audit *audit_open(const char *path);

/* Record an entry. detail is an optional JSON string (may be NULL). */
void audit_log(audit *a, const char *action, const char *subject, const char *result, const char *detail_json);

/* Flush and close. */
void audit_close(audit *a);

#ifdef __cplusplus
}
#endif
