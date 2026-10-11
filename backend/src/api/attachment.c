#include "api/attachment.h"
#include "api/attachment_extractor.h"
#include "api/auth.h"
#include "api/websocket.h"
#include "memory/memory.h"
#include "os/os_fs.h"
#include "os/os_proc.h"
#include "os/os_time.h"
#include "infra/util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *field(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static int valid_id(const char *id) {
    if (!id || strlen(id) != 32) return 0;
    for (int i=0;i<32;i++) if (!strchr("0123456789abcdef", id[i])) return 0;
    return 1;
}
static void file_path(char out[1200], const char *root, const char *id, const char *ext) {
    char dir[1100], name[64];
    path_join(dir, sizeof dir, root, "uploads");
    snprintf(name, sizeof name, "%s.%s", id, ext);
    path_join(out, 1200, dir, name);
}
static cJSON *load_meta(const char *root, const char *id) {
    if (!valid_id(id)) return NULL;
    char path[1200]; file_path(path, root, id, "json");
    char *s = fs_read_file(path);
    cJSON *o = s ? cJSON_Parse(s) : NULL; free(s);
    if (!o || strcmp(field(o, "id"), id)) { cJSON_Delete(o); return NULL; }
    return o;
}
static int save_meta(const char *root, cJSON *o) {
    char path[1200]; file_path(path, root, field(o,"id"), "json");
    char *s = cJSON_PrintUnformatted(o);
    int rc = s ? fs_write_file(path, s, strlen(s)) : -1; free(s); return rc;
}
static const char *image_mime(const unsigned char *p, size_t n) {
    if (n >= 24 && !memcmp(p,"\x89PNG\r\n\x1a\n",8)) return "image/png";
    if (n >= 4 && p[0]==255 && p[1]==216 && p[2]==255) return "image/jpeg";
    if (n >= 12 && !memcmp(p,"RIFF",4) && !memcmp(p+8,"WEBP",4)) return "image/webp";
    if (n >= 10 && (!memcmp(p,"GIF87a",6) || !memcmp(p,"GIF89a",6))) return "image/gif";
    return NULL;
}

static int text_extension(const char *ext) {
    const char *extensions[]={"","txt","md","csv","json","yaml","yml","log","tsv",
        "c","h","cc","cpp","py","js","ts","tsx","jsx","html","xml","css",
        "sh","ps1","ini","conf","sql","toml","rs","go",NULL};
    for(int i=0;extensions[i];i++) if(!strcmp(ext,extensions[i])) return 1;
    return 0;
}

static int processed_extension(const char *ext) {
    const char *extensions[]={"pdf","doc","docx","ppt","pptx","xlsx","rtf","odt","odp",
        "bmp","tif","tiff","heic","heif","avif","svg",
        "mp4","mov","avi","mkv","webm","m4v","mpeg","mpg","wmv","flv","3gp",NULL};
    for(int i=0;extensions[i];i++) if(!strcmp(ext,extensions[i])) return 1;
    return 0;
}
static const char *source_mime(const char *ext) {
    static const struct { const char *ext, *mime; } types[]={
        {"pdf","application/pdf"},{"doc","application/msword"},
        {"docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"ppt","application/vnd.ms-powerpoint"},
        {"pptx","application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"xlsx","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"rtf","application/rtf"},{"odt","application/vnd.oasis.opendocument.text"},
        {"odp","application/vnd.oasis.opendocument.presentation"},
        {"mp4","video/mp4"},{"m4v","video/mp4"},{"mov","video/quicktime"},
        {"avi","video/x-msvideo"},{"mkv","video/x-matroska"},{"webm","video/webm"},
        {"mpeg","video/mpeg"},{"mpg","video/mpeg"},{"wmv","video/x-ms-wmv"},
        {"flv","video/x-flv"},{"3gp","video/3gpp"},
        {"bmp","image/bmp"},{"tif","image/tiff"},{"tiff","image/tiff"},
        {"heic","image/heic"},{"heif","image/heif"},{"avif","image/avif"},{"svg","image/svg+xml"}
    };
    for(size_t i=0;i<sizeof types/sizeof types[0];i++) if(!strcmp(ext,types[i].ext)) return types[i].mime;
    return NULL;
}
static void remove_frames(const char *root, const char *id) {
    char path[1200], ext[32];
    for(int i=0;i<6;i++) { snprintf(ext,sizeof ext,"frame%d.jpg",i); file_path(path,root,id,ext); fs_remove(path); }
    file_path(path,root,id,"decode.jpg"); fs_remove(path);
}
static int storage_name(const char *name) {
    if(strlen(name)<36) return 0;
    char id[33]; memcpy(id,name,32); id[32]=0;
    if(!valid_id(id)) return 0;
    if(!strcmp(name+32,".raw") || !strcmp(name+32,".txt") || !strcmp(name+32,".json") || !strcmp(name+32,".decode.jpg")) return 1;
    return strlen(name)==43 && !strncmp(name+32,".frame",6) && name[38]>='0' && name[38]<='5' && !strcmp(name+39,".jpg");
}
static cJSON *extract_document(const char *path, const char *ext, const char *prefix, char **error) {
    const char *python = getenv("COA_DOCUMENT_PYTHON");
#ifdef _WIN32
    const char *default_python="python";
#else
    const char *default_python="python3";
#endif
    char *argv[] = {(char *)(python && *python ? python : default_python), "-c", (char *)attachment_extractor,
        (char *)path, (char *)ext, (char *)prefix, NULL};
    proc_popen *p = proc_popen_new_direct(argv, 1);
    if (!p) { *error=xstrdup("Python attachment processor unavailable"); return NULL; }
    int64_t end = time_now_ms()+60000;
    while (proc_popen_alive(p) && time_now_ms()<end) {
        proc_popen_read(p, 50);
        if (strlen(proc_popen_buffer(p)) > 8*1024*1024) break;
    }
    int alive=proc_popen_alive(p);
    if (!alive) while (proc_popen_read(p, 1) && strlen(proc_popen_buffer(p))<=8*1024*1024) {}
    const char *json=proc_popen_buffer(p), *next;
    while((next=strstr(json,"\n{"))!=NULL) json=next+1;
    cJSON *o = alive ? NULL : cJSON_Parse(json);
    if (!o || *field(o,"error")) {
        *error=xstrdup(alive ? "attachment processing time or output limit exceeded" :
            (*field(o,"error") ? field(o,"error") : "attachment processor unavailable or failed"));
        cJSON_Delete(o); o=NULL;
    }
    proc_popen_free(p); return o;
}

cJSON *attachment_store(const char *root, memory *m, const char *name,
                        const char *session, const char *body, size_t len) {
    if (!root || !name || !*name || !body || !len || len>ATTACHMENT_MAX_BYTES) return NULL;
    char dir[1100], id[33], path[1200], base[64];
    path_join(dir,sizeof dir,root,"uploads"); fs_mkdirs(dir);
    do { auth_generate_token(id,16); if(!*id) return NULL; file_path(path,root,id,"json"); } while (fs_exists(path));
    file_path(path,root,id,"raw");
    if (fs_write_file(path,body,len)) return NULL;
    cJSON *o=cJSON_CreateObject();
    cJSON_AddBoolToObject(o,"ok",1); cJSON_AddStringToObject(o,"id",id);
    cJSON_AddStringToObject(o,"name",name); cJSON_AddNumberToObject(o,"size",(double)len);
    cJSON_AddStringToObject(o,"session",session && *session ? session : "default");
    const char *mime=image_mime((const unsigned char *)body,len);
    const char *status="unsupported";
    char *text=NULL, *error=NULL;
    cJSON *processed=NULL;
    char ext[16]=""; const char *dot=strrchr(name,'.');
    if (dot) for (size_t i=0;dot[i+1] && i+1<sizeof ext;i++) ext[i]=(char)tolower((unsigned char)dot[i+1]);
    int needs_processing=processed_extension(ext) || (mime && (len>8*1024*1024 || !strcmp(mime,"image/gif")));
    if(mime && needs_processing) snprintf(ext,sizeof ext,"%s",!strcmp(mime,"image/png")?"png":
        !strcmp(mime,"image/jpeg")?"jpg":!strcmp(mime,"image/gif")?"gif":"webp");
    if (mime && !needs_processing) { status="ready"; cJSON_AddStringToObject(o,"kind","image"); }
    else if (needs_processing) {
        char prefix[1200]; snprintf(prefix,sizeof prefix,"%s",path); prefix[strlen(prefix)-4]=0;
        processed=extract_document(path,ext,prefix,&error);
        if(processed) {
            if(*field(processed,"text")) text=xstrdup(field(processed,"text"));
            const char *keys[]={"kind","frames","warnings","page_count","duration_ms","source_frame_count"};
            for(size_t i=0;i<sizeof keys/sizeof keys[0];i++) {
                cJSON *v=cJSON_GetObjectItemCaseSensitive(processed,keys[i]);
                if(v) cJSON_AddItemToObject(o,keys[i],cJSON_Duplicate(v,1));
            }
        }
        status=processed ? "ready" : "failed";
    } else if (!memchr(body,0,len) && text_extension(ext)) {
        size_t preview=len>1000000 ? 1000000 : len;
        while(preview<len && ((unsigned char)body[preview]&0xc0)==0x80) preview--;
        char *raw=malloc(preview+1);
        if (raw) { memcpy(raw,body,preview); raw[preview]=0; text=str_utf8_sanitize(raw); free(raw); }
        cJSON_AddStringToObject(o,"kind","text");
        if(preview<len) {
            cJSON *warnings=cJSON_AddArrayToObject(o,"warnings");
            cJSON_AddItemToArray(warnings,cJSON_CreateString("Text preview truncated; original file retains all bytes"));
        }
        status=text ? "ready" : "failed";
    }
    int chunks=0;
    if (text) {
        file_path(path,root,id,"txt"); snprintf(base,sizeof base,"upload:%s",id);
        if (fs_write_file(path,text,strlen(text)) || (m && (chunks=memory_index_text(m,base,text))<0)) {
            status="failed"; chunks=0; memory_remove_document(m,base);
            free(error); error=xstrdup("text storage or indexing failed");
        }
    }
    const char *original_type=mime ? mime : source_mime(ext);
    cJSON_AddStringToObject(o,"mime",original_type ? original_type : (text ? "text/plain" : "application/octet-stream"));
    cJSON_AddStringToObject(o,"status",status); cJSON_AddNumberToObject(o,"chunks",chunks);
    if (error) cJSON_AddStringToObject(o,"error",error);
    cJSON_Delete(processed);
    free(text); free(error);
    if(!strcmp(status,"failed")) remove_frames(root,id);
    if (save_meta(root,o)) {
        snprintf(base,sizeof base,"upload:%s",id); memory_remove_document(m,base);
        file_path(path,root,id,"txt"); fs_remove(path); file_path(path,root,id,"raw"); fs_remove(path);
        remove_frames(root,id); cJSON_Delete(o); return NULL;
    }
    return o;
}

cJSON *attachment_list(const char *root) {
    char dir[1100]; path_join(dir,sizeof dir,root,"uploads");
    cJSON *arr=cJSON_CreateArray(); dir_list dl;
    if (fs_list_dir(dir,&dl)) return arr;
    for(size_t i=0;i<dl.count;i++) {
        const char *name=dl.items[i].name;
        if (dl.items[i].is_dir || strlen(name)!=37 || strcmp(name+32,".json")) continue;
        char id[33]; memcpy(id,name,32); id[32]=0;
        cJSON *o=load_meta(root,id); if(o) cJSON_AddItemToArray(arr,o);
    }
    fs_list_free(&dl); return arr;
}

int attachment_delete(const char *root, memory *m, const char *key) {
    char id[33]="";
    if (valid_id(key)) snprintf(id,sizeof id,"%s",key);
    else {
        cJSON *arr=attachment_list(root), *item;
        int count=0;
        cJSON_ArrayForEach(item,arr) if (!strcmp(field(item,"name"),key)) {
            snprintf(id,sizeof id,"%s",field(item,"id")); count++;
        }
        cJSON_Delete(arr);
        if (count>1) return 409;
    }
    char path[1200], base[600];
    if (!*id) {
        /* Compatibility for pre-manifest files; caller has sanitized key. */
        char dir[1100]; path_join(dir,sizeof dir,root,"uploads"); path_join(path,sizeof path,dir,key);
        if(fs_remove(path)) return 404;
        snprintf(base,sizeof base,"upload:%s",key); memory_remove_document(m,base); return 200;
    }
    cJSON *o=load_meta(root,id); if(!o) return 404; cJSON_Delete(o);
    file_path(path,root,id,"json"); if(fs_remove(path)) return 500;
    file_path(path,root,id,"raw"); fs_remove(path); file_path(path,root,id,"txt"); fs_remove(path);
    remove_frames(root,id);
    snprintf(base,sizeof base,"upload:%s",id); memory_remove_document(m,base);
    return 200;
}

char *attachment_snapshot(const char *root, const char *session, const cJSON *ids) {
    if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids)>4) return NULL;
    cJSON *arr=cJSON_CreateArray(), *v;
    cJSON_ArrayForEach(v,ids) {
        cJSON *o=cJSON_IsString(v) ? load_meta(root,v->valuestring) : NULL;
        if (!o || strcmp(field(o,"session"),session && *session ? session : "default") || strcmp(field(o,"status"),"ready")) {
            cJSON_Delete(o); cJSON_Delete(arr); return NULL;
        }
        char path[1200]; const char *id=field(o,"id");
        file_path(path,root,id,"raw"); cJSON_AddStringToObject(o,"original_path",path);
        cJSON *frames=cJSON_GetObjectItemCaseSensitive(o,"frames");
        int has_frames=cJSON_IsArray(frames) && cJSON_GetArraySize(frames)>0;
        if(cJSON_GetArraySize(frames)>6) { cJSON_Delete(o); cJSON_Delete(arr); return NULL; }
        int raw_image=!strncmp(field(o,"mime"),"image/",6) && !has_frames;
        file_path(path,root,id,raw_image?"raw":"txt");
        char *data=NULL;
        if(fs_exists(path)) {
            long long len=fs_file_size(path);
            data=len>0 && len<=ATTACHMENT_MAX_BYTES ? fs_read_file(path) : NULL;
            if(!data) { cJSON_Delete(o); cJSON_Delete(arr); return NULL; }
            cJSON_AddStringToObject(o,"path",path);
            if(raw_image) {
                char *b64=base64_encode((unsigned char *)data,(size_t)len);
                if(!b64) { free(data); cJSON_Delete(o); cJSON_Delete(arr); return NULL; }
                cJSON_AddStringToObject(o,"image_b64",b64); free(b64);
            } else cJSON_AddStringToObject(o,"text",data);
        } else if(!has_frames) { cJSON_Delete(o); cJSON_Delete(arr); return NULL; }
        int index=0; cJSON *frame;
        cJSON_ArrayForEach(frame,frames) {
            char ext[32], expected[64]; snprintf(ext,sizeof ext,"frame%d.jpg",index++);
            snprintf(expected,sizeof expected,"%s.%s",id,ext);
            if(strcmp(field(frame,"file"),expected)) { free(data); cJSON_Delete(o); cJSON_Delete(arr); return NULL; }
            file_path(path,root,id,ext); long long bytes=fs_file_size(path);
            char *jpeg=bytes>0 && bytes<=1024*1024 ? fs_read_file(path):NULL;
            char *b64=jpeg ? base64_encode((unsigned char *)jpeg,(size_t)bytes):NULL; free(jpeg);
            if(!b64) { free(data); cJSON_Delete(o); cJSON_Delete(arr); return NULL; }
            cJSON_AddStringToObject(frame,"image_b64",b64); free(b64);
        }
        free(data); cJSON_AddItemToArray(arr,o);
    }
    char *s=cJSON_PrintUnformatted(arr); cJSON_Delete(arr); return s;
}

void attachment_reindex(const char *root, memory *m) {
    /* Upgrade pre-ID uploads without losing bytes or names. */
    char dir[1100]; path_join(dir,sizeof dir,root,"uploads");
    dir_list dl;
    if(!fs_list_dir(dir,&dl)) {
        for(size_t i=0;i<dl.count;i++) {
            const char *name=dl.items[i].name;
            if(dl.items[i].is_dir || storage_name(name)) continue;
            char path[1200]; path_join(path,sizeof path,dir,name);
            long long size=fs_file_size(path);
            char *data=size>0 && size<=ATTACHMENT_MAX_BYTES ? fs_read_file(path):NULL;
            if(!data) continue;
            cJSON *imported=attachment_store(root,m,name,"default",data,(size_t)size);
            free(data);
            if(imported) {
                char base[600]; snprintf(base,sizeof base,"upload:%s",name);
                memory_remove_document(m,base); fs_remove(path); cJSON_Delete(imported);
            }
        }
        fs_list_free(&dl);
    }
    cJSON *arr=attachment_list(root), *o;
    cJSON_ArrayForEach(o,arr) {
        if (strcmp(field(o,"status"),"ready")) continue;
        char path[1200],base[64]; file_path(path,root,field(o,"id"),"txt");
        char *text=fs_read_file(path); if(!text) continue;
        snprintf(base,sizeof base,"upload:%s",field(o,"id")); memory_index_text(m,base,text); free(text);
    }
    cJSON_Delete(arr);
}
