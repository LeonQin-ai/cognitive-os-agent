#include "cognitive-os-agent/llm/sse.h"
#include "cognitive-os-agent/os/http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct sse {
    http_stream *h;
};

sse *sse_start(const char *base_url, const char *path, const char *body, const char *content_type,
                       strmap *extra_headers, int timeout_ms) {
    http_stream *h = http_stream_open(base_url, "POST", path, body, content_type, extra_headers, timeout_ms);
    sse *s;

    if (!h)
        return NULL;
    s = malloc(sizeof(sse));
    if (!s) {
        http_stream_close(h);
        return NULL;
    }

    s->h = h;
    return s;
}

int sse_status(const sse *s) {
    return s ? http_stream_status(s->h) : 0;
}

int sse_next(sse *s, char *out, size_t cap) {
    char line[8192];
    for (;;) {
        int n = http_stream_read_line(s->h, line, sizeof(line));
        if (n < 0)
            return 0; /* EOF / connection end */
        if (strncmp(line, "data:", 5) == 0) {
            const char *payload = line + 5;
            while (*payload == ' ')
                payload++;
            if (strcmp(payload, "[DONE]") == 0)
                return 0;
            snprintf(out, cap, "%s", payload);
            return 1;
        }
        /* ignore event:/id:/heartbeat lines */
    }
}

void sse_close(sse *s) {
    if (!s)
        return;
    http_stream_close(s->h);
    free(s);
}
