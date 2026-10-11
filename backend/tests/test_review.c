/* Offline regression checks for issues #73-87. Run build.ps1 -Target review. */
#include "runtime/scheduler.h"
#include "memory/memory.h"
#include "api/http_server.h"
#include "os/os_socket.h"
#include "os/os_time.h"
#include "infra/util.h"
#include "cJSON.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed, passed;
#define CHECK(x) do { if(x) passed++; else { failed++; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static _Atomic int gate, entered;
static void runner(task *t, scheduler *s, void *ud) {
    (void)s; entered=1;
    while(gate) time_sleep_ms(5);
    t->output=xstrdup(ud ? (const char *)ud : "finished");
}
static int ping(const http_request *req, http_response *resp, void *ud) {
    (void)req; (void)ud; http_resp_json(resp,"{\"ok\":true}"); return 0;
}
static void serve_thread(void *s) { http_server_serve(s); }
int main(void) {
    gate=1; entered=0;
    scheduler *s=scheduler_new_limited(1,1,100,runner,NULL);
    CHECK(s!=NULL);
    scheduler_submit(s,0,"blocked",NULL,0);
    for(int i=0;i<100 && !entered;i++) time_sleep_ms(5);
    CHECK(entered);
    int64_t start=time_now_ms();
    CHECK(scheduler_shutdown(s,100)==-1);
    CHECK(time_now_ms()-start<1000);
    /* A timed out shutdown must preserve the object for the live runner. */
    scheduler_free(s);
    gate=0;
    CHECK(scheduler_shutdown(s,2000)==0); scheduler_free(s);

    s=scheduler_new_limited(1,1,100,runner,NULL);
    scheduler_set_history_limits(s,8,4096);
    int64_t last=-1;
    for(int i=0;i<100;i++) {
        last=scheduler_submit(s,0,"history",NULL,0);
        CHECK(last>=0); CHECK(scheduler_wait_idle(s,2000)==0);
    }
    CHECK(scheduler_shutdown(s,2000)==0);
    CHECK(scheduler_total(s)<=8);
    CHECK(scheduler_acquire(s,0)==NULL);
    task *t=scheduler_acquire(s,last); CHECK(t && t->id==last && t->status==TS_DONE);
    scheduler_release(s,t); scheduler_free(s);

    /* Acquired readers defer eviction; releasing allows the next completion
     * to reclaim the old task without invalidating its ID lookup. */
    s=scheduler_new_limited(1,1,100,runner,NULL);
    scheduler_set_history_limits(s,1,0);
    int64_t pinned_id=scheduler_submit(s,0,"pinned",NULL,0);
    CHECK(scheduler_wait_idle(s,2000)==0);
    t=scheduler_acquire(s,pinned_id); CHECK(t!=NULL);
    for(int i=0;i<20;i++) {
        scheduler_submit(s,0,"eviction",NULL,0);
        scheduler_wait_idle(s,2000);
    }
    CHECK(t && t->id==pinned_id && !strcmp(t->output,"finished"));
    scheduler_release(s,t);
    scheduler_submit(s,0,"after-release",NULL,0);
    CHECK(scheduler_shutdown(s,2000)==0);
    CHECK(scheduler_acquire(s,pinned_id)==NULL); scheduler_free(s);
    char large[8192]; memset(large,'q',sizeof large-1); large[sizeof large-1]=0;
    s=scheduler_new_limited(1,1,100,runner,large);
    scheduler_set_history_limits(s,0,1024);
    scheduler_submit(s,0,"large-output",NULL,0);
    CHECK(scheduler_shutdown(s,2000)==0);
    scheduler_stats stats; scheduler_get_stats(s,&stats);
    CHECK(stats.retained_bytes<=1024 && scheduler_total(s)==0); scheduler_free(s);

    char root[128]; snprintf(root,sizeof root,"build/review-memory-%lld",(long long)time_now_ms());
    memory *m=memory_new(root);
    char base[320]; memset(base,'a',sizeof base-1); base[sizeof base-1]=0;
    char text[1801]; memset(text,'x',1800); text[1800]=0;
    CHECK(memory_index_text(m,base,text)==3);
    char *hits=memory_retrieve(m,"x",20); cJSON *arr=hits ? cJSON_Parse(hits) : NULL;
    CHECK(cJSON_IsArray(arr) && cJSON_GetArraySize(arr)==3);
    cJSON *o; cJSON_ArrayForEach(o,arr) {
        cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"text");
        CHECK(cJSON_IsString(v) && strlen(v->valuestring)<=600);
    }
    free(hits); cJSON_Delete(arr);
    CHECK(memory_index_text(m,base,"replacement")==1);
    hits=memory_retrieve(m,"x",20); arr=hits ? cJSON_Parse(hits):NULL;
    CHECK(cJSON_IsArray(arr) && cJSON_GetArraySize(arr)==1);
    CHECK(hits && !strstr(hits,"xxxxxxxx")); free(hits); cJSON_Delete(arr);
    memory_remove_document(m,base);
    hits=memory_retrieve(m,"replacement",20); CHECK(hits && !strstr(hits,"replacement")); free(hits);
    strbuf utf; strbuf_init(&utf);
    for(int i=0;i<500;i++) strbuf_append(&utf,"中文");
    CHECK(memory_index_text(m,"utf",utf.buf)==5);
    hits=memory_retrieve(m,"中文",20); arr=cJSON_Parse(hits);
    cJSON_ArrayForEach(o,arr) {
        cJSON *v=cJSON_GetObjectItemCaseSensitive(o,"text");
        CHECK(cJSON_IsString(v) && strlen(v->valuestring)<=600 && strlen(v->valuestring)%3==0);
    }
    free(hits); cJSON_Delete(arr); strbuf_free(&utf); memory_free(m);

    sock_init();
    http_server *http=http_server_new_bind("127.0.0.1",18774);
    CHECK(http!=NULL); if(http) {
        http_server_route(http,"GET","/ping",ping,NULL);
        thread_t *thread=thread_create(serve_thread,http);
        sock *slow=sock_connect("127.0.0.1",18774,1000);
        CHECK(slow!=NULL); if(slow) sock_send(slow,"GET /",5);
        sock *fast=sock_connect("127.0.0.1",18774,1000);
        CHECK(fast!=NULL); if(fast) {
            const char *request="GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
            sock_send(fast,request,strlen(request));
            CHECK(sock_wait_readable(fast,1000)==1); sock_close(fast);
        }
        start=time_now_ms(); http_server_stop(http); thread_join(thread);
        CHECK(time_now_ms()-start<1500);
        if(slow) sock_close(slow); http_server_free(http);
    }
    sock_cleanup();
    printf("review regression: %d passed, %d failed\n",passed,failed);
    return failed ? 1:0;
}
