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

struct skill_registry {
    mutex_t mtx;
    skill *items; /* each holds owned dup'd strings */
    size_t count, cap;
};

static void skill_free(skill *s) {
    free((char *)s->name);
    free((char *)s->description);
    free((char *)s->kind);
    free((char *)s->body);
    free((char *)s->caps);
}

skill_registry *skill_registry_new(void) {
    skill_registry *r = (skill_registry *)calloc(1, sizeof(skill_registry));
    if (!r)
        return NULL;
    mutex_init(&r->mtx);
    return r;
}

void skill_registry_free(skill_registry *r) {
    if (!r)
        return;
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++)
        skill_free(&r->items[i]);
    free(r->items);
    mutex_unlock(&r->mtx);
    mutex_destroy(&r->mtx);
    free(r);
}

static int find_skill(skill_registry *r, const char *name) {
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->items[i].name, name) == 0)
            return (int)i;
    return -1;
}

int skill_register(skill_registry *r, const skill *s) {
    return skill_register_ex(r, s, 0);
}

int skill_register_ex(skill_registry *r, const skill *s, int replace) {
    if (!r || !s || !s->name || !*s->name)
        return -1;
    const char *kind = (s->kind && *s->kind) ? s->kind : "shell";
    if (strcmp(kind, "shell") != 0 && strcmp(kind, "python") != 0 && strcmp(kind, "prompt") != 0)
        return -1;
    mutex_lock(&r->mtx);
    int i = find_skill(r, s->name);
    if (i >= 0 && !replace) {
        mutex_unlock(&r->mtx);
        return -1;
    }
    if (i >= 0) {
        /* upsert: overwrite in place */
        skill *e = &r->items[i];
        free((void *)e->name);
        free((void *)e->description);
        free((void *)e->kind);
        free((void *)e->body);
        free((void *)e->caps);
        memset(e, 0, sizeof(*e));
        e->name = xstrdup(s->name);
        e->description = xstrdup(s->description ? s->description : "");
        e->kind = xstrdup(kind);
        e->body = xstrdup(s->body ? s->body : "");
        e->caps = xstrdup(s->caps ? s->caps : "");
        mutex_unlock(&r->mtx);
        return 0;
    }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        skill *ni = (skill *)realloc(r->items, ncap * sizeof(*ni));
        if (!ni) {
            mutex_unlock(&r->mtx);
            return -1;
        }
        r->items = ni;
        r->cap = ncap;
    }
    skill *e = &r->items[r->count++];
    memset(e, 0, sizeof(*e));
    e->name = xstrdup(s->name);
    e->description = xstrdup(s->description ? s->description : "");
    e->kind = xstrdup(kind);
    e->body = xstrdup(s->body ? s->body : "");
    e->caps = xstrdup(s->caps ? s->caps : "");
    mutex_unlock(&r->mtx);
    return 0;
}

const skill *skill_find(skill_registry *r, const char *name) {
    if (!r || !name)
        return NULL;
    mutex_lock(&r->mtx);
    const skill *s = NULL;
    int i = find_skill(r, name);
    if (i >= 0)
        s = &r->items[i];
    mutex_unlock(&r->mtx);
    return s;
}

int skill_count(skill_registry *r) {
    if (!r)
        return 0;
    mutex_lock(&r->mtx);
    int n = (int)r->count;
    mutex_unlock(&r->mtx);
    return n;
}

const skill *skill_get(skill_registry *r, size_t i) {
    if (!r)
        return NULL;
    mutex_lock(&r->mtx);
    const skill *s = (i < r->count) ? &r->items[i] : NULL;
    mutex_unlock(&r->mtx);
    return s;
}

/* Substitute {{key}} placeholders in body from args_json (a JSON object).
 * Values are stringified; unknown placeholders are left as-is. Returns a
 * malloc'd body (or a copy of body when args are absent/invalid). */
static char *bind_args(const char *body, const char *args_json) {
    cJSON *args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args);
        return xstrdup(body);
    }
    strbuf sb;
    strbuf_init(&sb);
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
                    while (*ks == ' ')
                        ks++;
                    char *ke = ks + strlen(ks);
                    while (ke > ks && ke[-1] == ' ')
                        *--ke = '\0';
                    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, ks);
                    if (v) {
                        char *vs = NULL;
                        if (cJSON_IsString(v) && v->valuestring)
                            vs = xstrdup(v->valuestring);
                        else
                            vs = cJSON_PrintUnformatted(v);
                        strbuf_append(&sb, vs ? vs : "");
                        free(vs);
                        p = end + 2;
                        continue;
                    }
                }
            }
        }
        char tmp[2] = {*p, '\0'};
        strbuf_append(&sb, tmp);
        p++;
    }
    cJSON_Delete(args);
    return strbuf_detach(&sb);
}

/* 1 if a granted token covers `need` ("fs.*" covers "fs.write", exact else). */
static int cap_covers(const char *granted, const char *need) {
    const char *star = strchr(granted, '*');
    size_t plen = star ? (size_t)(star - granted) : strlen(granted);
    if (plen > 0 && granted[plen - 1] == '.')
        plen--;
    if (strlen(need) < plen)
        return 0;
    return strncmp(granted, need, plen) == 0;
}

/* Capability gate for plugin skills: the command's operation classes must be
 * covered by the granted csv. Legacy skills (caps == NULL) are unrestricted. */
static int caps_allow(const char *caps_csv, const char *cmd, char *denied, size_t dcap) {
    if (!caps_csv || !*caps_csv || !cmd)
        return 1;
    const char *required[4];
    int n_req = 0;
    if (strstr(cmd, ">") || strstr(cmd, "rm ") || strstr(cmd, "mv ") || strstr(cmd, "tee "))
        required[n_req++] = "fs.write";
    if (strstr(cmd, "curl") || strstr(cmd, "wget") || strstr(cmd, "http://") || strstr(cmd, "https://"))
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
                while (*t == ' ')
                    t++;
                size_t tl = strlen(t);
                while (tl && t[tl - 1] == ' ')
                    t[--tl] = '\0';
                if (*t && cap_covers(t, required[i])) {
                    covered = 1;
                    break;
                }
            }
            if (!e)
                break;
            p = e + 1;
        }
        if (!covered) {
            snprintf(denied, dcap, "%s", required[i]);
            return 0;
        }
    }
    return 1;
}

skill_result *skill_execute(skill_registry *r, const char *name, const char *args_json,
                                    const char *workspace, int timeout_ms) {
    if (!r || !name)
        return NULL;
    const skill *s = skill_find(r, name);
    if (!s)
        return NULL;

    char *bound = bind_args(s->body, args_json);
    char *cmd = NULL;
    if (strcmp(s->kind, "prompt") == 0) {
        /* prompt skills need an LLM backend — not runnable as a process */
        free(bound);
        skill_result *res = (skill_result *)calloc(1, sizeof(*res));
        if (res) {
            res->ok = 0;
            res->output = xstrdup("prompt skill: run via /v1/skills/run (needs an LLM backend)");
        }
        return res;
    }
    /* the workspace may not exist yet (first action of a fresh session);
     * create it BEFORE python skills write their temp file there, otherwise
     * the write fails and the raw python source falls through to the shell */
    if (workspace && *workspace)
        fs_mkdirs(workspace);
    char pyfile[1024] = "";
    if (strcmp(s->kind, "python") == 0) {
        /* Write the substituted source to a temp file instead of a fragile
         * `python -c "..."` quoting chain. */
        if (workspace && *workspace) {
            path_join(pyfile, sizeof(pyfile), workspace, ".ca-skill.py");
        } else {
            snprintf(pyfile, sizeof(pyfile), ".ca-skill.py");
        }
        if (fs_write_file(pyfile, bound, strlen(bound)) == 0) {
            char cmdbuf[1120];
            snprintf(cmdbuf, sizeof(cmdbuf), "python \"%s\"", pyfile);
            cmd = xstrdup(cmdbuf);
        } else {
            pyfile[0] = '\0';
            cmd = xstrdup(bound); /* fallback: run as shell anyway */
        }
    } else {
        cmd = xstrdup(bound);
    }
    free(bound);

    if (!cmd)
        return NULL;
    char denied[64] = "";
    if (!caps_allow(s->caps, cmd, denied, sizeof(denied))) {
        free(cmd);
        if (pyfile[0])
            fs_remove(pyfile);
        skill_result *res = (skill_result *)calloc(1, sizeof(*res));
        if (res) {
            res->ok = 0;
            char msg[192];
            snprintf(msg, sizeof(msg), "capability denied: skill '%s' requires '%s' (granted: %s)", name, denied,
                     s->caps);
            res->output = xstrdup(msg);
        }
        return res;
    }
    if (sandbox_forbidden(cmd)) {
        free(cmd);
        if (pyfile[0])
            fs_remove(pyfile);
        return NULL;
    }
    proc_result *pr = proc_run_in(cmd, timeout_ms, workspace);
    free(cmd);
    if (pyfile[0])
        fs_remove(pyfile);
    if (!pr)
        return NULL;

    skill_result *res = (skill_result *)calloc(1, sizeof(skill_result));
    if (!res) {
        proc_result_free(pr);
        return NULL;
    }
    res->ok = (pr->exit_code == 0 && !pr->timed_out) ? 1 : 0;
    res->output = xstrdup(pr->output ? pr->output : "");
    proc_result_free(pr);
    return res;
}

void skill_result_free(skill_result *res) {
    if (!res)
        return;
    free(res->output);
    free(res);
}

char *skill_render_prompt(skill_registry *r, const char *name, const char *args_json) {
    if (!r || !name)
        return NULL;
    const skill *s = skill_find(r, name);
    if (!s || !s->kind || strcmp(s->kind, "prompt") != 0)
        return NULL;
    return bind_args(s->body, args_json);
}

char *skill_list_json(skill_registry *r) {
    cJSON *arr = cJSON_CreateArray();
    if (!r)
        return cJSON_PrintUnformatted(arr);
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        skill *e = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "description", e->description);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddItemToArray(arr, o);
    }
    mutex_unlock(&r->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static int dump_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return 0;
}

int skill_unregister(skill_registry *r, const char *name) {
    if (!r || !name)
        return -1;
    mutex_lock(&r->mtx);
    int idx = find_skill(r, name);
    if (idx < 0) {
        mutex_unlock(&r->mtx);
        return -1;
    }
    skill_free(&r->items[idx]);
    if ((size_t)idx + 1 < r->count)
        memmove(&r->items[idx], &r->items[idx + 1], (r->count - (size_t)idx - 1) * sizeof(skill));
    r->count--;
    mutex_unlock(&r->mtx);
    return 0;
}

int skill_registry_persist(skill_registry *r, const char *state_root) {
    if (!r || !state_root)
        return -1;
    char path[1024];
    path_join(path, sizeof(path), state_root, "skills.json");
    cJSON *arr = cJSON_CreateArray();
    mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        skill *e = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "description", e->description);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddStringToObject(o, "body", e->body);
        if (e->caps && *e->caps)
            cJSON_AddStringToObject(o, "caps", e->caps);
        cJSON_AddItemToArray(arr, o);
    }
    mutex_unlock(&r->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    int rc = dump_file(path, s ? s : "[]");
    free(s);
    return rc;
}

int skill_registry_load(skill_registry *r, const char *state_root) {
    if (!r || !state_root)
        return -1;
    char path[1024];
    path_join(path, sizeof(path), state_root, "skills.json");
    char *txt = slurp_file(path);
    if (!txt)
        return 0;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr)
            cJSON_Delete(arr);
        return 0;
    }
    for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(o))
            continue;
        cJSON *n = cJSON_GetObjectItemCaseSensitive(o, "name");
        cJSON *d = cJSON_GetObjectItemCaseSensitive(o, "description");
        cJSON *k = cJSON_GetObjectItemCaseSensitive(o, "kind");
        cJSON *b = cJSON_GetObjectItemCaseSensitive(o, "body");
        cJSON *cp = cJSON_GetObjectItemCaseSensitive(o, "caps");
        if (!n || !cJSON_IsString(n))
            continue;
        skill sk = {n->valuestring, (d && cJSON_IsString(d)) ? d->valuestring : "",
                        (k && cJSON_IsString(k)) ? k->valuestring : "shell",
                        (b && cJSON_IsString(b)) ? b->valuestring : "",
                        (cp && cJSON_IsString(cp)) ? cp->valuestring : ""};
        skill_register(r, &sk); /* skips duplicate names */
    }
    cJSON_Delete(arr);
    return 0;
}
