/* http_dispatch.c — public http_* API dispatcher over registered platform
 * hooks (see os/platform.h). The per-OS backend lives in a platform
 * directory and binds here through os_http_platform_hooks(); the first
 * call to any http_* function registers it. */
#include "os/platform.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "os/os_thread.h"
#include "os/os_time.h"

static _Thread_local http_cancel_check cancel_check;
static _Thread_local void *cancel_context;
void http_set_cancel_check(http_cancel_check check,void *context){cancel_check=check;cancel_context=context;}
http_cancel_check http_get_cancel_check(void **context){if(context)*context=cancel_context;return cancel_check;}
int http_cancel_requested(void){return cancel_check && cancel_check(cancel_context);}
struct http_cancel_guard {http_cancel_check check;void *context;void (*interrupt)(void *);void *handle;thread_t *thread;atomic_int stop,interrupted;};
static void cancellation_worker(void *arg){
    http_cancel_guard *g=arg;
    while(!atomic_load(&g->stop)){
        if(g->check(g->context)){g->interrupt(g->handle);atomic_store(&g->interrupted,1);break;}
        time_sleep_ms(25);
    }
}
http_cancel_guard *http_cancel_watch(void (*interrupt)(void *),void *handle){
    if(!cancel_check)return NULL;http_cancel_guard *g=calloc(1,sizeof(*g));if(!g)return NULL;
    g->check=cancel_check;g->context=cancel_context;g->interrupt=interrupt;g->handle=handle;
    g->thread=thread_create(cancellation_worker,g);if(!g->thread){free(g);return NULL;}return g;
}
int http_cancel_unwatch(http_cancel_guard *g){
    if(!g)return 0;atomic_store(&g->stop,1);thread_join(g->thread);int interrupted=atomic_load(&g->interrupted);free(g);return interrupted;
}

static _Thread_local char g_last_http_error[256];

const char *http_last_error(void) { return g_last_http_error; }
void http_set_last_error(const char *message) {
    snprintf(g_last_http_error, sizeof g_last_http_error, "%s", message ? message : "");
}

static const os_http_hooks *g_http = NULL;

void os_http_register(const os_http_hooks *hooks) {
    if (hooks)
        g_http = hooks;
}

const os_http_hooks *os_http_active(void) {
    if (!g_http)
        g_http = os_http_platform_hooks();
    return g_http;
}

const char *os_http_backend_name(void) {
    const os_http_hooks *h = os_http_active();
    return h && h->name ? h->name : "";
}

http_response *http_post(const char *base_url, const char *path, const char *body,
                         const char *content_type, strmap *extra_headers, int timeout_ms) {
    http_set_last_error(NULL);
    const os_http_hooks *h = os_http_active();
    return h && h->post ? h->post(base_url, path, body, content_type,
                                  extra_headers, timeout_ms)
                        : NULL;
}

http_response *http_get(const char *base_url, const char *path,
                        strmap *extra_headers, int timeout_ms) {
    http_set_last_error(NULL);
    const os_http_hooks *h = os_http_active();
    return h && h->get ? h->get(base_url, path, extra_headers, timeout_ms) : NULL;
}

void http_response_free(http_response *r) {
    const os_http_hooks *h = os_http_active();
    if (h && h->response_free)
        h->response_free(r);
}

http_stream *http_stream_open(const char *base_url, const char *method, const char *path,
                              const char *body, const char *content_type,
                              strmap *extra_headers, int timeout_ms) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_open ? h->stream_open(base_url, method, path, body,
                                                content_type, extra_headers,
                                                timeout_ms)
                               : NULL;
}

int http_stream_status(http_stream *s) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_status ? h->stream_status(s) : -1;
}

int http_stream_read_line(http_stream *s, char *out, size_t cap) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_read_line ? h->stream_read_line(s, out, cap) : -1;
}

int http_stream_read(http_stream *s, char *out, size_t cap) {
    const os_http_hooks *h = os_http_active();
    return h && h->stream_read ? h->stream_read(s, out, cap) : -1;
}

void http_stream_close(http_stream *s) {
    const os_http_hooks *h = os_http_active();
    if (h && h->stream_close)
        h->stream_close(s);
}
