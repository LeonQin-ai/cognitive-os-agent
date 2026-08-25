/* skill.c — static Shell/Python skill registry. */
#include "cagent/action/skill.h"
#include "cagent/os/os_thread.h"
#include "cagent/os/os_proc.h"
#include "cagent/plugin_runtime/sandbox.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

struct ca_skill_registry {
    ca_mutex mtx;
    ca_skill *items;   /* each holds owned dup'd strings */
    size_t count, cap;
};

static void skill_free(ca_skill *s) {
    free((char *)s->name);
    free((char *)s->description);
    free((char *)s->kind);
    free((char *)s->body);
}

ca_skill_registry *ca_skill_registry_new(void) {
    ca_skill_registry *r = (ca_skill_registry *)calloc(1, sizeof(ca_skill_registry));
    if (!r) return NULL;
    ca_mutex_init(&r->mtx);
    return r;
}

void ca_skill_registry_free(ca_skill_registry *r) {
    if (!r) return;
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) skill_free(&r->items[i]);
    free(r->items);
    ca_mutex_unlock(&r->mtx);
    ca_mutex_destroy(&r->mtx);
    free(r);
}

static int find_skill(ca_skill_registry *r, const char *name) {
    for (size_t i = 0; i < r->count; i++)
        if (strcmp(r->items[i].name, name) == 0) return (int)i;
    return -1;
}

int ca_skill_register(ca_skill_registry *r, const ca_skill *s) {
    if (!r || !s || !s->name || !*s->name) return -1;
    const char *kind = (s->kind && *s->kind) ? s->kind : "shell";
    if (strcmp(kind, "shell") != 0 && strcmp(kind, "python") != 0) return -1;
    ca_mutex_lock(&r->mtx);
    if (find_skill(r, s->name) >= 0) { ca_mutex_unlock(&r->mtx); return -1; }
    if (r->count == r->cap) {
        size_t ncap = r->cap ? r->cap * 2 : 8;
        ca_skill *ni = (ca_skill *)realloc(r->items, ncap * sizeof(*ni));
        if (!ni) { ca_mutex_unlock(&r->mtx); return -1; }
        r->items = ni;
        r->cap = ncap;
    }
    ca_skill *e = &r->items[r->count++];
    memset(e, 0, sizeof(*e));
    e->name = ca_strdup(s->name);
    e->description = ca_strdup(s->description ? s->description : "");
    e->kind = ca_strdup(kind);
    e->body = ca_strdup(s->body ? s->body : "");
    ca_mutex_unlock(&r->mtx);
    return 0;
}

const ca_skill *ca_skill_find(ca_skill_registry *r, const char *name) {
    if (!r || !name) return NULL;
    ca_mutex_lock(&r->mtx);
    const ca_skill *s = NULL;
    int i = find_skill(r, name);
    if (i >= 0) s = &r->items[i];
    ca_mutex_unlock(&r->mtx);
    return s;
}

int ca_skill_count(ca_skill_registry *r) {
    if (!r) return 0;
    ca_mutex_lock(&r->mtx);
    int n = (int)r->count;
    ca_mutex_unlock(&r->mtx);
    return n;
}

const ca_skill *ca_skill_get(ca_skill_registry *r, size_t i) {
    if (!r) return NULL;
    ca_mutex_lock(&r->mtx);
    const ca_skill *s = (i < r->count) ? &r->items[i] : NULL;
    ca_mutex_unlock(&r->mtx);
    return s;
}

/* Wrap python source into a `python -c "<escaped>"` command. */
static char *py_command(const char *code) {
    ca_strbuf sb;
    ca_strbuf_init(&sb);
    ca_strbuf_append(&sb, "python -c \"");
    for (const char *p = code; *p; p++) {
        if (*p == '\\' || *p == '"') ca_strbuf_append(&sb, "\\");
        char tmp[2] = { *p, '\0' };
        ca_strbuf_append(&sb, tmp);
    }
    ca_strbuf_append(&sb, "\"");
    return ca_strbuf_detach(&sb);
}

ca_skill_result *ca_skill_execute(ca_skill_registry *r, const char *name,
                                  const char *args_json, const char *workspace,
                                  int timeout_ms) {
    if (!r || !name) return NULL;
    (void)args_json;
    const ca_skill *s = ca_skill_find(r, name);
    if (!s) return NULL;

    char *cmd = NULL;
    if (strcmp(s->kind, "python") == 0) {
        cmd = py_command(s->body);
    } else {
        cmd = ca_strdup(s->body);
    }

    if (ca_sandbox_forbidden(cmd)) {
        free(cmd);
        return NULL;
    }
    ca_proc_result *pr = ca_proc_run_in(cmd, timeout_ms, workspace);
    free(cmd);
    if (!pr) return NULL;

    ca_skill_result *res = (ca_skill_result *)calloc(1, sizeof(ca_skill_result));
    if (!res) { ca_proc_result_free(pr); return NULL; }
    res->ok = (pr->exit_code == 0 && !pr->timed_out) ? 1 : 0;
    res->output = ca_strdup(pr->output ? pr->output : "");
    ca_proc_result_free(pr);
    return res;
}

void ca_skill_result_free(ca_skill_result *res) {
    if (!res) return;
    free(res->output);
    free(res);
}

char *ca_skill_list_json(ca_skill_registry *r) {
    cJSON *arr = cJSON_CreateArray();
    if (!r) return cJSON_PrintUnformatted(arr);
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        ca_skill *e = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "description", e->description);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&r->mtx);
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

int ca_skill_unregister(ca_skill_registry *r, const char *name) {
    if (!r || !name) return -1;
    ca_mutex_lock(&r->mtx);
    int idx = find_skill(r, name);
    if (idx < 0) { ca_mutex_unlock(&r->mtx); return -1; }
    skill_free(&r->items[idx]);
    if ((size_t)idx + 1 < r->count)
        memmove(&r->items[idx], &r->items[idx + 1], (r->count - (size_t)idx - 1) * sizeof(ca_skill));
    r->count--;
    ca_mutex_unlock(&r->mtx);
    return 0;
}

int ca_skill_registry_persist(ca_skill_registry *r, const char *state_root) {
    if (!r || !state_root) return -1;
    char path[1024];
    ca_path_join(path, sizeof(path), state_root, "skills.json");
    cJSON *arr = cJSON_CreateArray();
    ca_mutex_lock(&r->mtx);
    for (size_t i = 0; i < r->count; i++) {
        ca_skill *e = &r->items[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "description", e->description);
        cJSON_AddStringToObject(o, "kind", e->kind);
        cJSON_AddStringToObject(o, "body", e->body);
        cJSON_AddItemToArray(arr, o);
    }
    ca_mutex_unlock(&r->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    int rc = dump_file(path, s ? s : "[]");
    free(s);
    return rc;
}

int ca_skill_registry_load(ca_skill_registry *r, const char *state_root) {
    if (!r || !state_root) return -1;
    char path[1024];
    ca_path_join(path, sizeof(path), state_root, "skills.json");
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
        if (!n || !cJSON_IsString(n)) continue;
        ca_skill sk = {
            n->valuestring,
            (d && cJSON_IsString(d)) ? d->valuestring : "",
            (k && cJSON_IsString(k)) ? k->valuestring : "shell",
            (b && cJSON_IsString(b)) ? b->valuestring : ""
        };
        ca_skill_register(r, &sk); /* skips duplicate names */
    }
    cJSON_Delete(arr);
    return 0;
}
