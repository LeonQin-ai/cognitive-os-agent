/* logging.h — leveled, thread-safe logging to console + optional file */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ca_loglevel {
    CA_LOG_TRACE = 0,
    CA_LOG_DEBUG = 1,
    CA_LOG_INFO  = 2,
    CA_LOG_WARN  = 3,
    CA_LOG_ERROR = 4,
    CA_LOG_FATAL = 5,
    CA_LOG_OFF   = 6,
} ca_loglevel;

typedef struct ca_log_opts {
    ca_loglevel level;      /* minimum level emitted (default INFO) */
    int color;              /* 1 = ANSI colors on stderr (default 1 if tty) */
    const char *file;       /* optional log file path (NULL = none) */
} ca_log_opts;

/* Initialize the global logger. Returns 0 on success, -1 if file open failed. */
int ca_log_init(const ca_log_opts *opts);
void ca_log_shutdown(void);

ca_loglevel ca_log_get_level(void);
void ca_log_set_level(ca_loglevel lvl);

const char *ca_log_level_name(ca_loglevel lvl);

void ca_log_write(ca_loglevel lvl, const char *fmt, ...);

#define ca_log_trace(...) ca_log_write(CA_LOG_TRACE, __VA_ARGS__)
#define ca_log_debug(...) ca_log_write(CA_LOG_DEBUG, __VA_ARGS__)
#define ca_log_info(...)  ca_log_write(CA_LOG_INFO,  __VA_ARGS__)
#define ca_log_warn(...)  ca_log_write(CA_LOG_WARN,  __VA_ARGS__)
#define ca_log_error(...) ca_log_write(CA_LOG_ERROR, __VA_ARGS__)
#define ca_log_fatal(...) ca_log_write(CA_LOG_FATAL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
