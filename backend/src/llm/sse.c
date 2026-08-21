#include "cagent/llm/sse.h"
#include "cagent/os/http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct ca_sse {
    ca_http_stream *h;
};

ca_sse *ca_sse_start(const char *base_url, const char *path, const char *body,
                     const char *content_type, ca_strmap *extra_headers, int timeout_ms) {
    ca_http_stream *h = ca_http_stream_open(base_url, "POST", path, body, content_type,
                                            extra_headers, timeout_ms);
    if (!h) return NULL;
    ca_sse *s = malloc(sizeof(ca_sse));
    if (!s) { ca_http_stream_close(h); return NULL; }
    s->h = h;
    return s;
}

int ca_sse_status(const ca_sse *s) { return s ? ca_http_stream_status(s->h) : 0; }

int ca_sse_next(ca_sse *s, char *out, size_t cap) {
    char line[8192];
    for (;;) {
        int n = ca_http_stream_read_line(s->h, line, sizeof(line));
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

void ca_sse_close(ca_sse *s) {
    if (!s) return;
    ca_http_stream_close(s->h);
    free(s);
}
