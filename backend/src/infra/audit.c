#include "cognitive-os-agent/infra/audit.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/os/os_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct audit {
    FILE *f;
    mutex_t mtx;
};

audit *audit_open(const char *path) {
    audit *a = calloc(1, sizeof(audit));
    if (!a)
        return NULL;
    a->f = fopen(path, "a");
    if (!a->f) {
        free(a);
        return NULL;
    }
    mutex_init(&a->mtx);
    return a;
}

void audit_log(audit *a, const char *action, const char *subject, const char *result, const char *detail_json) {
    if (!a || !a->f)
        return;
    char ts[40];
    time_now_iso(ts, sizeof(ts));
    /* Escape detail_json minimally: strip raw newlines/tabs. */
    const char *detail = detail_json ? detail_json : "";
    char *esc = NULL;
    size_t dl = strlen(detail);
    if (dl > 0 && (strchr(detail, '\n') || strchr(detail, '\t') || strchr(detail, '"'))) {
        strbuf sb;
        strbuf_init(&sb);
        for (size_t i = 0; i < dl; i++) {
            char ch = detail[i];
            if (ch == '"')
                strbuf_append(&sb, "\\\"");
            else if (ch == '\\')
                strbuf_append(&sb, "\\\\");
            else if (ch == '\n')
                strbuf_append(&sb, "\\n");
            else if (ch == '\t')
                strbuf_append(&sb, "\\t");
            else
                strbuf_append_n(&sb, &ch, 1);
        }
        esc = strbuf_detach(&sb);
        detail = esc;
    }
    mutex_lock(&a->mtx);
    fprintf(a->f, "{\"ts\":\"%s\",\"action\":\"%s\",\"subject\":\"%s\",\"result\":\"%s\",\"detail\":%s}\n", ts,
            action ? action : "", subject ? subject : "", result ? result : "", detail);
    fflush(a->f);
    mutex_unlock(&a->mtx);
    free(esc);
}

void audit_close(audit *a) {
    if (!a)
        return;
    if (a->f)
        fclose(a->f);
    mutex_destroy(&a->mtx);
    free(a);
}
