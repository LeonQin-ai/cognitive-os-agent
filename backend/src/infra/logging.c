#include "cagent/infra/logging.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    ca_loglevel level;
    int color;
    FILE *file;
    ca_mutex mtx;
    int inited;
} logger;

static logger g_log;

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};

const char *ca_log_level_name(ca_loglevel lvl) {
    if (lvl < CA_LOG_TRACE || lvl > CA_LOG_FATAL) return "?";
    return level_names[lvl];
}

static const char *level_colors[] = {
    "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m",
};

int ca_log_init(const ca_log_opts *opts) {
    if (g_log.inited) ca_log_shutdown();
    ca_mutex_init(&g_log.mtx);
    g_log.level = opts ? opts->level : CA_LOG_INFO;
    g_log.file = NULL;
    g_log.color = opts ? opts->color : 1;
    if (opts && opts->file) {
        g_log.file = fopen(opts->file, "a");
        if (!g_log.file) return -1;
    }
    g_log.inited = 1;
    return 0;
}

void ca_log_shutdown(void) {
    if (!g_log.inited) return;
    if (g_log.file) { fclose(g_log.file); g_log.file = NULL; }
    ca_mutex_destroy(&g_log.mtx);
    memset(&g_log, 0, sizeof(g_log));
}

ca_loglevel ca_log_get_level(void) { return g_log.level; }
void ca_log_set_level(ca_loglevel lvl) { g_log.level = lvl; }

static void log_line_to(FILE *f, int color, ca_loglevel lvl, const char *text) {
    char ts[32];
    ca_time_now_str(ts, sizeof(ts));
    if (color) {
        fprintf(f, "%s %s%-5s\x1b[0m %s\n", ts, level_colors[lvl], level_names[lvl], text);
    } else {
        fprintf(f, "%s %-5s %s\n", ts, level_names[lvl], text);
    }
}

void ca_log_write(ca_loglevel lvl, const char *fmt, ...) {
    if (lvl < g_log.level || lvl > CA_LOG_FATAL) return;
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    ca_mutex_lock(&g_log.mtx);
    int use_color = g_log.color && !g_log.file;
    log_line_to(stderr, use_color, lvl, text);
    if (g_log.file) log_line_to(g_log.file, 0, lvl, text);
    ca_mutex_unlock(&g_log.mtx);
}
