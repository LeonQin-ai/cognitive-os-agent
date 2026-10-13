/* Real runtime scheduling/context/Flow/rollback benchmark; no remote LLM. */
#include "cognitive-os-agent.h"
#include "runtime/flow.h"
#include "infra/logging.h"
#include "os/os_time.h"
#include "os/os_fs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

typedef struct work {
    agent_pool *pool;
    unsigned char *context;
    size_t context_size;
    int agent, fail;
    int64_t submitted_us, latency_us;
    int ok;
} work;

static void runner(task *t, scheduler *s, void *ud) {
    (void)s; (void)ud;
    work *w=t->userdata;
    w->latency_us=time_now_us()-w->submitted_us;
    char name[32]; snprintf(name,sizeof name,"bench-%d",w->agent);
    char *prompt=agent_pool_task_prompt(w->pool,name,t->input);
    unsigned sum=0;
    for (size_t i=0;i<w->context_size;i+=4096) sum+=w->context[i];
    w->ok=prompt && sum && !w->fail;
    free(prompt);
    t->status=w->ok ? TS_DONE : TS_FAILED;
    t->output=xstrdup(w->ok ? "context verified" : "injected failure");
}

static size_t peak_rss(void) {
#if defined(_WIN32)
    HMODULE lib=LoadLibraryW(L"psapi.dll");
    if (!lib) return 0;
    typedef BOOL (WINAPI *mem_fn)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    mem_fn fn=(mem_fn)GetProcAddress(lib,"GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS counters={0}; counters.cb=sizeof counters;
    size_t value=fn && fn(GetCurrentProcess(),&counters,sizeof counters) ? counters.PeakWorkingSetSize : 0;
    FreeLibrary(lib); return value;
#else
    struct rusage r;
    if (getrusage(RUSAGE_SELF,&r)) return 0;
#if defined(__APPLE__)
    return (size_t)r.ru_maxrss;
#else
    return (size_t)r.ru_maxrss*1024;
#endif
#endif
}
static int compare_latency(const void *a,const void *b) {
    int64_t x=*(const int64_t *)a,y=*(const int64_t *)b; return (x>y)-(x<y);
}

int main(int argc,char **argv) {
    if(argc!=5) { fprintf(stderr,"usage: bench-runtime STATE_DIR TASKS CONTEXT_MIB DAG_DEPTH\n"); return 2; }
    int count=atoi(argv[2]),mib=atoi(argv[3]),depth=atoi(argv[4]);
    if(count<100 || count>100000 || mib<1 || mib>2048 || depth<1 || depth>64) return 2;
    const int agents=100,workers=8;
    config cfg={0}; cfg.state_root=argv[1]; cfg.workspace=argv[1]; cfg.provider="mock"; cfg.http_port=0;
    runtime_ctx ctx; if(init(&ctx,&cfg)) return 1;
    log_set_level(LOG_ERROR);
    work *jobs=calloc((size_t)count,sizeof(*jobs));
    int64_t *latencies=calloc((size_t)count,sizeof(*latencies));
    size_t bytes=(size_t)mib*1024*1024,each=bytes/(size_t)agents;
    unsigned char *context=malloc(bytes);
    if(!jobs || !latencies || !context) { fprintf(stderr,"context allocation failed\n"); return 1; }
    memset(context,1,bytes); /* fault in all context pages, not a virtual reservation */
    for(int i=0;i<agents;i++) {
        char name[32]; snprintf(name,sizeof name,"bench-%d",i);
        if(agent_pool_add(ctx.agents,name,"runtime benchmark worker")<0) return 1;
    }
    scheduler *s=scheduler_new_limited(workers,workers,count,runner,NULL);
    if(!s) return 1;
    scheduler_set_history_limits(s,64,1024*1024);
    int64_t start=time_now_us();
    for(int i=0;i<count;i++) {
        jobs[i].pool=ctx.agents; jobs[i].agent=i%agents;
        jobs[i].context=context+(size_t)jobs[i].agent*each; jobs[i].context_size=each;
        jobs[i].submitted_us=time_now_us();
        if(scheduler_submit(s,0,"verify assigned agent context",&jobs[i],0)<0) return 1;
    }
    if(scheduler_wait_idle(s,60000)) return 1;
    int64_t elapsed=time_now_us()-start;
    for(int i=0;i<count;i++) { if(!jobs[i].ok) return 1; latencies[i]=jobs[i].latency_us; }
    qsort(latencies,(size_t)count,sizeof(*latencies),compare_latency);
    scheduler_stats stats; scheduler_get_stats(s,&stats);
    work recovery=jobs[0]; recovery.fail=1;
    int64_t bad_id=scheduler_submit(s,0,"injected failure",&recovery,0);
    if(bad_id<0 || scheduler_wait_idle(s,5000)) return 1;
    task *bad=scheduler_acquire(s,bad_id);
    int failed=bad && bad->status==TS_FAILED; if(bad) scheduler_release(s,bad);
    if(!failed) return 1;
    recovery.fail=0; recovery.submitted_us=time_now_us();
    if(scheduler_submit(s,0,"recover after failed task",&recovery,0)<0 || scheduler_wait_idle(s,5000) || !recovery.ok) return 1;
    int64_t recovery_us=time_now_us()-recovery.submitted_us;
    scheduler_free(s);

    strbuf dag; strbuf_init(&dag); strbuf_append(&dag,"{\"nodes\":[");
    for(int i=0;i<depth;i++) strbuf_appendf(&dag,"%s{\"id\":\"n%d\",\"agent\":\"bench-%d\",\"task\":\"hello\"}",i?",":"",i,i);
    strbuf_append(&dag,"],\"edges\":[");
    for(int i=1;i<depth;i++) strbuf_appendf(&dag,"%s[\"n%d\",\"n%d\"]",i>1?",":"",i-1,i);
    strbuf_append(&dag,"]}");
    char *answer=NULL,*trace=NULL;
    start=time_now_us(); int flow_rc=flow_run(&ctx,dag.buf,-1,&answer,&trace);
    int64_t flow_us=time_now_us()-start;
    cJSON *nodes=trace ? cJSON_Parse(trace):NULL;
    if(flow_rc || cJSON_GetArraySize(nodes)!=depth) return 1;
    cJSON_Delete(nodes); free(answer); free(trace); strbuf_free(&dag);

    char file[2048],args[4096]; path_join(file,sizeof file,argv[1],"rollback.txt");
    const char *original="original verified content";
    if(fs_write_file(file,original,strlen(original))) return 1;
    tool_ctx tc={0}; tc.workspace=argv[1]; tc.reg=ctx.tools;
    tx *transaction=tx_begin(ctx.txm,ctx.snapshot,ctx.tools,&tc);
    snprintf(args,sizeof args,"{\"path\":\"rollback.txt\",\"content\":\"changed content\"}");
    if(!transaction || tx_run(transaction,"file_write",args)) return 1;
    /* An actual failed action forces rollback of the preceding write. */
    if(tx_run(transaction,"file_read","{\"path\":\"absent-for-benchmark.txt\"}")==0 || tx_validate(transaction)) return 1;
    start=time_now_us(); int rollback_rc=tx_rollback(transaction); int64_t rollback_us=time_now_us()-start;
    tx_free(transaction);
    char *restored=fs_read_file(file);
    if(rollback_rc || !restored || strcmp(restored,original)) return 1;
    free(restored);
    cJSON *report=cJSON_CreateObject();
#define NUM(k,v) cJSON_AddNumberToObject(report,k,(double)(v))
    cJSON_AddStringToObject(report,"scope","real runtime; local deterministic runner/LLM; no remote inference");
    NUM("agents",agents); NUM("tasks_completed",count); NUM("workers",workers);
    NUM("aggregate_context_bytes",bytes); NUM("peak_rss_bytes",peak_rss());
    NUM("elapsed_ms",elapsed/1000.0); NUM("tasks_per_second",count*1000000.0/elapsed);
    NUM("scheduler_latency_p50_ms",latencies[count/2]/1000.0);
    NUM("scheduler_latency_p95_ms",latencies[(count*95)/100]/1000.0);
    NUM("scheduler_latency_max_ms",latencies[count-1]/1000.0);
    NUM("retained_tasks",stats.retained); NUM("retained_bytes",stats.retained_bytes);
    NUM("dag_depth",depth); NUM("dag_run_ms",flow_us/1000.0);
    NUM("failure_recovery_ms",recovery_us/1000.0); NUM("rollback_ms",rollback_us/1000.0);
    cJSON_AddBoolToObject(report,"rollback_verified",1);
    char *json=cJSON_Print(report); cJSON_Delete(report);
    puts(json); free(json); free(context); free(latencies); free(jobs); runtime_shutdown(&ctx);
    return 0;
}
