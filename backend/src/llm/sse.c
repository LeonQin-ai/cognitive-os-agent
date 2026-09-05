#include "cognitive-os-agent/llm/sse.h"
#include "cognitive-os-agent/os/http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct coa_sse {
    coa_http_stream *h;
};

coa_sse *coa_sse_start(const char *base_url, const char *path, const char *body,
                     const char *content_type, coa_strmap *extra_headers, int timeout_ms) {
    coa_http_stream *h = coa_http_stream_open(base_url, "POST", path, body, content_type,
                                            extra_headers, timeout_ms);
    if (!h) return NULL;
    coa_sse *s = malloc(sizeof(coa_sse));
    if (!s) { coa_http_stream_close(h); return NULL; }
    s->h = h;
    return s;
}

int coa_sse_status(const coa_sse *s) { return s ? coa_http_stream_status(s->h) : 0; }

int coa_sse_next(coa_sse *s, char *out, size_t cap) {
    char line[8192];
    for (;;) {
        int n = coa_http_stream_read_line(s->h, line, sizeof(line));
        if (n < 0) return 0;             /* EOF / connection end */
        if (strncmp(line, "data:", 5) == 0) {
            const char *payload = line + 5;
            while (*payload == ' ') payload++;
            if (strcmp(payload, "[DONE]") == 0) return 0;
            snprintf(out, cap, "%s", payload);
            return 1;
        }
        /* ignore event:/id:/heartbeat lines */
    }
}

void coa_sse_close(coa_sse *s) {
    if (!s) return;
    coa_http_stream_close(s->h);
    free(s);
}
