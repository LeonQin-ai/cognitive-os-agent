/* Real runtime scheduling/context/Flow/rollback benchmark; no remote LLM. */
#include "cognitive-os-agent.h"
#include "runtime/flow.h"
#include "context/mmu.h"
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
#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif
#endif

typedef struct work {
    agent_pool *pool;
    unsigned char *context;
    size_t context_size;
    int agent, fail;
    int64_t submitted_us, latency_us;
    int ok;
} work;

static _Atomic int release_backlog=1;

static void runner(task *t, scheduler *s, void *ud) {
    (void)s; (void)ud;
    work *w=t->userdata;
    w->latency_us=time_now_us()-w->submitted_us;
    while(!atomic_load(&release_backlog)) time_sleep_ms(1);
    char name[32]; snprintf(name,sizeof name,"bench-%d",w->agent);
    char *prompt=agent_pool_task_prompt(w->pool,name,t->input);
    unsigned sum=0;
    for (size_t i=0;i<64;i++) sum+=w->context[(i*4096)%w->context_size];
    w->ok=prompt && sum && !w->fail;
    free(prompt);
    t->status=w->ok ? TS_DONE : TS_FAILED;
    t->output=xstrdup(w->ok ? "context verified" : "injected failure");
}

static size_t memory_rss(int peak) {
#if defined(_WIN32)
    HMODULE lib=LoadLibraryW(L"psapi.dll");
    if (!lib) return 0;
    typedef BOOL (WINAPI *mem_fn)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    mem_fn fn=(mem_fn)GetProcAddress(lib,"GetProcessMemoryInfo");
    PROCESS_MEMORY_COUNTERS counters={0}; counters.cb=sizeof counters;
    size_t value=fn && fn(GetCurrentProcess(),&counters,sizeof counters) ? (peak ? counters.PeakWorkingSetSize : counters.WorkingSetSize) : 0;
    FreeLibrary(lib); return value;
#else
    if(!peak) {
#if defined(__APPLE__)
        struct proc_taskinfo info;
        return proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &info, sizeof info)==sizeof info ? (size_t)info.pti_resident_size:0;
#else
        FILE *f=fopen("/proc/self/statm","r"); unsigned long total=0,resident=0;
        if(!f) return 0; int ok=fscanf(f,"%lu %lu",&total,&resident)==2; fclose(f);
        return ok ? (size_t)resident*(size_t)sysconf(_SC_PAGESIZE):0;
#endif
    }
    struct rusage r;
    if (getrusage(RUSAGE_SELF,&r)) return 0;
#if defined(__APPLE__)
    return (size_t)r.ru_maxrss;
#else
    return (size_t)r.ru_maxrss*1024;
#endif
#endif
}
static size_t peak_rss(void) { return memory_rss(1); }
static int compare_latency(const void *a,const void *b) {
    int64_t x=*(const int64_t *)a,y=*(const int64_t *)b; return (x>y)-(x<y);
}

static int context_benchmark(const char *root,int a,int b,int c) {
    int sizes[3]={a,b,c}; uint64_t hashes[3]={0}; size_t total=0;
    mem_records *rs=mem_records_new(root); ctx_mmu *mmu=ctx_mmu_new();
    if(!rs || !mmu) return 1;
    for(int i=0;i<3;i++) {
        if(sizes[i]<1 || sizes[i]>1024) return 2;
        size_t bytes=(size_t)sizes[i]*1024*1024; total+=bytes;
        mem_record record={0}; snprintf(record.id,sizeof record.id,"mem_pressure_%d",i);
        snprintf(record.title,sizeof record.title,"Agent %c context",'A'+i);
        snprintf(record.scope,sizeof record.scope,"agent/%c",'A'+i);
        record.type=MEMR_SEMANTIC; record.importance=record.confidence=record.utility=1;
        record.content=malloc(bytes+1); if(!record.content) return 1;
        for(size_t k=0;k<bytes;k++) record.content[k]=(char)(64+k%32);
        record.content[bytes-1]='\n'; record.content[bytes]=0;
        hashes[i]=hash64(record.content,bytes);
        if(mem_records_put(rs,&record)) return 1;
        free(record.content);
    }
    ctx_mmu_config cfg; ctx_mmu_config_default(&cfg); cfg.token_budget=total/4+4096;
    int64_t start=time_now_us(); char *out=ctx_mmu_recall(mmu,rs,NULL,"context pressure",&cfg);
    int64_t first_us=time_now_us()-start; free(out);
    char *stats=ctx_mmu_stats_json(mmu); cJSON *initial=cJSON_Parse(stats); free(stats);
    cfg.token_budget=1; start=time_now_us(); out=ctx_mmu_recall(mmu,rs,NULL,"context pressure",&cfg);
    int64_t shrink_us=time_now_us()-start; free(out);
    stats=ctx_mmu_stats_json(mmu); cJSON *evicted=cJSON_Parse(stats); free(stats);
    if(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(evicted,"token_used"))>1 ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(evicted,"materialized_bytes"))!=0 ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(evicted,"evictions"))<3) return 1;
    cfg.token_budget=4096; out=ctx_mmu_recall(mmu,rs,NULL,"context pressure",&cfg); free(out);
    stats=ctx_mmu_stats_json(mmu); cJSON *overview=cJSON_Parse(stats); free(stats);
    cfg.token_budget=total/4+4096; start=time_now_us(); out=ctx_mmu_recall(mmu,rs,NULL,"context pressure",&cfg);
    int64_t recover_us=time_now_us()-start; free(out);
    stats=ctx_mmu_stats_json(mmu); cJSON *recovered=cJSON_Parse(stats); free(stats);
    if(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(recovered,"rehydrations"))<3 ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(recovered,"hot"))!=3 ||
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(recovered,"materialized_bytes"))<(double)total) return 1;
    ctx_mmu_free(mmu); mem_records_free(rs);
    start=time_now_us(); rs=mem_records_new(root); int64_t reload_us=time_now_us()-start;
    if(!rs || mem_records_count(rs)!=3) return 1;
    for(int i=0;i<3;i++) {
        char id[40]; snprintf(id,sizeof id,"mem_pressure_%d",i);
        const mem_record *record=mem_records_find(rs,id);
        if(!record || strlen(record->content)!=(size_t)sizes[i]*1024*1024 ||
           hash64(record->content,strlen(record->content))!=hashes[i]) return 1;
    }
    cJSON *report=cJSON_CreateObject(); cJSON_AddStringToObject(report,"scope","real Context MMU with canonical persisted records; L1 deterministic overview is lossy, canonical source recovery is checksum verified");
    cJSON_AddNumberToObject(report,"source_bytes",(double)total);
    cJSON_AddNumberToObject(report,"peak_rss_bytes",(double)peak_rss());
    cJSON_AddNumberToObject(report,"first_recall_ms",first_us/1000.0);
    cJSON_AddNumberToObject(report,"budget_shrink_recall_ms",shrink_us/1000.0);
    cJSON_AddNumberToObject(report,"rehydrate_recall_ms",recover_us/1000.0);
    cJSON_AddNumberToObject(report,"canonical_reload_ms",reload_us/1000.0);
    cJSON_AddBoolToObject(report,"canonical_checksums_verified",1);
    cJSON_AddItemToObject(report,"initial",initial); cJSON_AddItemToObject(report,"evicted",evicted);
    cJSON_AddItemToObject(report,"recovered",recovered); cJSON_AddItemToObject(report,"overview",overview);
    char *json=cJSON_Print(report); puts(json); free(json); cJSON_Delete(report); mem_records_free(rs);
    return 0;
}

int main(int argc,char **argv) {
    if(argc==6 && !strcmp(argv[1],"--context")) return context_benchmark(argv[2],atoi(argv[3]),atoi(argv[4]),atoi(argv[5]));
    if(argc!=5 && argc!=7 && argc!=8) { fprintf(stderr,"usage: bench-runtime STATE_DIR TASKS CONTEXT_MIB DAG_DEPTH [AGENTS WORKERS [backlog]]\n"); return 2; }
    int count=atoi(argv[2]),mib=atoi(argv[3]),depth=atoi(argv[4]);
    int agents=argc>=7 ? atoi(argv[5]):100, workers=argc>=7 ? atoi(argv[6]):8;
    int backlog=argc==8 && !strcmp(argv[7],"backlog");
    if(argc==8 && !backlog) return 2;
    if(backlog) atomic_store(&release_backlog,0);
    if(count<100 || count>1000000 || mib<1 || mib>2048 || depth<1 || depth>64 ||
       agents<depth || agents>10000 || workers<1 || workers>256) return 2;
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
    cJSON *samples=cJSON_CreateArray();
    size_t observed_queue=0,observed_retained=0;
    size_t start_rss=memory_rss(0);
    int64_t start=time_now_us();
    for(int i=0;i<count;i++) {
        jobs[i].pool=ctx.agents; jobs[i].agent=i%agents;
        jobs[i].context=context+(size_t)jobs[i].agent*each; jobs[i].context_size=each;
        jobs[i].submitted_us=time_now_us();
        if(scheduler_submit(s,0,"verify assigned agent context",&jobs[i],0)<0) return 1;
        if(i%2048==0 || i==count-1) {
            scheduler_stats point; scheduler_get_stats(s,&point);
            if(point.queued>observed_queue) observed_queue=point.queued;
            if(point.retained>observed_retained) observed_retained=point.retained;
            cJSON *sample=cJSON_CreateObject();
            cJSON_AddNumberToObject(sample,"submitted",i+1);
            cJSON_AddNumberToObject(sample,"elapsed_ms",(time_now_us()-start)/1000.0);
            cJSON_AddNumberToObject(sample,"queued",(double)point.queued);
            cJSON_AddNumberToObject(sample,"active",point.active);
            cJSON_AddNumberToObject(sample,"retained_bytes",(double)point.retained_bytes);
            cJSON_AddNumberToObject(sample,"peak_rss_bytes",(double)peak_rss());
            cJSON_AddNumberToObject(sample,"rss_bytes",(double)memory_rss(0));
            cJSON_AddItemToArray(samples,sample);
        }
    }
    atomic_store(&release_backlog,1);
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
    cJSON_AddBoolToObject(report,"backlog_gate",backlog);
    NUM("agents",agents); NUM("tasks_completed",count); NUM("workers",workers);
    NUM("tasks_submitted",count); NUM("tasks_admitted",count); NUM("tasks_failed",0);
    NUM("tasks_rejected",stats.rejected); NUM("queue_at_end",stats.queued); NUM("active_at_end",stats.active);
    NUM("observed_queue_max",observed_queue); NUM("observed_retained_max",observed_retained);
    NUM("context_probes_per_task",64); NUM("auxiliary_idle_runtime_workers",ctx.workers);
    cJSON_AddItemToObject(report,"submission_samples",samples);
    NUM("aggregate_context_bytes",bytes); NUM("peak_rss_bytes",peak_rss());
    NUM("elapsed_ms",elapsed/1000.0); NUM("tasks_per_second",count*1000000.0/elapsed);
    NUM("scheduler_latency_p50_ms",latencies[count/2]/1000.0);
    NUM("scheduler_latency_p95_ms",latencies[(count*95)/100]/1000.0);
    NUM("scheduler_latency_p99_ms",latencies[(count*99)/100]/1000.0);
    NUM("scheduler_latency_max_ms",latencies[count-1]/1000.0);
    NUM("start_rss_bytes",start_rss); NUM("end_rss_bytes",memory_rss(0));
    NUM("retained_tasks",stats.retained); NUM("retained_bytes",stats.retained_bytes);
    NUM("dag_depth",depth); NUM("dag_run_ms",flow_us/1000.0);
    NUM("failure_recovery_ms",recovery_us/1000.0); NUM("rollback_ms",rollback_us/1000.0);
    cJSON_AddBoolToObject(report,"rollback_verified",1);
    char *json=cJSON_Print(report); cJSON_Delete(report);
    puts(json); free(json); free(context); free(latencies); free(jobs); runtime_shutdown(&ctx);
    return 0;
}
