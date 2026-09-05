/* logging.h — leveled, thread-safe logging to console + optional file */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum coa_loglevel {
    COA_LOG_TRACE = 0,
    COA_LOG_DEBUG = 1,
    COA_LOG_INFO  = 2,
    COA_LOG_WARN  = 3,
    COA_LOG_ERROR = 4,
    COA_LOG_FATAL = 5,
    COA_LOG_OFF   = 6,
} coa_loglevel;

typedef struct coa_log_opts {
    coa_loglevel level;      /* minimum level emitted (default INFO) */
    int color;              /* 1 = ANSI colors on stderr (default 1 if tty) */
    const char *file;       /* optional log file path (NULL = none) */
} coa_log_opts;

/* Initialize the global logger. Returns 0 on success, -1 if file open failed. */
int coa_log_init(const coa_log_opts *opts);
void coa_log_shutdown(void);

coa_loglevel coa_log_get_level(void);
void coa_log_set_level(coa_loglevel lvl);

const char *coa_log_level_name(coa_loglevel lvl);

void coa_log_write(coa_loglevel lvl, const char *fmt, ...);

#define coa_log_trace(...) coa_log_write(COA_LOG_TRACE, __VA_ARGS__)
#define coa_log_debug(...) coa_log_write(COA_LOG_DEBUG, __VA_ARGS__)
#define coa_log_info(...)  coa_log_write(COA_LOG_INFO,  __VA_ARGS__)
#define coa_log_warn(...)  coa_log_write(COA_LOG_WARN,  __VA_ARGS__)
#define coa_log_error(...) coa_log_write(COA_LOG_ERROR, __VA_ARGS__)
#define coa_log_fatal(...) coa_log_write(COA_LOG_FATAL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
