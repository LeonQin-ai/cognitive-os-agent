/* Incremental decoding of the root complete_task.answer JSON string.
 * Only whole JSON escapes / surrogate pairs are exposed. This is a preview,
 * never authorization to execute a tool or mark a task complete. */
static char *native_answer_prefix(const char *args){
    if(!args)return NULL;
    int depth=0;
    for(const char *p=args;*p;p++){
        if(*p=='{'||*p=='['){depth++;continue;}
        if(*p=='}'||*p==']'){depth--;continue;}
        if(*p!='"')continue;
        const char *start=p++;while(*p&&*p!='"'){if(*p=='\\'&&p[1])p++;p++;}
        if(!*p)return NULL;
        const char *after=p+1;while(*after==' '||*after=='\n'||*after=='\r'||*after=='\t')after++;
        if(depth!=1||*after!=':')continue;
        size_t keylen=(size_t)(p-start+1);char *key=malloc(keylen+1);if(!key)return NULL;
        memcpy(key,start,keylen);key[keylen]=0;cJSON *name=cJSON_Parse(key);free(key);
        int answer=cJSON_IsString(name)&&!strcmp(name->valuestring,"answer");cJSON_Delete(name);
        if(!answer)continue;
        after++;while(*after==' '||*after=='\n'||*after=='\r'||*after=='\t')after++;
        if(*after!='"')return NULL;
        start=after;const char *q=after+1,*safe=q;
        while(*q&&*q!='"'){
            if(*q=='\\'){
                if(!q[1])break;
                if(q[1]=='u'){
                    unsigned value=0;int ok=1;
                    for(int i=2;i<6;i++){unsigned char c=(unsigned char)q[i];if(!c){ok=0;break;}int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(d<0){ok=0;break;}value=value*16+(unsigned)d;}
                    if(!ok)break;
                    if(value>=0xd800&&value<=0xdbff){
                        if(q[6]!='\\'||q[7]!='u')break;
                        unsigned low=0;for(int i=8;i<12;i++){unsigned char c=(unsigned char)q[i];if(!c){ok=0;break;}int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(d<0){ok=0;break;}low=low*16+(unsigned)d;}
                        if(!ok||low<0xdc00||low>0xdfff)break;q+=12;
                    }else q+=6;
                }else q+=2;
            }else q++;
            safe=q;
        }
        size_t n=(size_t)(safe-start);char *quoted=malloc(n+2);if(!quoted)return NULL;
        memcpy(quoted,start,n);quoted[n]='"';quoted[n+1]=0;cJSON *value=cJSON_Parse(quoted);free(quoted);
        char *out=cJSON_IsString(value)?xstrdup(value->valuestring):NULL;cJSON_Delete(value);return out;
    }
    return NULL;
}

static http_response *openai_native_stream(llm *model,const llm_request *req,const char *base,const char *path,const char *body,strmap *headers){
    http_stream *stream=http_stream_open(base,"POST",path,body,"application/json",headers,llm_req_timeout_ms(req));
    if(!stream)return NULL;
    http_response *response=calloc(1,sizeof(*response));if(!response){http_stream_close(stream);return NULL;}
    response->status=http_stream_status(stream);
    strbuf names[128]={{0}},arguments[128]={{0}},ids[128]={{0}},content={0},line={0};
    size_t exposed[128]={0};int highest=-1,done=0,valid=response->status==200;
    int64_t last_preview[128]={0};
    char finish[64]="",buffer[4096];size_t buffer_pos=0,buffer_len=0;long long usage[3]={0};
    while(valid&&!done){
        if(buffer_pos==buffer_len){int n=http_stream_read(stream,buffer,sizeof(buffer));if(n<=0)break;buffer_len=(size_t)n;buffer_pos=0;}
        char c=buffer[buffer_pos++];strbuf_append_n(&line,&c,1);
        if(c!='\n')continue;
        if(strncmp(line.buf,"data:",5)){strbuf_free(&line);continue;}
        char *payload=line.buf+5;while(*payload==' ')payload++;
        size_t len=strlen(payload);while(len&&(payload[len-1]=='\n'||payload[len-1]=='\r'))payload[--len]=0;
        if(!strcmp(payload,"[DONE]")){done=1;strbuf_free(&line);break;}
        cJSON *event=cJSON_Parse(payload);if(!event){valid=0;break;}
        accumulate_usage(model,event,usage);
        cJSON *choice=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(event,"choices"),0);
        cJSON *stop=cJSON_GetObjectItemCaseSensitive(choice,"finish_reason");
        if(cJSON_IsString(stop))snprintf(finish,sizeof(finish),"%s",stop->valuestring);
        cJSON *delta=cJSON_GetObjectItemCaseSensitive(choice,"delta");
        const char *text=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(delta,"content"));if(text)strbuf_append(&content,text);
        cJSON *call;
        cJSON_ArrayForEach(call,cJSON_GetObjectItemCaseSensitive(delta,"tool_calls")){
            cJSON *index=cJSON_GetObjectItemCaseSensitive(call,"index");
            if(!cJSON_IsNumber(index)||index->valuedouble<0||index->valuedouble>127||index->valuedouble!=(int)index->valuedouble){valid=0;break;}
            int i=index->valueint;if(i>highest)highest=i;
            cJSON *fn=cJSON_GetObjectItemCaseSensitive(call,"function");
            const char *id=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call,"id"));
            const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fn,"name"));
            const char *arg=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fn,"arguments"));
            if(id)strbuf_append(&ids[i],id);if(name)strbuf_append(&names[i],name);if(arg)strbuf_append(&arguments[i],arg);
            if(names[i].buf&&!strcmp(names[i].buf,"complete_task")&&time_now_ms()-last_preview[i]>=30){
                char *prefix=native_answer_prefix(arguments[i].buf);
                if(prefix){size_t bytes=strlen(prefix);if(bytes>exposed[i]){last_preview[i]=time_now_ms();req->preview(prefix+exposed[i],req->preview_ud);exposed[i]=bytes;}free(prefix);}
            }
        }
        cJSON_Delete(event);strbuf_free(&line);
        if(model->cancel){valid=0;break;}
    }
    http_stream_close(stream);strbuf_free(&line);
    for(int i=0;i<=highest;i++)if(names[i].buf&&!strcmp(names[i].buf,"complete_task")){
        char *prefix=native_answer_prefix(arguments[i].buf);if(prefix){size_t bytes=strlen(prefix);if(bytes>exposed[i])req->preview(prefix+exposed[i],req->preview_ud);free(prefix);}
    }
    if(response->status!=200)response->body=xstrdup("native stream provider rejected request");
    else if(!valid||!done||!*finish){response->status=502;response->body=xstrdup("incomplete or malformed native stream; no actions executed");}
    else {
        cJSON *root=cJSON_CreateObject(),*choices=cJSON_AddArrayToObject(root,"choices"),*choice=cJSON_CreateObject();cJSON_AddItemToArray(choices,choice);
        cJSON_AddStringToObject(choice,"finish_reason",finish);cJSON *msg=cJSON_AddObjectToObject(choice,"message");cJSON_AddStringToObject(msg,"content",content.buf?content.buf:"");
        if(highest>=0){cJSON *calls=cJSON_AddArrayToObject(msg,"tool_calls");for(int i=0;i<=highest;i++){
            cJSON *call=cJSON_CreateObject();cJSON_AddItemToArray(calls,call);cJSON_AddStringToObject(call,"id",ids[i].buf?ids[i].buf:"");cJSON *fn=cJSON_AddObjectToObject(call,"function");
            cJSON_AddStringToObject(fn,"name",names[i].buf?names[i].buf:"");cJSON_AddStringToObject(fn,"arguments",arguments[i].buf?arguments[i].buf:"");}}
        response->body=cJSON_PrintUnformatted(root);cJSON_Delete(root);
    }
    if(response->body)response->body_len=strlen(response->body);
    for(int i=0;i<128;i++){strbuf_free(&names[i]);strbuf_free(&arguments[i]);strbuf_free(&ids[i]);}strbuf_free(&content);
    return response;
}
