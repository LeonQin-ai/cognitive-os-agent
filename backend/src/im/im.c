/* im.c — instant messaging store backed by a JSON file.
 * Layout: <state_root>/im/sessions.json
 *   {"next_id":5,"sessions":[{"id":1,"name":"...","created_ms":...,"messages":[
 *     {"id":1,"role":"user","content":"...","ts_ms":...}]}]}
 * The store is guarded by a mutex; every mutation persists to disk. */
#include "cagent/im/im.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_time.h"
#include "cagent/os/os_fs.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

typedef struct im_msg {
    int64_t id;
    char *role;
    char *sender;
    char *content;
    int64_t ts_ms;
} im_msg;

typedef struct im_sess {
    int64_t id;
    char *name;
    char *kind;
    char **members;
    size_t n_members;
    char *channel;
    int64_t created_ms;
    im_msg *msgs;
    size_t count, cap;
} im_sess;

struct ca_im {
    char path[600];
    ca_mutex mtx;
    im_sess *sessions;
    size_t count, cap;
    int64_t next_id;   /* shared id counter for sessions + messages */
};

static im_sess *find_sess(ca_im *im, int64_t id) {
    for (size_t i = 0; i < im->count; i++)
        if (im->sessions[i].id == id) return &im->sessions[i];
    return NULL;
}

static void sess_add_msg(im_sess *s, int64_t id, const char *role, const char *sender,
                         const char *content, int64_t ts) {
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        im_msg *nm = realloc(s->msgs, cap * sizeof(im_msg));
        if (!nm) return;
        s->msgs = nm;
        s->cap = cap;
    }
    s->msgs[s->count].id = id;
    s->msgs[s->count].role = ca_strdup(role);
    s->msgs[s->count].sender = (sender && *sender) ? ca_strdup(sender) : NULL;
    s->msgs[s->count].content = ca_strdup(content);
    s->msgs[s->count].ts_ms = ts;
    s->count++;
}

static void sess_free(im_sess *s) {
    for (size_t i = 0; i < s->count; i++) {
        free(s->msgs[i].role);
        free(s->msgs[i].sender);
        free(s->msgs[i].content);
    }
    free(s->msgs);
    free(s->name);
    free(s->kind);
    free(s->channel);
    for (size_t i = 0; i < s->n_members; i++) free(s->members[i]);
    free(s->members);
}

static void im_persist(ca_im *im) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next_id", (double)im->next_id);
    cJSON *arr = cJSON_AddArrayToObject(root, "sessions");
    for (size_t i = 0; i < im->count; i++) {
        im_sess *s = &im->sessions[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)s->id);
        cJSON_AddStringToObject(o, "name", s->name ? s->name : "");
        cJSON_AddStringToObject(o, "kind", s->kind ? s->kind : "direct");
        cJSON *mem = cJSON_AddArrayToObject(o, "members");
        for (size_t j = 0; j < s->n_members; j++)
            cJSON_AddItemToArray(mem, cJSON_CreateString(s->members[j]));
        if (s->channel) cJSON_AddStringToObject(o, "channel", s->channel);
        cJSON_AddNumberToObject(o, "created_ms", (double)s->created_ms);
        cJSON *ma = cJSON_AddArrayToObject(o, "messages");
        for (size_t j = 0; j < s->count; j++) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddNumberToObject(m, "id", (double)s->msgs[j].id);
            cJSON_AddStringToObject(m, "role", s->msgs[j].role);
            if (s->msgs[j].sender) cJSON_AddStringToObject(m, "sender", s->msgs[j].sender);
            cJSON_AddStringToObject(m, "content", s->msgs[j].content);
            cJSON_AddNumberToObject(m, "ts_ms", (double)s->msgs[j].ts_ms);
            cJSON_AddItemToArray(ma, m);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js) {
        ca_fs_write_file(im->path, js, (size_t)strlen(js));
        free(js);
    }
}

static void im_load(ca_im *im) {
    char *js = ca_fs_read_file(im->path);
    if (!js) return;
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root || !cJSON_IsObject(root)) { if (root) cJSON_Delete(root); return; }
    cJSON *nid = cJSON_GetObjectItemCaseSensitive(root, "next_id");
    if (nid && cJSON_IsNumber(nid)) im->next_id = (int64_t)nid->valuedouble;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "sessions");
    if (arr && cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsObject(it)) continue;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (!id || !cJSON_IsNumber(id)) continue;
            im_sess s;
            memset(&s, 0, sizeof(s));
            s.id = (int64_t)id->valuedouble;
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            s.name = ca_strdup(nm && cJSON_IsString(nm) ? nm->valuestring : "");
            cJSON *kd = cJSON_GetObjectItemCaseSensitive(it, "kind");
            s.kind = ca_strdup(kd && cJSON_IsString(kd) ? kd->valuestring : "direct");
            cJSON *mem = cJSON_GetObjectItemCaseSensitive(it, "members");
            if (mem && cJSON_IsArray(mem)) {
                int mn = cJSON_GetArraySize(mem);
                if (mn > 0) {
                    s.members = calloc((size_t)mn, sizeof(char *));
                    if (s.members) {
                        s.n_members = (size_t)mn;
                        for (int mi = 0; mi < mn; mi++) {
                            cJSON *mv = cJSON_GetArrayItem(mem, mi);
                            s.members[mi] = ca_strdup(mv && cJSON_IsString(mv) ? mv->valuestring : "");
                        }
                    }
                }
            }
            cJSON *cr = cJSON_GetObjectItemCaseSensitive(it, "created_ms");
            s.created_ms = (cr && cJSON_IsNumber(cr)) ? (int64_t)cr->valuedouble : 0;
            cJSON *chn = cJSON_GetObjectItemCaseSensitive(it, "channel");
            s.channel = (chn && cJSON_IsString(chn)) ? ca_strdup(chn->valuestring) : NULL;
            cJSON *ms = cJSON_GetObjectItemCaseSensitive(it, "messages");
            if (ms && cJSON_IsArray(ms)) {
                cJSON *m;
                cJSON_ArrayForEach(m, ms) {
                    if (!cJSON_IsObject(m)) continue;
                    cJSON *mid = cJSON_GetObjectItemCaseSensitive(m, "id");
                    cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
                    cJSON *sender = cJSON_GetObjectItemCaseSensitive(m, "sender");
                    cJSON *cont = cJSON_GetObjectItemCaseSensitive(m, "content");
                    cJSON *ts = cJSON_GetObjectItemCaseSensitive(m, "ts_ms");
                    sess_add_msg(&s,
                        (mid && cJSON_IsNumber(mid)) ? (int64_t)mid->valuedouble : 0,
                        (role && cJSON_IsString(role)) ? role->valuestring : "",
                        (sender && cJSON_IsString(sender)) ? sender->valuestring : NULL,
                        (cont && cJSON_IsString(cont)) ? cont->valuestring : "",
                        (ts && cJSON_IsNumber(ts)) ? (int64_t)ts->valuedouble : 0);
                }
            }
            if (im->count == im->cap) {
                size_t cap = im->cap ? im->cap * 2 : 8;
                im_sess *ns = realloc(im->sessions, cap * sizeof(im_sess));
                if (!ns) { sess_free(&s); break; }
                im->sessions = ns;
                im->cap = cap;
            }
            im->sessions[im->count++] = s;
        }
    }
    cJSON_Delete(root);
}

ca_im *ca_im_new(const char *state_root) {
    ca_im *im = calloc(1, sizeof(ca_im));
    if (!im) return NULL;
    snprintf(im->path, sizeof(im->path), "%s", state_root ? state_root : "state");
    /* state_root/im/sessions.json */
    {
        char dir[600];
        snprintf(dir, sizeof(dir), "%s", im->path);
        ca_fs_mkdirs(dir);
        snprintf(dir, sizeof(dir), "%s/im", im->path);
        ca_fs_mkdirs(dir);
        snprintf(im->path, sizeof(im->path), "%s/im/sessions.json", im->path);
    }
    ca_mutex_init(&im->mtx);
    im->next_id = 1;
    im_load(im);
    return im;
}

void ca_im_free(ca_im *im) {
    if (!im) return;
    ca_mutex_lock(&im->mtx);
    for (size_t i = 0; i < im->count; i++) sess_free(&im->sessions[i]);
    free(im->sessions);
    ca_mutex_unlock(&im->mtx);
    ca_mutex_destroy(&im->mtx);
    free(im);
}

int64_t ca_im_create_session(ca_im *im, const char *name) {
    return ca_im_create_session_ex(im, name, "direct", NULL, 0);
}

int64_t ca_im_create_session_ex(ca_im *im, const char *name, const char *kind,
                                const char **members, size_t n_members) {
    if (!im) return -1;
    ca_mutex_lock(&im->mtx);
    if (im->count == im->cap) {
        size_t cap = im->cap ? im->cap * 2 : 8;
        im_sess *ns = realloc(im->sessions, cap * sizeof(im_sess));
        if (!ns) { ca_mutex_unlock(&im->mtx); return -1; }
        im->sessions = ns;
        im->cap = cap;
    }
    im_sess *s = &im->sessions[im->count];
    memset(s, 0, sizeof(*s));
    s->id = im->next_id++;
    s->name = ca_strdup(name && *name ? name : "新会话");
    s->kind = ca_strdup(kind && *kind ? kind : "direct");
    if (members && n_members) {
        s->members = calloc(n_members, sizeof(char *));
        if (s->members) {
            s->n_members = n_members;
            for (size_t i = 0; i < n_members; i++)
                s->members[i] = ca_strdup(members[i] ? members[i] : "");
        }
    }
    s->created_ms = ca_time_now_ms();
    im->count++;
    im_persist(im);
    ca_mutex_unlock(&im->mtx);
    return s->id;
}

int ca_im_delete_session(ca_im *im, int64_t id) {
    if (!im) return 0;
    ca_mutex_lock(&im->mtx);
    int found = 0;
    for (size_t i = 0; i < im->count; i++) {
        if (im->sessions[i].id == id) {
            sess_free(&im->sessions[i]);
            memmove(&im->sessions[i], &im->sessions[i + 1],
                    (im->count - i - 1) * sizeof(im_sess));
            im->count--;
            found = 1;
            break;
        }
    }
    if (found) im_persist(im);
    ca_mutex_unlock(&im->mtx);
    return found;
}

ca_im_session *ca_im_list_sessions(ca_im *im, size_t *count) {
    if (!im) { if (count) *count = 0; return NULL; }
    ca_mutex_lock(&im->mtx);
    ca_im_session *out = NULL;
    if (im->count) {
        out = calloc(im->count, sizeof(ca_im_session));
        if (out) {
            for (size_t i = 0; i < im->count; i++) {
                out[i].id = im->sessions[i].id;
                out[i].name = ca_strdup(im->sessions[i].name);
                out[i].kind = ca_strdup(im->sessions[i].kind ? im->sessions[i].kind : "direct");
                out[i].channel = im->sessions[i].channel ? ca_strdup(im->sessions[i].channel) : NULL;
                out[i].created_ms = im->sessions[i].created_ms;
                if (im->sessions[i].n_members) {
                    out[i].members = calloc(im->sessions[i].n_members, sizeof(char *));
                    if (out[i].members) {
                        out[i].n_members = im->sessions[i].n_members;
                        for (size_t j = 0; j < out[i].n_members; j++)
                            out[i].members[j] = ca_strdup(im->sessions[i].members[j]);
                    }
                }
            }
        }
    }
    if (count) *count = out ? im->count : 0;
    ca_mutex_unlock(&im->mtx);
    return out;
}

void ca_im_sessions_free(ca_im_session *s, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(s[i].name);
        free(s[i].kind);
        free(s[i].channel);
        for (size_t j = 0; j < s[i].n_members; j++) free(s[i].members[j]);
        free(s[i].members);
    }
    free(s);
}

ca_im_message *ca_im_messages(ca_im *im, int64_t session_id, size_t *count) {
    if (!im) { if (count) *count = 0; return NULL; }
    ca_mutex_lock(&im->mtx);
    im_sess *s = find_sess(im, session_id);
    ca_im_message *out = NULL;
    size_t n = 0;
    if (s && s->count) {
        out = calloc(s->count, sizeof(ca_im_message));
        if (out) {
            for (size_t i = 0; i < s->count; i++) {
                out[i].id = s->msgs[i].id;
                out[i].role = ca_strdup(s->msgs[i].role);
                out[i].sender = s->msgs[i].sender ? ca_strdup(s->msgs[i].sender) : NULL;
                out[i].content = ca_strdup(s->msgs[i].content);
                out[i].ts_ms = s->msgs[i].ts_ms;
            }
            n = s->count;
        }
    }
    if (count) *count = n;
    ca_mutex_unlock(&im->mtx);
    return out;
}

void ca_im_messages_free(ca_im_message *m, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(m[i].role);
        free(m[i].sender);
        free(m[i].content);
    }
    free(m);
}

int64_t ca_im_send(ca_im *im, int64_t session_id, const char *role, const char *content) {
    return ca_im_send_ex(im, session_id, role, content, NULL);
}

int64_t ca_im_send_ex(ca_im *im, int64_t session_id, const char *role,
                      const char *content, const char *sender) {
    if (!im || !content) return -1;
    ca_mutex_lock(&im->mtx);
    im_sess *s = find_sess(im, session_id);
    if (!s) { ca_mutex_unlock(&im->mtx); return -1; }
    int64_t id = im->next_id++;
    sess_add_msg(s, id, role ? role : "user", sender, content, ca_time_now_ms());
    im_persist(im);
    ca_mutex_unlock(&im->mtx);
    return id;
}

size_t ca_im_total_messages(ca_im *im) {
    if (!im) return 0;
    ca_mutex_lock(&im->mtx);
    size_t total = 0;
    for (size_t i = 0; i < im->count; i++) total += im->sessions[i].count;
    ca_mutex_unlock(&im->mtx);
    return total;
}

char *ca_im_sessions_json(ca_im *im) {
    if (!im) return ca_strdup("{}");
    ca_mutex_lock(&im->mtx);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "sessions");
    size_t total = 0;
    for (size_t i = 0; i < im->count; i++) {
        im_sess *s = &im->sessions[i];
        total += s->count;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)s->id);
        cJSON_AddStringToObject(o, "name", s->name ? s->name : "");
        cJSON_AddStringToObject(o, "kind", s->kind ? s->kind : "direct");
        cJSON *mem = cJSON_AddArrayToObject(o, "members");
        for (size_t j = 0; j < s->n_members; j++)
            cJSON_AddItemToArray(mem, cJSON_CreateString(s->members[j]));
        if (s->channel) cJSON_AddStringToObject(o, "channel", s->channel);
        cJSON_AddNumberToObject(o, "created_ms", (double)s->created_ms);
        cJSON_AddNumberToObject(o, "messages", (double)s->count);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "total_messages", (double)total);
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ca_mutex_unlock(&im->mtx);
    return js ? js : ca_strdup("{}");
}

/* case-insensitive substring match */
static int ci_strstr(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    if (hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (j = 0; j < nlen; j++) {
            int a = (unsigned char)hay[i + j], b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

char *ca_im_search(ca_im *im, const char *query, int limit) {
    if (!im) return ca_strdup("[]");
    if (!query || !*query) return ca_strdup("[]");
    ca_mutex_lock(&im->mtx);
    cJSON *arr = cJSON_CreateArray();
    int count = 0;
    for (size_t i = 0; i < im->count && (limit <= 0 || count < limit); i++) {
        im_sess *s = &im->sessions[i];
        for (size_t j = 0; j < s->count && (limit <= 0 || count < limit); j++) {
            if (!ci_strstr(s->msgs[j].content, query)) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "session_id", (double)s->id);
            cJSON_AddStringToObject(o, "session_name", s->name ? s->name : "");
            cJSON_AddStringToObject(o, "kind", s->kind ? s->kind : "direct");
            cJSON_AddNumberToObject(o, "id", (double)s->msgs[j].id);
            cJSON_AddStringToObject(o, "role", s->msgs[j].role);
            if (s->msgs[j].sender) cJSON_AddStringToObject(o, "sender", s->msgs[j].sender);
            cJSON_AddStringToObject(o, "content", s->msgs[j].content);
            cJSON_AddNumberToObject(o, "ts_ms", (double)s->msgs[j].ts_ms);
            cJSON_AddItemToArray(arr, o);
            count++;
        }
    }
    char *js = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ca_mutex_unlock(&im->mtx);
    return js ? js : ca_strdup("[]");
}

const char *ca_im_session_channel(ca_im *im, int64_t session_id) {
    if (!im) return NULL;
    ca_mutex_lock(&im->mtx);
    im_sess *s = find_sess(im, session_id);
    const char *ch = s ? s->channel : NULL;
    ca_mutex_unlock(&im->mtx);
    return ch;
}

int ca_im_session_set_channel(ca_im *im, int64_t session_id, const char *channel) {
    if (!im) return -1;
    ca_mutex_lock(&im->mtx);
    im_sess *s = find_sess(im, session_id);
    if (!s) { ca_mutex_unlock(&im->mtx); return -1; }
    char *old = s->channel;
    s->channel = (channel && *channel) ? ca_strdup(channel) : NULL;
    free(old);
    im_persist(im);
    ca_mutex_unlock(&im->mtx);
    return 0;
}

int64_t ca_im_session_by_channel(ca_im *im, const char *channel) {
    if (!im || !channel || !*channel) return -1;
    ca_mutex_lock(&im->mtx);
    int64_t found = -1;
    for (size_t i = 0; i < im->count; i++)
        if (im->sessions[i].channel && strcmp(im->sessions[i].channel, channel) == 0) {
            found = im->sessions[i].id;
            break;
        }
    ca_mutex_unlock(&im->mtx);
    return found;
}
