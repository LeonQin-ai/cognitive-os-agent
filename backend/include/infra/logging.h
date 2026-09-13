/* logging.h — leveled, thread-safe logging to console + optional file */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loglevel {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO = 2,
    LOG_WARN = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5,
    LOG_OFF = 6,
} loglevel;

/* Set the minimum level emitted by the global logger (default INFO). */
void log_set_level(loglevel lvl);

void log_write(loglevel lvl, const char *fmt, ...);

#define log_trace(...) log_write(LOG_TRACE, __VA_ARGS__)
#define log_debug(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define log_info(...) log_write(LOG_INFO, __VA_ARGS__)
#define log_warn(...) log_write(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)
#define log_fatal(...) log_write(LOG_FATAL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
