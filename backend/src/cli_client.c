/* Terminal client of the existing Runtime HTTP API; never calls a model directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "os/http.h"
#include "os/os_time.h"
#include "os/os_socket.h"
#include "cJSON.h"

static volatile sig_atomic_t interrupted;
static void on_interrupt(int sig) { (void)sig; interrupted=1; }
static const char *sval(cJSON *o,const char *key) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    return cJSON_IsString(v)?v->valuestring:"";
}
static double nval(cJSON *o,const char *key) {
    cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsNumber(v)?v->valuedouble:0;
}
/* Render untrusted model/tool text without interpreting terminal control sequences. */
static void text_out(FILE *f,const char *s) {
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)
        if(*p=='\n'||*p=='\t'||(*p>=32&&*p!=127)) fputc(*p,f);
}
#include "terminal_screen.h"
static cJSON *request(const char *base,strmap *headers,const char *method,const char *path,cJSON *body) {
    char *encoded=body?cJSON_PrintUnformatted(body):NULL;
    http_response *r=NULL;
    if(!strcmp(method,"GET"))r=http_get(base,path,headers,5000);
    else if(!strcmp(method,"POST"))r=http_post(base,path,encoded?encoded:"{}","application/json",headers,5000);
    else {
        http_stream *stream=http_stream_open(base,method,path,NULL,NULL,headers,5000);
        if(stream){int status=http_stream_status(stream);http_stream_close(stream);free(encoded);
            if(status>=200&&status<300)return cJSON_CreateObject();
            fprintf(stderr,"HTTP %d\n",status);return NULL;}
    }
    free(encoded);
    if(!r){fprintf(stderr,"Runtime connection failed; check --connect and start the server.\n");return NULL;}
    cJSON *o=cJSON_Parse(r->body?r->body:"");
    if(r->status<200||r->status>=300){fprintf(stderr,"HTTP %d: ",r->status);text_out(stderr,sval(o,"error"));fputc('\n',stderr);cJSON_Delete(o);o=NULL;}
    else if(!o)fprintf(stderr,"Runtime returned invalid JSON.\n");
    http_response_free(r);return o;
}
static char *input_line(void) {
    size_t n=0,cap=256;char *s=malloc(cap);if(!s)return NULL;int c;
    while((c=getchar())!=EOF&&c!='\n'){
        if(n+1==cap){cap*=2;char *p=realloc(s,cap);if(!p){free(s);return NULL;}s=p;}
        s[n++]=(char)c;
    }
    if(c==EOF&&!n){free(s);return NULL;}if(n&&s[n-1]=='\r')n--;s[n]=0;return s;
}
static int valid_session(const char *s){
    if(!*s||strlen(s)>200)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'))return 0;
    return 1;
}
static int new_session(const char *base,strmap *headers,char **session,const char *workspace){
    cJSON *body=cJSON_CreateObject();if(workspace)cJSON_AddStringToObject(body,"workspace",workspace);
    cJSON *o=request(base,headers,"POST","/v1/chat/sessions",body);cJSON_Delete(body);
    const char *id=sval(o,"id");int ok=valid_session(id);
    if(ok){char *copy=malloc(strlen(id)+1);if(!copy)ok=0;else {strcpy(copy,id);free(*session);*session=copy;fprintf(stderr,"Session: %s\n",copy);}}
    cJSON_Delete(o);return ok?0:1;
}
static int watch(const char *base,strmap *headers,long long id){
    signal(SIGINT,on_interrupt); /* Windows CRT resets the handler after delivery. */
    char path[96],last[512]="";int shown=0,cancelled=0;unsigned generation=0;strbuf displayed={0};snprintf(path,sizeof(path),"/v1/tasks/%lld",id);
    fprintf(stderr,"Task #%lld\n",id);
    for(;;){
        if(interrupted&&!cancelled){cJSON *o=request(base,headers,"DELETE",path,NULL);if(!o){strbuf_free(&displayed);return 1;}cJSON_Delete(o);cancelled=1;fprintf(stderr,"Cancellation requested for task #%lld.\n",id);}
        cJSON *o=request(base,headers,"GET",path,NULL);if(!o){if(interrupted&&!cancelled)continue;fprintf(stderr,"Task #%lld remains on the server; use /resume only after inspecting its state.\n",id);strbuf_free(&displayed);return 1;}
        char label[512];snprintf(label,sizeof(label),"[%s] round %.0f | model %.0f | tools %.0f | %.1fs | %s",sval(o,"status"),nval(o,"round"),nval(o,"llm_calls"),nval(o,"tool_calls"),nval(o,"elapsed_ms")/1000,sval(o,"cur_tool"));
        /* Emit changed milestones, not a new line for every elapsed-time tick. */
        char key[512];snprintf(key,sizeof(key),"%s/%.0f/%.0f/%.0f/%s",sval(o,"status"),nval(o,"round"),nval(o,"llm_calls"),nval(o,"tool_calls"),sval(o,"cur_tool"));
        if(strcmp(key,last)){text_out(stderr,label);fputc('\n',stderr);strcpy(last,key);}
        cJSON *steps=cJSON_GetObjectItemCaseSensitive(o,"steps");int count=cJSON_GetArraySize(steps);
        for(;shown<count;shown++){cJSON *step=cJSON_GetArrayItem(steps,shown);fprintf(stderr,"  tool: ");text_out(stderr,sval(step,"tool"));fprintf(stderr," | %s | %.0fms\n",nval(step,"ok")?"ok":"failed",nval(step,"ms"));text_out(stderr,sval(step,"args"));fputc('\n',stderr);text_out(stderr,sval(step,"out"));fputc('\n',stderr);}
        const char *status=sval(o,"status");
        const char *preview=sval(o,"answer_preview");unsigned turn=(unsigned)nval(o,"preview_generation");
        if(turn!=generation){if(displayed.len)fputc('\n',stdout);strbuf_free(&displayed);generation=turn;}
        size_t bytes=strlen(preview);
        if(bytes>displayed.len && (!displayed.len||!memcmp(preview,displayed.buf,displayed.len))){
            if(!displayed.len)fputs("[Provisional answer — pending verification]\n",stderr);
            text_out(stdout,preview+displayed.len);fflush(stdout);strbuf_append(&displayed,preview+displayed.len);
        }
        if(strcmp(status,"RUNNING")&&strcmp(status,"QUEUED")){
            const char *answer=sval(o,"output");size_t size=strlen(answer);
            if(displayed.len && size>=displayed.len && !memcmp(answer,displayed.buf,displayed.len))text_out(stdout,answer+displayed.len);
            else {if(displayed.len)fputs("\n[Final recorded result]\n",stdout);text_out(stdout,answer);}
            fputc('\n',stdout);strbuf_free(&displayed);
            if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"usage_available")))fprintf(stderr,"Tokens: input %.0f / output %.0f\n",nval(o,"tokens_in"),nval(o,"tokens_out"));
            else fprintf(stderr,"Tokens: unavailable\n");
            int rc=!strcmp(status,"DONE")?0:1;cJSON_Delete(o);return rc;
        }
        cJSON_Delete(o);time_sleep_ms(300);
    }
}
int cli_client_main(int argc,char **argv,const char *workspace){
    const char *base="http://127.0.0.1:18300",*prompt=NULL;char *session=NULL,*changed_workspace=NULL;int rc=0,tui=1,force_tui=0,stream=1;
    for(int i=0;i<argc;i++){
        if(!strcmp(argv[i],"--help")){puts("chat [--connect URL] [--session ID] [-p PROMPT] [--tui|--no-tui] [--no-stream]\nCommands: /new /sessions /session ID /workspace PATH /resume ID /help /exit\nInteractive terminals use an alternate-screen layout; pipes stay plain.\nNative OpenAI answers stream provisionally; completion still requires verification.\nCtrl+C cancels the active task. Authentication: COA_CLIENT_TOKEN environment variable.");return 0;}
        if(!strcmp(argv[i],"--no-tui"))tui=0;
        else if(!strcmp(argv[i],"--tui")){tui=1;force_tui=1;}
        else if(!strcmp(argv[i],"--no-stream"))stream=0;
        else if(i+1<argc&&!strcmp(argv[i],"--connect"))base=argv[++i];
        else if(i+1<argc&&!strcmp(argv[i],"-p"))prompt=argv[++i];
        else if(i+1<argc&&!strcmp(argv[i],"--session")){const char *s=argv[++i];if(!valid_session(s)){free(session);return 2;}free(session);session=malloc(strlen(s)+1);if(!session)return 1;strcpy(session,s);}
        else {fprintf(stderr,"Unknown or incomplete chat option: %s\n",argv[i]);free(session);return 2;}
    }
    if(force_tui&&(prompt||!screen_available())){fprintf(stderr,"--tui requires an interactive terminal, without -p.\n");free(session);return 2;}
    if(sock_init()!=0){free(session);return 1;}
    strmap headers={0};const char *token=getenv("COA_CLIENT_TOKEN");
    if(token&&*token){char *auth=malloc(strlen(token)+8);if(!auth){free(session);return 1;}sprintf(auth,"Bearer %s",token);strmap_set(&headers,"Authorization",auth);free(auth);}
    void (*previous)(int)=signal(SIGINT,on_interrupt);interrupted=0;
    if(tui&&!prompt)screen_start();
    if(!session&&new_session(base,&headers,&session,workspace)){rc=1;goto done;}
    fprintf(stderr,"Cognitive OS terminal client · ");text_out(stderr,base);fputs("\n/help for commands; Ctrl+C cancels a running task.\n",stderr);
    for(;;){
        if(!prompt){screen_context(session,workspace);screen_prompt();}char *line=prompt?NULL:input_line();const char *p=prompt?prompt:line;
        if(line)screen_submit(line);
        if(!p){free(line);break;}if(!*p){free(line);if(prompt){rc=2;break;}continue;}interrupted=0;
        if(!strcmp(p,"/exit")||!strcmp(p,"/quit")){free(line);break;}
        if(!strcmp(p,"/help"))puts("/new  /sessions  /session ID  /workspace PATH  /resume ID  /exit\nSend a task directly. Ctrl+C requests cancellation; the server keeps its records.");
        else if(!strcmp(p,"/new"))rc=new_session(base,&headers,&session,workspace);
        else if(!strcmp(p,"/sessions")){cJSON *o=request(base,&headers,"GET","/v1/chat/sessions",NULL);if(o){char *s=cJSON_Print(o);if(s){text_out(stdout,s);puts("");free(s);}cJSON_Delete(o);}else rc=1;}
        else if(!strncmp(p,"/session ",9)){if(valid_session(p+9)){char *copy=malloc(strlen(p+9)+1);if(copy){strcpy(copy,p+9);free(session);session=copy;printf("Session: %s\n",session);}}else fprintf(stderr,"Invalid session ID.\n");}
        else {
            char path[300];cJSON *body=cJSON_CreateObject();int resume=!strncmp(p,"/resume ",8),change=!strncmp(p,"/workspace ",11);
            if(change){snprintf(path,sizeof(path),"/v1/chat/sessions/%s/workspace",session);cJSON_AddStringToObject(body,"workspace",p+11);cJSON *reg=cJSON_CreateObject();cJSON_AddStringToObject(reg,"path",p+11);cJSON *saved=request(base,&headers,"POST","/v1/workspaces",reg);cJSON_Delete(reg);if(!saved){cJSON_Delete(body);free(line);if(prompt){rc=1;break;}continue;}cJSON_Delete(saved);}
            else if(resume){char *end;long long id=strtoll(p+8,&end,10);if(end==p+8||*end||id<0){fprintf(stderr,"Usage: /resume ID\n");cJSON_Delete(body);free(line);if(prompt){rc=2;break;}continue;}snprintf(path,sizeof(path),"/v1/tasks/%lld/resume",id);}
            else if(*p=='/'){fprintf(stderr,"Unknown command; use /help.\n");cJSON_Delete(body);free(line);if(prompt){rc=2;break;}continue;}
            else {strcpy(path,"/v1/chat");cJSON_AddStringToObject(body,"session",session);cJSON_AddStringToObject(body,"message",p);cJSON_AddBoolToObject(body,"stream",stream);}
            cJSON *o=request(base,&headers,"POST",path,body);cJSON_Delete(body);
            if(o){if(!change){cJSON *id=cJSON_GetObjectItemCaseSensitive(o,"id");rc=cJSON_IsNumber(id)?watch(base,&headers,(long long)id->valuedouble):1;}else {char *copy=malloc(strlen(p+11)+1);if(copy){strcpy(copy,p+11);free(changed_workspace);workspace=changed_workspace=copy;}puts("Workspace updated for future tasks.");}cJSON_Delete(o);}else rc=1;
        }
        free(line);if(prompt)break;
    }
done:
    screen_end();signal(SIGINT,previous);strmap_free(&headers);free(session);free(changed_workspace);sock_cleanup();return rc;
}
