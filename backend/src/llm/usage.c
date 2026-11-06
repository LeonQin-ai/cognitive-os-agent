/* usage.c — per-model token accounting. */
#include "llm/usage.h"
#include "os/os_thread.h"
#include "infra/util.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <time.h>

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

typedef struct model_entry {
    char *model;
    long prompt, completion, calls;
    long reasoning;
} model_entry;

struct usage {
    mutex_t mtx;
    model_entry *models;
    size_t count, cap;
    long prompt_total, completion_total, reasoning_total;
    cJSON *breakdown;
    char ledger[2048];
};

usage *usage_new(void) {
    usage *u = (usage *)calloc(1, sizeof(usage));
    if (!u)
        return NULL;
    mutex_init(&u->mtx);
    u->breakdown=cJSON_CreateArray();
    return u;
}

void usage_free(usage *u) {
    if (!u)
        return;
    mutex_lock(&u->mtx);
    for (size_t i = 0; i < u->count; i++)
        free(u->models[i].model);
    free(u->models);
    cJSON_Delete(u->breakdown);
    mutex_unlock(&u->mtx);
    mutex_destroy(&u->mtx);
    free(u);
}

void usage_add(usage *u, const char *model, long prompt_tokens, long completion_tokens) {
    usage_add_ex(u, model, prompt_tokens, completion_tokens, 0);
}

void usage_add_ex(usage *u, const char *model, long prompt_tokens, long completion_tokens,
                  long reasoning_tokens) {
    if (!u || !model)
        return;
    mutex_lock(&u->mtx);
    model_entry *e = NULL;
    for (size_t i = 0; i < u->count; i++)
        if (strcmp(u->models[i].model, model) == 0) {
            e = &u->models[i];
            break;
        }

    if (!e) {
        if (u->count == u->cap) {
            size_t ncap = u->cap ? u->cap * 2 : 8;
            model_entry *nm = (model_entry *)realloc(u->models, ncap * sizeof(model_entry));
            if (!nm) {
                mutex_unlock(&u->mtx);
                return;
            }
            u->models = nm;
            u->cap = ncap;
        }
        e = &u->models[u->count++];
        memset(e, 0, sizeof(*e));
        e->model = xstrdup(model);
    }

    e->prompt += prompt_tokens;
    e->completion += completion_tokens;
    e->calls++;
    if (reasoning_tokens > 0)
        e->reasoning += reasoning_tokens;
    u->prompt_total += prompt_tokens;
    u->completion_total += completion_tokens;
    if (reasoning_tokens > 0)
        u->reasoning_total += reasoning_tokens;
    mutex_unlock(&u->mtx);
}

static void usage_event(usage *u,const char *model,long prompt,long completion,long reasoning,long long task_id,const char *day){
    usage_add_ex(u,model,prompt,completion,reasoning);
    mutex_lock(&u->mtx);cJSON *row=NULL,*it;
    cJSON_ArrayForEach(it,u->breakdown){
        const char *d=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it,"day")),*m=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it,"model"));
        if(d&&m&&!strcmp(d,day)&&!strcmp(m,model)&&cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it,"task_id"))==(double)task_id){row=it;break;}
    }
    if(!row){row=cJSON_CreateObject();cJSON_AddStringToObject(row,"day",day);cJSON_AddStringToObject(row,"model",model);cJSON_AddNumberToObject(row,"task_id",(double)task_id);cJSON_AddItemToArray(u->breakdown,row);}
    const char *names[]={"prompt","completion","reasoning","calls"};long values[]={prompt,completion,reasoning,1};
    for(int i=0;i<4;i++){cJSON *n=cJSON_GetObjectItemCaseSensitive(row,names[i]);double before=cJSON_IsNumber(n)?n->valuedouble:0;cJSON_DeleteItemFromObject(row,names[i]);cJSON_AddNumberToObject(row,names[i],before+values[i]);}
    if(u->ledger[0]){
        cJSON *event=cJSON_CreateObject();cJSON_AddStringToObject(event,"day",day);cJSON_AddStringToObject(event,"model",model);cJSON_AddNumberToObject(event,"task_id",(double)task_id);
        for(int i=0;i<3;i++)cJSON_AddNumberToObject(event,names[i],values[i]);
        char *text=cJSON_PrintUnformatted(event);FILE *f=fopen(u->ledger,"ab");if(f&&text){fputs(text,f);fputc('\n',f);}if(f)fclose(f);free(text);cJSON_Delete(event);
    }
    mutex_unlock(&u->mtx);
}
void usage_add_task(usage *u,const char *model,long prompt,long completion,long reasoning,long long task_id){
    if(!u||!model)return;time_t now=time(NULL);struct tm date;
#ifdef _WIN32
    gmtime_s(&date,&now);
#else
    gmtime_r(&now,&date);
#endif
    char day[16];strftime(day,sizeof(day),"%Y-%m-%d",&date);usage_event(u,model,prompt,completion,reasoning,task_id,day);
}
void usage_set_state(usage *u,const char *root){
    if(!u||!root)return;char path[2048];path_join(path,sizeof(path),root,"usage.jsonl");
    FILE *f=fopen(path,"rb");char line[8192];
    while(f&&fgets(line,sizeof(line),f)){
        cJSON *o=cJSON_Parse(line);const char *day=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"day")),*model=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,"model"));
        if(day&&model)usage_event(u,model,(long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"prompt")),(long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"completion")),(long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"reasoning")),(long long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"task_id")),day);
        cJSON_Delete(o);
    }
    if(f)fclose(f);snprintf(u->ledger,sizeof(u->ledger),"%s",path);
}
long usage_prompt_total(usage *u) {
    long v;

    if (!u)
        return 0;
    mutex_lock(&u->mtx);
    v = u->prompt_total;
    mutex_unlock(&u->mtx);
    return v;
}

long usage_completion_total(usage *u) {
    long v;

    if (!u)
        return 0;
    mutex_lock(&u->mtx);
    v = u->completion_total;
    mutex_unlock(&u->mtx);
    return v;
}

char *usage_json(usage *u) {
    cJSON *root = cJSON_CreateObject();
    cJSON *models = cJSON_CreateObject();
    char *js;

    if (u) {
        mutex_lock(&u->mtx);
        cJSON_AddStringToObject(root,"day_timezone","UTC");
        cJSON_AddItemToObject(root,"breakdown",cJSON_Duplicate(u->breakdown,1));
        for (size_t i = 0; i < u->count; i++) {
            model_entry *e = &u->models[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "prompt", (double)e->prompt);
            cJSON_AddNumberToObject(o, "completion", (double)e->completion);
            cJSON_AddNumberToObject(o, "reasoning", (double)e->reasoning);
            cJSON_AddNumberToObject(o, "calls", (double)e->calls);
            cJSON_AddItemToObject(models, e->model, o);
        }
        cJSON_AddBoolToObject(root, "usage_available", u->count > 0);
        cJSON *tot = cJSON_CreateObject();
        cJSON_AddNumberToObject(tot, "prompt", (double)u->prompt_total);
        cJSON_AddNumberToObject(tot, "completion", (double)u->completion_total);
        cJSON_AddNumberToObject(tot, "reasoning", (double)u->reasoning_total);
        mutex_unlock(&u->mtx);
        cJSON_AddItemToObject(root, "total", tot);
    } else {
        cJSON *tot = cJSON_CreateObject();
        cJSON_AddNumberToObject(tot, "prompt", 0);
        cJSON_AddNumberToObject(tot, "completion", 0);
        cJSON_AddItemToObject(root, "total", tot);
    }

    cJSON_AddItemToObject(root, "models", models);
    js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}
