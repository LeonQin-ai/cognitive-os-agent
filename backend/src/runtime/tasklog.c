/* tasklog.c — durable task journal. See tasklog.h. */
#include "runtime/tasklog.h"
#include "infra/util.h"
#include "os/os_fs.h"
#include "os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "cJSON.h"


struct tasklog {
    mutex_t mtx;
    cJSON *session_states; /* initialized lazily; updated on append */
    char path[600]; /* <state_root>/tasks.jsonl */
};

static void session_state_set(tasklog *tl,int64_t id,const char *session,const char *status){
    const char *key=session && *session?session:"default";
    cJSON *o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"id",(double)id);cJSON_AddStringToObject(o,"status",status?status:"?");
    cJSON_DeleteItemFromObjectCaseSensitive(tl->session_states,key);cJSON_AddItemToObject(tl->session_states,key,o);
}
static void tasklog_path(char *out, size_t n, const char *state_root) {
    char dir[600];

    path_join(dir, sizeof(dir), state_root ? state_root : "state", "journal");
    fs_mkdirs(dir);
    path_join(out, n, dir, "tasks.jsonl");
}

tasklog *tasklog_new(const char *state_root) {
    tasklog *tl = (tasklog *)calloc(1, sizeof(tasklog));

    if (!tl)
        return NULL;
    tasklog_path(tl->path, sizeof(tl->path), state_root);
    mutex_init(&tl->mtx);
    return tl;
}

void tasklog_free(tasklog *tl) {
    if (!tl)
        return;
    cJSON_Delete(tl->session_states);
    mutex_destroy(&tl->mtx);
    free(tl);
}

void tasklog_record(tasklog *tl, int64_t id, const char *status, const char *session, const char *input,
                    const char *output, const char *trace_json, const char *spans_json) {
    tasklog_record_access(tl,id,status,session,input,output,trace_json,spans_json,0);
}
void tasklog_record_access(tasklog *tl,int64_t id,const char *status,const char *session,const char *input,
    const char *output,const char *trace_json,const char *spans_json,int permission_mode){
    cJSON *o;
    char *line;
    FILE *f;

    /* Scheduler task ids begin at zero; only negative ids are invalid. */
    if (!tl || id < 0)
        return;
    o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddStringToObject(o, "status", status ? status : "?");
    cJSON_AddNumberToObject(o,"permission_mode",permission_mode>=0&&permission_mode<=2?permission_mode:1);
    cJSON_AddStringToObject(o, "session", session && *session ? session : "");
    cJSON_AddStringToObject(o, "input", input ? input : "");
    if (output && *output) {
        /* This is the durable answer, not an input-context preview. Cutting
         * here silently destroys the result after navigation/restart and can
         * split a UTF-8 codepoint. Context packing has its own budgets. */
        char *safe = str_utf8_sanitize(output);
        cJSON_AddStringToObject(o, "output", safe ? safe : "");
        free(safe);
    }
    /* task_trace_add already bounds the timeline to 128 events. A byte cap
     * here discarded the entire failed execution record for long tasks. */
    if (trace_json && *trace_json) {
        cJSON *trace = cJSON_Parse(trace_json);
        if (trace && cJSON_IsArray(trace))
            cJSON_AddItemToObject(o, "trace", trace);
        else
            cJSON_Delete(trace);
    }
    if (spans_json && *spans_json) {
        cJSON *spans = cJSON_Parse(spans_json);
        if (spans && cJSON_IsArray(spans))
            cJSON_AddItemToObject(o, "spans", spans);
        else
            cJSON_Delete(spans);
    }
    cJSON_AddNumberToObject(o, "ts", (double)time(NULL) * 1000);
    line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!line)
        return;

    mutex_lock(&tl->mtx);
    f = fopen(tl->path, "a+b");
    if (f) {
        fseek(f,0,SEEK_END);
        long size=ftell(f);
        if(size>0) {
            fseek(f,-1,SEEK_END); int tail=fgetc(f); fseek(f,0,SEEK_END);
            if(tail!='\n') fputc('\n',f); /* isolate an interrupted partial JSON line */
        }
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
        if(tl->session_states)session_state_set(tl,id,session,status);
    }
    mutex_unlock(&tl->mtx);
    free(line);
}

/* Parse up to `limit` (0 = all) newest records; returns a cJSON array or
 * NULL. `pick` (optional) filters by task id (any status). */
static cJSON *journal_parse(const char *path, int limit, int64_t pick_id) {
    FILE *f=fopen(path,"rb");
    if(!f)return NULL;
    cJSON *arr=cJSON_CreateArray();
    size_t cap=4096,used=0; char *buf=malloc(cap);
    if(!arr || !buf){free(buf);cJSON_Delete(arr);fclose(f);return NULL;}
    /* Stream complete records; file growth must not hide all historical answers.
     * Limit the number of records retained, never the text within a record. */
    int ch;
    do {
        ch=fgetc(f);
        if(ch!='\n' && ch!=EOF){
            if(used+1>=cap){
                if(cap>SIZE_MAX/2){free(buf);cJSON_Delete(arr);fclose(f);return NULL;}
                size_t next=cap*2;char *grown=realloc(buf,next);
                if(!grown){free(buf);cJSON_Delete(arr);fclose(f);return NULL;}
                buf=grown;cap=next;
            }
            buf[used++]=(char)ch;continue;
        }
        if(!used)continue;
        buf[used]=0;used=0;
        /* Old versions could cut a UTF-8 sequence inside an answer. */
        char *valid=str_utf8_sanitize(buf);
        cJSON *o=valid?cJSON_Parse(valid):NULL;free(valid);
        if(!o)continue;
        if(pick_id!=INT64_MIN){
            cJSON *id=cJSON_GetObjectItemCaseSensitive(o,"id");
            if(!cJSON_IsNumber(id) || (int64_t)id->valuedouble!=pick_id){cJSON_Delete(o);continue;}
        }
        cJSON_AddItemToArray(arr,o);
        if(limit>0 && cJSON_GetArraySize(arr)>limit)cJSON_Delete(cJSON_DetachItemFromArray(arr,0));
    }while(ch!=EOF);
    free(buf);fclose(f);
    return arr;
}

char *tasklog_json(tasklog *tl, int limit) {
    cJSON *arr;
    char *s;

    if (!tl)
        return xstrdup("[]");
    mutex_lock(&tl->mtx);
    arr = journal_parse(tl->path, limit > 0 ? limit : 0, INT64_MIN);
    mutex_unlock(&tl->mtx);
    if (!arr)
        return xstrdup("[]");
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

char *tasklog_session_states_json(tasklog *tl){
    if(!tl)return xstrdup("{}");mutex_lock(&tl->mtx);
    if(!tl->session_states){
        tl->session_states=cJSON_CreateObject();cJSON *records=journal_parse(tl->path,0,INT64_MIN),*it;
        cJSON_ArrayForEach(it,records){cJSON *id=cJSON_GetObjectItemCaseSensitive(it,"id");
            if(cJSON_IsNumber(id))session_state_set(tl,(int64_t)id->valuedouble,cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it,"session")),cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it,"status")));}
        cJSON_Delete(records);
    }
    char *text=cJSON_PrintUnformatted(tl->session_states);mutex_unlock(&tl->mtx);return text;
}

int tasklog_permission_mode(tasklog *tl,int64_t id){
    if(!tl)return 1;mutex_lock(&tl->mtx);cJSON *arr=journal_parse(tl->path,0,id);mutex_unlock(&tl->mtx);
    if(!arr)return 1;
    int mode=0;cJSON *it;cJSON_ArrayForEach(it,arr){cJSON *v=cJSON_GetObjectItemCaseSensitive(it,"permission_mode");
        if(v)mode=cJSON_IsNumber(v)&&v->valuedouble>=0&&v->valuedouble<=2&&(int)v->valuedouble==v->valuedouble?v->valueint:1;}
    cJSON_Delete(arr);return mode;
}
int tasklog_find(tasklog *tl, int64_t id, char **status, char **session, char **input, char **output) {
    cJSON *arr = NULL, *hit = NULL, *it;
    int found = 0;

    if (!tl || id < 0)
        return 0;
    mutex_lock(&tl->mtx);
    arr = journal_parse(tl->path, 0, id);
    mutex_unlock(&tl->mtx);
    if (!arr)
        return 0;
    cJSON_ArrayForEach(it, arr) {
        cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "status");
        /* only terminal tasks are resumable */
        if (st && cJSON_IsString(st) && st->valuestring)
            hit = it;
    }
    if (hit) {
        found = 1;
        if (status) {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(hit, "status");
            *status = (st && cJSON_IsString(st) && st->valuestring) ? xstrdup(st->valuestring) : NULL;
        }
        if (session) {
            cJSON *se = cJSON_GetObjectItemCaseSensitive(hit, "session");
            *session = (se && cJSON_IsString(se) && se->valuestring && *se->valuestring) ? xstrdup(se->valuestring)
                                                                                         : NULL;
        }
        if (input) {
            cJSON *in = cJSON_GetObjectItemCaseSensitive(hit, "input");
            *input = (in && cJSON_IsString(in) && in->valuestring) ? xstrdup(in->valuestring) : NULL;
        }
        if (output) {
            cJSON *ou = cJSON_GetObjectItemCaseSensitive(hit, "output");
            *output = (ou && cJSON_IsString(ou) && ou->valuestring) ? xstrdup(ou->valuestring) : NULL;
        }
    }
    cJSON_Delete(arr);
    return found;
}

typedef struct recovery_record { int64_t id; size_t order; cJSON *json; } recovery_record;
static int compare_recovery(const void *a,const void *b) {
    const recovery_record *x=a,*y=b;
    if(x->id!=y->id) return (x->id>y->id)-(x->id<y->id);
    return (x->order>y->order)-(x->order<y->order);
}
int tasklog_recover(tasklog *tl,int64_t *next_id) {
    if(next_id) *next_id=0;
    if(!tl) return -1;
    mutex_lock(&tl->mtx); cJSON *arr=journal_parse(tl->path,0,INT64_MIN); mutex_unlock(&tl->mtx);
    if(!arr) return fs_file_size(tl->path)>0 ? -1 : 0; /* never reuse IDs after a read/allocation failure */
    size_t count=(size_t)cJSON_GetArraySize(arr),n=0,order=0;
    if(!count && fs_file_size(tl->path)>0){cJSON_Delete(arr);return -1;} /* corrupt journal is not an empty history */
    recovery_record *records=calloc(count ? count:1,sizeof(*records));
    if(!records) { cJSON_Delete(arr); return -1; }
    cJSON *it;
    cJSON_ArrayForEach(it,arr) {
        cJSON *id=cJSON_GetObjectItemCaseSensitive(it,"id");
        if(cJSON_IsNumber(id) && id->valuedouble>=0 && id->valuedouble<9007199254740991.0 &&
           (double)(int64_t)id->valuedouble==id->valuedouble) {
            records[n++]=(recovery_record){(int64_t)id->valuedouble,order,it};
            if(next_id && *next_id<records[n-1].id+1) *next_id=records[n-1].id+1;
        }
        order++;
    }
    qsort(records,n,sizeof(*records),compare_recovery);
    int interrupted=0;
    for(size_t i=0;i<n;i++) {
        if(i+1<n && records[i].id==records[i+1].id) continue; /* newest transition only */
        cJSON *o=records[i].json;
        const char *status=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"status"));
        if(status && (!strcmp(status,"RUNNING") || !strcmp(status,"QUEUED"))) {
            cJSON *access=cJSON_GetObjectItemCaseSensitive(o,"permission_mode");
            tasklog_record_access(tl,records[i].id,"INTERRUPTED",
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"session")),
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"input")),
                "Runtime restarted before task completion; inspect checkpoint and explicitly resume.",NULL,NULL,
                !access?0:(cJSON_IsNumber(access)&&access->valuedouble>=0&&access->valuedouble<=2&&access->valuedouble==access->valueint?access->valueint:1));
            interrupted++;
        }
    }
    free(records); cJSON_Delete(arr); return interrupted;
}
