/* logging.h — leveled, thread-safe logging to console + optional file */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum coa_loglevel {
    COA_LOG_TRACE = 0,
    COA_LOG_DEBUG = 1,
    COA_LOG_INFO = 2,
    COA_LOG_WARN = 3,
    COA_LOG_ERROR = 4,
    COA_LOG_FATAL = 5,
    COA_LOG_OFF = 6,
} coa_loglevel;

/* Set the minimum level emitted by the global logger (default INFO). */
void coa_log_set_level(coa_loglevel lvl);

void coa_log_write(coa_loglevel lvl, const char *fmt, ...);

#define coa_log_trace(...) coa_log_write(COA_LOG_TRACE, __VA_ARGS__)
#define coa_log_debug(...) coa_log_write(COA_LOG_DEBUG, __VA_ARGS__)
#define coa_log_info(...) coa_log_write(COA_LOG_INFO, __VA_ARGS__)
#define coa_log_warn(...) coa_log_write(COA_LOG_WARN, __VA_ARGS__)
#define coa_log_error(...) coa_log_write(COA_LOG_ERROR, __VA_ARGS__)
#define coa_log_fatal(...) coa_log_write(COA_LOG_FATAL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
