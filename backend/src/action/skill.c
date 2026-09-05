/* skill.c — static Shell/Python skill registry. */
#include "cognitive-os-agent/action/skill.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/os/os_proc.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/plugin_runtime/sandbox.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct coa_skill_registry {
    coa_mutex mtx;
    coa_skill *items;   /* each holds owned dup'd strings */
    size_t count, cap;
};

static void skill_free(coa_skill *s) {
    free((char *)s->name);
    free((char *)s->description);
    free((char *)s->kind);
    free((char *)s->body);
    free((char *)s->caps);
}

coa_skill_registry *coa_skill_registry_new(void) {
    coa_skill_registry *r = (coa_skill_registry *)calloc(1, sizeof(coa_skill_registry));
    if (!r) return NULL;
    coa_mutex_init(&r->mtx);
    return r;
}

void coa_skill_registry_free(coa_skill_registry *r) {
    if (!r) return;
    coa_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) skill_free(&r->items[i]);
    free(r->items);
    coa_mutex_unlock(&r->mtx);
    coa_mutex_destroy(&r->mtx);
    free(r);
}

static int find_skill(coa_skill_registry *r, const char *name) {
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->items[i].name, name) == 0) return (int)i;
    return -1;
}

int coa_skill_register(coa_skill_registry *r, const coa_skill *s) {
    return coa_skill_register_ex(r, s, 0);
}

int coa_skill_register_ex(coa_skill_registry *r, const coa_skill *s, int replace) {
    if (!r || !s || !s->name || !*s->name) return -1;
    const char *kind = (s->kind && *s->kind) ? s->kind : "shell";
    if (strcmp(kind, "shell") != 0 && strcmp(kind, "python") != 0) return -1;
    coa_mutex_lock(&r->mtx);
    int i = find_skill(r, s->name);
    if (i >= 0 && !replace) { coa_mutex_unlock(&r->mtx); return -1; }
    if (i >= 0) {
        /* upsert: overwrite in place */
        coa_skill *e = &r->items[i];
        free((void *)e->name); free((void *)e->description);
        free((void *)e->kind); free((void *)e->body); free((void *)e->caps);
        memset(e, 0, sizeof(*e));
        e->name = coa_strdup(s->name);
        e->description = coa_strdup(s->description ? s->description : "");
        e->kind = coa_strdup(kind);
        e->body = coa_strdup(s->body ? s->body : "");
        e->caps = coa_strdup(s->caps ? s->caps : "");
        coa_mutex_unlock(&r->mtx);
        return 0;
    }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        coa_skill *ni = (coa_skill *)realloc(r->items, ncap * sizeof(*ni));
        if (!ni) { coa_mutex_unlock(&r->mtx); return -1; }
        r->items = ni;
        r->cap = ncap;
    }
    coa_skill *e = &r->items[r->count++];
    memset(e, 0, sizeof(*e));
    e->name = coa_strdup(s->name);
    e->description = coa_strdup(s->description ? s->description : "");
    e->kind = coa_strdup(kind);
    e->body = coa_strdup(s->body ? s->body : "");
    e->caps = coa_strdup(s->caps ? s->caps : "");
    coa_mutex_unlock(&r->mtx);
    return 0;
}

const coa_skill *coa_skill_find(coa_skill_registry *r, const char *name) {
    if (!r || !name) return NULL;
    coa_mutex_lock(&r->mtx);
    const coa_skill *s = NULL;
    int i = find_skill(r, name);
    if (i >= 0) s = &r->items[i];
    coa_mutex_unlock(&r->mtx);
    return s;
}

int coa_skill_count(coa_skill_registry *r) {
    if (!r) return 0;
    coa_mutex_lock(&r->mtx);
    int n = (int)r->count;
    coa_mutex_unlock(&r->mtx);
    return n;
}

const coa_skill *coa_skill_get(coa_skill_registry *r, size_t i) {
    if (!r) return NULL;
    coa_mutex_lock(&r->mtx);
    const coa_skill *s = (i < r->count) ? &r->items[i] : NULL;
    coa_mutex_unlock(&r->mtx);
    return s;
}

/* Substitute {{key}} placeholders in body from args_json (a JSON object).
 * Values are stringified; unknown placeholders are left as-is. Returns a
 * malloc'd body (or a copy of body when args are absent/invalid). */
static char *bind_args(const char *body, const char *args_json) {
    cJSON *args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
    if (!args || !cJSON_IsObject(args)) { cJSON_Delete(args); return coa_strdup(body); }
    coa_strbuf sb;
    coa_strbuf_init(&sb);
    for (const char *p = body; *p;) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                size_t klen = (size_t)(end - (p + 2));
                char key[128];
                if (klen < sizeof(key)) {
                    memcpy(key, p + 2, klen);
                    key[klen] = '\0';
                    char *ks = key;
                    while (*ks == ' ') ks++;
                    char *ke = ks + strlen(ks);
                    while (ke > ks && ke[-1] == ' ') *--ke = '\0';
                    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, ks);
                    if (v) {
                        char *vs = NULL;
                        if (cJSON_IsString(v) && v->valuestring) vs = coa_strdup(v->valuestring);
                        else vs = cJSON_PrintUnformatted(v);
                        coa_strbuf_append(&sb, vs ? vs : "");
                        free(vs);
                        p = end + 2;
                        continue;
                    }
                }
            }
        }
        char tmp[2] = { *p, '\0' };
        coa_strbuf_append(&sb, tmp);
        p++;
    }
    cJSON_Delete(args);
    return coa_strbuf_detach(&sb);
}

/* 1 if a granted token covers `need` ("fs.*" covers "fs.write", exact else). */
static int cap_covers(const char *granted, const char *need) {
    const char *star = strchr(granted, '*');
    size_t plen = star ? (size_t)(star - granted) : strlen(granted);
    if (plen > 0 && granted[plen - 1] == '.') plen--;
    if (strlen(need) < plen) return 0;
    return strncmp(granted, need, plen) == 0;
}

/* Capability gate for plugin skills: the command's operation classes must be
 * covered by the granted csv. Legacy skills (caps == NULL) are unrestricted. */
static int caps_allow(const char *caps_csv, const char *cmd, char *denied, size_t dcap) {
    if (!caps_csv || !*caps_csv || !cmd) return 1;
    const char *required[4];
    int n_req = 0;
    if (strstr(cmd, ">") || strstr(cmd, "rm ") || strstr(cmd, "mv ") || strstr(cmd, "tee "))
        required[n_req++] = "fs.write";
    if (strstr(cmd, "curl") || strstr(cmd, "wget") ||
        strstr(cmd, "http://") || strstr(cmd, "https://"))
        required[n_req++] = "net";
    for (int i = 0; i < n_req; i++) {
        int covered = 0;
        const char *p = caps_csv;
        while (*p) {
            const char *e = strchr(p, ',');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char tok[64];
            if (len < sizeof(tok)) {
                memcpy(tok, p, len);
                tok[len] = '\0';
                char *t = tok;
                while (*t == ' ') t++;
                size_t tl = strlen(t);
                while (tl && t[tl - 1] == ' ') t[--tl] = '\0';
                if (*t && cap_covers(t, required[i])) { covered = 1; break; }
            }
            if (!e) break;
            p = e + 1;
        }
        if (!covered) {
            snprintf(denied, dcap, "%s", required[i]);
            return 0;
        }
    }
    return 1;
}

coa_skill_result *coa_skill_execute(coa_skill_registry *r, const char *name,
                                  const char *args_json, const char *workspace,
                                  int timeout_ms) {
    if (!r || !name) return NULL;
    const coa_skill *s = coa_skill_find(r, name);
    if (!s) return NULL;

    char *bound = bind_args(s->body, args_json);
    char *cmd = NULL;
    char pyfile[1024] = "";
    if (strcmp(s->kind, "python") == 0) {
        /* Write the substituted source to a temp file instead of a fragile
         * `python -c "..."` quoting chain. */
        if (workspace && *workspace) {
            coa_path_join(pyfile, sizeof(pyfile), workspace, ".ca-skill.py");
        } else {
            snprintf(pyfile, sizeof(pyfile), ".ca-skill.py");
        }
        if (coa_fs_write_file(pyfile, bound, strlen(bound)) == 0) {
            char cmdbuf[1120];
            snprintf(cmdbuf, sizeof(cmdbuf), "python \"%s\"", pyfile);
            cmd = coa_strdup(cmdbuf);
        } else {
            pyfile[0] = '\0';
            cmd = coa_strdup(bound); /* fallback: run as shell anyway */
        }
    } else {
        cmd = coa_strdup(bound);
    }
    free(bound);

    if (!cmd) return NULL;
    /* the workspace may not exist yet (first action of a fresh session);
     * python skills also write their temp file there */
    if (workspace && *workspace) coa_fs_mkdirs(workspace);
    char denied[64] = "";
    if (!caps_allow(s->caps, cmd, denied, sizeof(denied))) {
        free(cmd);
        if (pyfile[0]) coa_fs_remove(pyfile);
        coa_skill_result *res = (coa_skill_result *)calloc(1, sizeof(*res));
        if (res) {
            res->ok = 0;
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "capability denied: skill '%s' requires '%s' (granted: %s)",
                     name, denied, s->caps);
            res->output = coa_strdup(msg);
        }
        return res;
    }
    if (coa_sandbox_forbidden(cmd)) {
        free(cmd);
        if (pyfile[0]) coa_fs_remove(pyfile);
        return NULL;
    }
    coa_proc_result *pr = coa_proc_run_in(cmd, timeout_ms, workspace);
    free(cmd);
    if (pyfile[0]) coa_fs_remove(pyfile);
    if (!pr) return NULL;

    coa_skill_result *res = (coa_skill_result *)calloc(1, sizeof(coa_skill_result));
    if (!res) { coa_proc_result_free(pr); return NULL; }
    res->ok = (pr->exit_code == 0 && !pr->timed_out) ? 1 : 0;
    res->output = coa_strdup(pr->output ? pr->output : "");
    coa_proc_result_free(pr);
    return res;
}

void coa_skill_result_free(coa_skill_result *res) {
    if (!res) return;
    free(res->output);
    free(res);
}

char *coa_skill_list_json(coa_skill_registry *r) {
    cJSON *arr = cJSON_CreateArray();
    if (!r) return cJSON_PrintUnformatted(arr);
    coa_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        coa_skill *e = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "description", e->description);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddItemToArray(arr, o);
    }
    coa_mutex_unlock(&r->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static int dump_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 0;
}

int coa_skill_unregister(coa_skill_registry *r, const char *name) {
    if (!r || !name) return -1;
    coa_mutex_lock(&r->mtx);
    int idx = find_skill(r, name);
    if (idx < 0) { coa_mutex_unlock(&r->mtx); return -1; }
    skill_free(&r->items[idx]);
    if ((size_t)idx + 1 < r->count)
        memmove(&r->items[idx], &r->items[idx + 1], (r->count - (size_t)idx - 1) * sizeof(coa_skill));
    r->count--;
    coa_mutex_unlock(&r->mtx);
    return 0;
}

int coa_skill_registry_persist(coa_skill_registry *r, const char *state_root) {
    if (!r || !state_root) return -1;
    char path[1024];
    coa_path_join(path, sizeof(path), state_root, "skills.json");
    cJSON *arr = cJSON_CreateArray();
    coa_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        coa_skill *e = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "description", e->description);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddStringToObject(o, "body", e->body);
        if (e->caps && *e->caps) cJSON_AddStringToObject(o, "caps", e->caps);
        cJSON_AddItemToArray(arr, o);
    }
    coa_mutex_unlock(&r->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    int rc = dump_file(path, s ? s : "[]");
    free(s);
    return rc;
}

int coa_skill_registry_load(coa_skill_registry *r, const char *state_root) {
    if (!r || !state_root) return -1;
    char path[1024];
    coa_path_join(path, sizeof(path), state_root, "skills.json");
    char *txt = slurp_file(path);
    if (!txt) return 0;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return 0; }
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o)) continue;
        cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "name");
        cJSON *d = cJSON_GetObjectItemCaseSensitive(o, "description");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(o, "kind");
        cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "body");
        cJSON *cp = cJSON_GetObjectItemCaseSensitive(o, "caps");
        if (!n || !cJSON_IsString(n)) continue;
        coa_skill sk = {
            n->valuestring,
            (d && cJSON_IsString(d)) ? d->valuestring : "",
            (k && cJSON_IsString(k)) ? k->valuestring : "shell",
            (b && cJSON_IsString(b)) ? b->valuestring : "",
            (cp && cJSON_IsString(cp)) ? cp->valuestring : ""
        };
        coa_skill_register(r, &sk); /* skips duplicate names */
    }
    cJSON_Delete(arr);
    return 0;
}
