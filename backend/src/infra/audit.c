#include "cagent/infra/audit.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_time.h"
#include "cagent/os/os_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ca_audit {
    FILE *f;
    ca_mutex mtx;
};

ca_audit *ca_audit_open(const char *path) {
    ca_audit *a = calloc(1, sizeof(ca_audit));
    if (!a) return NULL;
    a->f = fopen(path, "a");
    if (!a->f) { free(a); return NULL; }
    ca_mutex_init(&a->mtx);
    return a;
}

void ca_audit_log(ca_audit *a, const char *action, const char *subject,
                  const char *result, const char *detail_json) {
    if (!a || !a->f) return;
    char ts[40];
    ca_time_now_iso(ts, sizeof(ts));
    /* Escape detail_json minimally: strip raw newlines/tabs. */
    const char *detail = detail_json ? detail_json : "";
    char *esc = NULL;
    size_t dl = strlen(detail);
    if (dl > 0 && (strchr(detail, '\n') || strchr(detail, '\t') || strchr(detail, '"'))) {
        ca_strbuf sb;
        ca_strbuf_init(&sb);
        for (size_t i = 0; i < dl; i++) {
            char ch = detail[i];
            if (ch == '"') ca_strbuf_append(&sb, "\\\"");
            else if (ch == '\\') ca_strbuf_append(&sb, "\\\\");
            else if (ch == '\n') ca_strbuf_append(&sb, "\\n");
            else if (ch == '\t') ca_strbuf_append(&sb, "\\t");
            else ca_strbuf_append_n(&sb, &ch, 1);
        }
        esc = ca_strbuf_detach(&sb);
        detail = esc;
    }
    ca_mutex_lock(&a->mtx);
    fprintf(a->f, "{\"ts\":\"%s\",\"action\":\"%s\",\"subject\":\"%s\",\"result\":\"%s\",\"detail\":%s}\n",
            ts,
            action ? action : "",
            subject ? subject : "",
            result ? result : "",
            detail);
    fflush(a->f);
    ca_mutex_unlock(&a->mtx);
    free(esc);
}

void ca_audit_close(ca_audit *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    ca_mutex_destroy(&a->mtx);
    free(a);
}
