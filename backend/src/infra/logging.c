#include "infra/logging.h"
#include "os/os_thread.h"
#include "os/os_time.h"
#include "security/secret.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>

typedef struct {
    int color;
    FILE *file;
    mutex_t mtx;
} logger;

static logger g_log;
static atomic_int g_level = LOG_INFO;
static atomic_int g_ready;

static int log_init_once(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_ready, &expected, 1))
        atomic_store(&g_ready, mutex_init(&g_log.mtx) == 0 ? 2 : -1);
    while (atomic_load(&g_ready) == 1) {
        /* The elected initializer only creates the mutex. */
    }
    return atomic_load(&g_ready) == 2;
}

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL",
};

const char *log_level_name(loglevel lvl) {
    if (lvl < LOG_TRACE || lvl > LOG_FATAL)
        return "?";
    return level_names[lvl];
}

static const char *level_colors[] = {
    "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m",
};

void log_set_level(loglevel lvl) {
    if (lvl >= LOG_TRACE && lvl <= LOG_OFF)
        atomic_store(&g_level, lvl);
}

static void log_line_to(FILE *f, int color, loglevel lvl, const char *text) {
    char ts[32];
    time_now_str(ts, sizeof(ts));
    if (color) {
        fprintf(f, "%s %s%-5s\x1b[0m %s\n", ts, level_colors[lvl], level_names[lvl], text);
    } else {
        fprintf(f, "%s %-5s %s\n", ts, level_names[lvl], text);
    }
}

void log_write(loglevel lvl, const char *fmt, ...) {
    char text[16384];
    char *expanded = NULL;
    const char *message = text;
    va_list ap, copy;
    int use_color;

    if (!fmt || lvl < LOG_TRACE || lvl > LOG_FATAL || (int)lvl < atomic_load(&g_level))
        return;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    /* Redact the complete message: cutting a credential in half before the
     * scan can defeat a pattern. Bound diagnostic allocations to 1 MiB. */
    if (n < 0)
        message = "[log message omitted: formatting failed]";
    else if ((size_t)n >= sizeof(text)) {
        if (n <= 1024 * 1024)
            expanded = malloc((size_t)n + 1);
        if (expanded && vsnprintf(expanded, (size_t)n + 1, fmt, copy) == n)
            message = expanded;
        else
            message = "[log message omitted: too large or allocation failed]";
    }
    va_end(copy);
    char *clean = secret_redact_text(message, strlen(message), NULL);
    free(expanded);
    if (!log_init_once()) {
        free(clean);
        return;
    }

    mutex_lock(&g_log.mtx);
    use_color = g_log.color && !g_log.file;
    message = clean ? clean : "[log message omitted: redaction failed]";
    log_line_to(stderr, use_color, lvl, message);
    if (g_log.file)
        log_line_to(g_log.file, 0, lvl, message);
    mutex_unlock(&g_log.mtx);
    free(clean);
}
