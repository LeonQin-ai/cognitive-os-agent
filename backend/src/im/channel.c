/* channel.c — external messaging channel adapters (IM bridge).
 * Sends IM content out to mainstream chat platforms and, for Telegram, polls
 * inbound messages back into linked IM sessions. Channels persist to
 * <state_root>/im/channels.json. */
#include "cagent/im/channel.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/http.h"
#include "cagent/os/os_fs.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define DEFAULT_TG_API "https://api.telegram.org"

struct ca_im_channels {
    char path[600];
    ca_mutex mtx;
    ca_im_channel *items;
    size_t count, cap;
};

static char *dup_or_null(const char *s) {
    return s ? ca_strdup(s) : NULL;
}

static int find_chan(ca_im_channels *cs, const char *name) {
    for (size_t i = 0; i < cs->count; i++)
        if (strcmp(cs->items[i].name, name) == 0) return (int)i;
    return -1;
}

static void chan_free(ca_im_channel *ch) {
    free(ch->name);
    free(ch->type);
    free(ch->endpoint);
    free(ch->token);
    free(ch->target);
}

static void chan_copy(ca_im_channel *dst, const ca_im_channel *src) {
    memset(dst, 0, sizeof(*dst));
    dst->name = dup_or_null(src->name);
    dst->type = dup_or_null(src->type);
    dst->endpoint = dup_or_null(src->endpoint);
    dst->token = dup_or_null(src->token);
    dst->target = dup_or_null(src->target);
    dst->enabled = src->enabled;
    dst->last_update_id = src->last_update_id;
}

/* ---------- persistence ---------- */

static void channels_persist(ca_im_channels *cs) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "channels");
    for (size_t i = 0; i < cs->count; i++) {
        ca_im_channel *ch = &cs->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", ch->name);
        cJSON_AddStringToObject(o, "type", ch->type ? ch->type : "generic");
        if (ch->endpoint) cJSON_AddStringToObject(o, "endpoint", ch->endpoint);
        if (ch->token) cJSON_AddStringToObject(o, "token", ch->token);
        if (ch->target) cJSON_AddStringToObject(o, "target", ch->target);
        cJSON_AddBoolToObject(o, "enabled", ch->enabled);
        cJSON_AddItemToArray(arr, o);
    }
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (js) {
        ca_fs_write_file(cs->path, js, (size_t)strlen(js));
        free(js);
    }
}

static void channels_load(ca_im_channels *cs) {
    char *js = ca_fs_read_file(cs->path);
    if (!js) return;
    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) return;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if (arr && cJSON_IsArray(arr)) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsObject(it)) continue;
            cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (!n || !cJSON_IsString(n)) continue;
            ca_im_channel ch;
            memset(&ch, 0, sizeof(ch));
            ch.name = n->valuestring;
            cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "type");
            ch.type = (t && cJSON_IsString(t)) ? t->valuestring : "generic";
            cJSON *e = cJSON_GetObjectItemCaseSensitive(it, "endpoint");
            ch.endpoint = (e && cJSON_IsString(e)) ? e->valuestring : NULL;
            cJSON *tk = cJSON_GetObjectItemCaseSensitive(it, "token");
            ch.token = (tk && cJSON_IsString(tk)) ? tk->valuestring : NULL;
            cJSON *tg = cJSON_GetObjectItemCaseSensitive(it, "target");
            ch.target = (tg && cJSON_IsString(tg)) ? tg->valuestring : NULL;
            cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
            ch.enabled = (en && cJSON_IsBool(en)) ? (en->type == cJSON_True) : 1;
            if (cs->count == cs->cap) {
                size_t cap = cs->cap ? cs->cap * 2 : 8;
                ca_im_channel *ni = realloc(cs->items, cap * sizeof(ca_im_channel));
                if (!ni) { chan_free(&ch); break; }
                cs->items = ni;
                cs->cap = cap;
            }
            chan_copy(&cs->items[cs->count++], &ch);
            /* ch's string fields point directly into the cJSON root (borrowed):
             * chan_copy duplicated them, and cJSON_Delete(root) below will free
             * the originals, so do NOT free(ch) here (double-free). */
        }
    }
    cJSON_Delete(root);
}

/* ---------- public registry ---------- */

ca_im_channels *ca_im_channels_new(const char *state_root) {
    ca_im_channels *cs = calloc(1, sizeof(ca_im_channels));
    if (!cs) return NULL;
    snprintf(cs->path, sizeof(cs->path), "%s", state_root ? state_root : "state");
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/im", cs->path);
    ca_fs_mkdirs(dir);
    snprintf(cs->path, sizeof(cs->path), "%s/im/channels.json", cs->path);
    ca_mutex_init(&cs->mtx);
    channels_load(cs);
    return cs;
}

void ca_im_channels_free(ca_im_channels *cs) {
    if (!cs) return;
    ca_mutex_lock(&cs->mtx);
    for (size_t i = 0; i < cs->count; i++) chan_free(&cs->items[i]);
    free(cs->items);
    ca_mutex_unlock(&cs->mtx);
    ca_mutex_destroy(&cs->mtx);
    free(cs);
}

int ca_im_channel_register(ca_im_channels *cs, const ca_im_channel *ch) {
    if (!cs || !ch || !ch->name || !*ch->name || !ch->type || !*ch->type) return -1;
    ca_mutex_lock(&cs->mtx);
    int i = find_chan(cs, ch->name);
    if (i >= 0) {
        chan_free(&cs->items[i]);
        chan_copy(&cs->items[i], ch);
    } else {
        if (cs->count == cs->cap) {
            size_t cap = cs->cap ? cs->cap * 2 : 8;
            ca_im_channel *ni = realloc(cs->items, cap * sizeof(ca_im_channel));
            if (!ni) { ca_mutex_unlock(&cs->mtx); return -1; }
            cs->items = ni;
            cs->cap = cap;
        }
        chan_copy(&cs->items[cs->count++], ch);
    }
    channels_persist(cs);
    ca_mutex_unlock(&cs->mtx);
    return 0;
}

int ca_im_channel_remove(ca_im_channels *cs, const char *name) {
    if (!cs || !name) return -1;
    ca_mutex_lock(&cs->mtx);
    int i = find_chan(cs, name);
    if (i < 0) { ca_mutex_unlock(&cs->mtx); return -1; }
    chan_free(&cs->items[i]);
    memmove(&cs->items[i], &cs->items[i + 1], (cs->count - i - 1) * sizeof(ca_im_channel));
    cs->count--;
    channels_persist(cs);
    ca_mutex_unlock(&cs->mtx);
    return 0;
}

ca_im_channel *ca_im_channel_find(ca_im_channels *cs, const char *name) {
    if (!cs || !name) return NULL;
    ca_mutex_lock(&cs->mtx);
    int i = find_chan(cs, name);
    ca_im_channel *c = (i >= 0) ? &cs->items[i] : NULL;
    ca_mutex_unlock(&cs->mtx);
    return c;
}

int ca_im_channel_count(ca_im_channels *cs) {
    if (!cs) return 0;
    ca_mutex_lock(&cs->mtx);
    int n = (int)cs->count;
    ca_mutex_unlock(&cs->mtx);
    return n;
}

ca_im_channel *ca_im_channel_get(ca_im_channels *cs, size_t i) {
    if (!cs) return NULL;
    ca_mutex_lock(&cs->mtx);
    ca_im_channel *c = (i < cs->count) ? &cs->items[i] : NULL;
    ca_mutex_unlock(&cs->mtx);
    return c;
}

char *ca_im_channels_json(ca_im_channels *cs) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "channels");
    if (cs) {
        ca_mutex_lock(&cs->mtx);
        for (size_t i = 0; i < cs->count; i++) {
            ca_im_channel *ch = &cs->items[i];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", ch->name);
            cJSON_AddStringToObject(o, "type", ch->type ? ch->type : "generic");
            if (ch->endpoint) cJSON_AddStringToObject(o, "endpoint", ch->endpoint);
            if (ch->token) cJSON_AddStringToObject(o, "token", ch->token);
            if (ch->target) cJSON_AddStringToObject(o, "target", ch->target);
            cJSON_AddBoolToObject(o, "enabled", ch->enabled);
            cJSON_AddItemToArray(arr, o);
        }
        ca_mutex_unlock(&cs->mtx);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s ? s : ca_strdup("{}");
}

/* ---------- sending ---------- */

/* Build the JSON body for a channel type and POST it. Returns the HTTP body
 * (malloc'd) or NULL. If `force_path` is non-NULL it overrides the URL path
 * (used by telegram's /bot<token>/sendMessage). */
static char *post_channel(ca_im_channel *ch, cJSON *body, const char *force_path,
                          char *out_err, size_t err_sz) {
    char *js = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!js) return NULL;

    char base[512], path[512];
    const char *ep = (ch->endpoint && *ch->endpoint) ? ch->endpoint
                   : (strcmp(ch->type, "telegram") == 0 ? DEFAULT_TG_API : NULL);
    if (!ep) { free(js); return NULL; }
    const char *slash = strstr(ep, "://");
    const char *pathstart = slash ? strchr(slash + 3, '/') : strchr(ep, '/');
    if (pathstart) {
        size_t blen = (size_t)(pathstart - ep);
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, ep, blen);
        base[blen] = '\0';
        snprintf(path, sizeof(path), "%s", pathstart);
    } else {
        snprintf(base, sizeof(base), "%s", ep);
        snprintf(path, sizeof(path), "/");
    }
    if (force_path && *force_path) snprintf(path, sizeof(path), "%s", force_path);

    ca_http_response *r = ca_http_post(base, path, js, "application/json", NULL, 10000);
    free(js);
    if (!r) {
        snprintf(out_err, err_sz, "unreachable: %s", ep);
        return NULL;
    }
    if (r->status != 200 && r->status != 201 && r->status != 202) {
        snprintf(out_err, err_sz, "http %d: %s", r->status, r->body ? r->body : "");
        ca_http_response_free(r);
        return NULL;
    }
    char *out = ca_strdup(r->body ? r->body : "");
    ca_http_response_free(r);
    return out;
}

char *ca_im_channel_send(ca_im_channels *cs, const char *name, const char *text) {
    if (!cs || !name) return ca_strdup("{\"ok\":false,\"error\":\"bad args\"}");
    ca_im_channel *ch = ca_im_channel_find(cs, name);
    if (!ch) return ca_strdup("{\"ok\":false,\"error\":\"channel not found\"}");
    if (!ch->enabled) return ca_strdup("{\"ok\":false,\"error\":\"channel disabled\"}");

    cJSON *body = NULL;
    char path[512];
    path[0] = '\0';
    if (strcmp(ch->type, "feishu") == 0) {
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "msg_type", "text");
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "text", text ? text : "");
        cJSON_AddItemToObject(body, "content", c);
    } else if (strcmp(ch->type, "wecom") == 0) {
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "msgtype", "text");
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "content", text ? text : "");
        cJSON_AddItemToObject(body, "text", t);
    } else if (strcmp(ch->type, "telegram") == 0) {
        if (!ch->token || !*ch->token || !ch->target || !*ch->target)
            return ca_strdup("{\"ok\":false,\"error\":\"telegram needs token + target (chat_id)\"}");
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", ch->target);
        cJSON_AddStringToObject(body, "text", text ? text : "");
        /* path is /bot<token>/sendMessage */
        char token_esc[512];
        snprintf(token_esc, sizeof(token_esc), "%s", ch->token);
        snprintf(path, sizeof(path), "/bot%s/sendMessage", token_esc);
    } else { /* generic */
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "text", text ? text : "");
    }

    char err[512] = "";
    char *resp = post_channel((ca_im_channel *)ch, body,
                              (strcmp(ch->type, "telegram") == 0) ? path : NULL, err, sizeof(err));
    if (!resp) {
        char out[768];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", err);
        return ca_strdup(out);
    }
    char out[900];
    size_t rl = strlen(resp);
    if (rl > 400) rl = 400;
    char tmp[401];
    memcpy(tmp, resp, rl);
    tmp[rl] = '\0';
    free(resp);
    snprintf(out, sizeof(out), "{\"ok\":true,\"channel\":\"%s\",\"response\":\"%s\"}", name, tmp);
    return ca_strdup(out);
}

/* ---------- telegram inbound poll ---------- */

int ca_im_channel_poll_telegram(ca_im_channels *cs, const char *name,
                                ca_im_ingest_fn ingest, void *ud) {
    if (!cs || !name || !ingest) return -1;
    ca_im_channel *ch = ca_im_channel_find(cs, name);
    if (!ch || strcmp(ch->type, "telegram") != 0 || !ch->enabled) return -1;
    if (!ch->token || !*ch->token) return -1;

    const char *ep = (ch->endpoint && *ch->endpoint) ? ch->endpoint : DEFAULT_TG_API;
    char base[512], path[900];
    const char *slash = strstr(ep, "://");
    const char *pathstart = slash ? strchr(slash + 3, '/') : strchr(ep, '/');
    if (pathstart) {
        size_t blen = (size_t)(pathstart - ep);
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, ep, blen);
        base[blen] = '\0';
        snprintf(path, sizeof(path), "%s/bot%s/getUpdates?offset=%lld&timeout=1",
                 pathstart, ch->token, (long long)(ch->last_update_id + 1));
    } else {
        snprintf(base, sizeof(base), "%s", ep);
        snprintf(path, sizeof(path), "/bot%s/getUpdates?offset=%lld&timeout=1",
                 ch->token, (long long)(ch->last_update_id + 1));
    }

    ca_http_response *r = ca_http_get(base, path, NULL, 9000);
    if (!r || r->status != 200 || !r->body) {
        if (r) ca_http_response_free(r);
        return -1;
    }
    cJSON *root = cJSON_Parse(r->body);
    ca_http_response_free(r);
    if (!root) return -1;
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    int ingested = 0;
    if (result && cJSON_IsArray(result)) {
        cJSON *it;
        cJSON_ArrayForEach(it, result) {
            if (!cJSON_IsObject(it)) continue;
            cJSON *uid = cJSON_GetObjectItemCaseSensitive(it, "update_id");
            int64_t this_uid = (uid && cJSON_IsNumber(uid)) ? (int64_t)uid->valuedouble : 0;
            /* advance the watermark and skip updates we've already seen (Telegram
             * dedups via offset; this is a defensive guard for polling robustness) */
            if (this_uid <= ch->last_update_id) continue;
            ch->last_update_id = this_uid;
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(it, "message");
            if (!msg || !cJSON_IsObject(msg)) continue;
            cJSON *text = cJSON_GetObjectItemCaseSensitive(msg, "text");
            if (!text || !cJSON_IsString(text) || !text->valuestring[0]) continue;
            /* sender name: from.first_name / from.username */
            const char *sender = NULL;
            cJSON *from = cJSON_GetObjectItemCaseSensitive(msg, "from");
            if (from && cJSON_IsObject(from)) {
                cJSON *fn = cJSON_GetObjectItemCaseSensitive(from, "first_name");
                cJSON *un = cJSON_GetObjectItemCaseSensitive(from, "username");
                if (fn && cJSON_IsString(fn)) sender = fn->valuestring;
                else if (un && cJSON_IsString(un)) sender = un->valuestring;
            }
            ingest(name, sender ? sender : "phone", text->valuestring, ud);
            ingested++;
        }
    }
    cJSON_Delete(root);
    return ingested;
}
