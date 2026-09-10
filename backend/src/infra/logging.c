#include "cognitive-os-agent/infra/logging.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    coa_loglevel level;
    int color;
    FILE *file;
    coa_mutex mtx;
    int inited;
} logger;

static logger g_log;

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};

const char *coa_log_level_name(coa_loglevel lvl) {
    if (lvl < COA_LOG_TRACE || lvl > COA_LOG_FATAL)
        return "?";
    return level_names[lvl];
}

static const char *level_colors[] = {
    "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m",
};

void coa_log_set_level(coa_loglevel lvl) {
    g_log.level = lvl;
}

static void log_line_to(FILE *f, int color, coa_loglevel lvl, const char *text) {
    char ts[32];
    coa_time_now_str(ts, sizeof(ts));
    if (color) {
        fprintf(f, "%s %s%-5s\x1b[0m %s\n", ts, level_colors[lvl], level_names[lvl], text);
    } else {
        fprintf(f, "%s %-5s %s\n", ts, level_names[lvl], text);
    }
}

void coa_log_write(coa_loglevel lvl, const char *fmt, ...) {
    if (lvl < g_log.level || lvl > COA_LOG_FATAL)
        return;
    char text[16384]; /* tool plans embed whole file bodies; 2048 cut them
                       * off mid-JSON and made LLM-plan diagnostics useless */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    coa_mutex_lock(&g_log.mtx);
    int use_color = g_log.color && !g_log.file;
    log_line_to(stderr, use_color, lvl, text);
    if (g_log.file)
        log_line_to(g_log.file, 0, lvl, text);
    coa_mutex_unlock(&g_log.mtx);
}
