/* audit.h — append-only JSONL audit trail of significant actions. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_audit coa_audit;

/* Open (append mode) an audit log file. NULL if the file cannot be opened. */
coa_audit *coa_audit_open(const char *path);

/* Record an entry. detail is an optional JSON string (may be NULL). */
void coa_audit_log(coa_audit *a, const char *action, const char *subject,
                  const char *result, const char *detail_json);

/* Flush and close. */
void coa_audit_close(coa_audit *a);

#ifdef __cplusplus
}
#endif
