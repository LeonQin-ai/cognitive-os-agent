#include "cognitive-os-agent/infra/audit.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_time.h"
#include "cognitive-os-agent/os/os_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct coa_audit {
    FILE *f;
    coa_mutex mtx;
};

coa_audit *coa_audit_open(const char *path) {
    coa_audit *a = calloc(1, sizeof(coa_audit));
    if (!a) return NULL;
    a->f = fopen(path, "a");
    if (!a->f) { free(a); return NULL; }
    coa_mutex_init(&a->mtx);
    return a;
}

void coa_audit_log(coa_audit *a, const char *action, const char *subject,
                  const char *result, const char *detail_json) {
    if (!a || !a->f) return;
    char ts[40];
    coa_time_now_iso(ts, sizeof(ts));
    /* Escape detail_json minimally: strip raw newlines/tabs. */
    const char *detail = detail_json ? detail_json : "";
    char *esc = NULL;
    size_t dl = strlen(detail);
    if (dl > 0 && (strchr(detail, '\n') || strchr(detail, '\t') || strchr(detail, '"'))) {
        coa_strbuf sb;
        coa_strbuf_init(&sb);
        for (size_t i = 0; i < dl; i++) {
            char ch = detail[i];
            if (ch == '"') coa_strbuf_append(&sb, "\\\"");
            else if (ch == '\\') coa_strbuf_append(&sb, "\\\\");
            else if (ch == '\n') coa_strbuf_append(&sb, "\\n");
            else if (ch == '\t') coa_strbuf_append(&sb, "\\t");
            else coa_strbuf_append_n(&sb, &ch, 1);
        }
        esc = coa_strbuf_detach(&sb);
        detail = esc;
    }
    coa_mutex_lock(&a->mtx);
    fprintf(a->f, "{\"ts\":\"%s\",\"action\":\"%s\",\"subject\":\"%s\",\"result\":\"%s\",\"detail\":%s}\n",
            ts,
            action ? action : "",
            subject ? subject : "",
            result ? result : "",
            detail);
    fflush(a->f);
    coa_mutex_unlock(&a->mtx);
    free(esc);
}

void coa_audit_close(coa_audit *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    coa_mutex_destroy(&a->mtx);
    free(a);
}
