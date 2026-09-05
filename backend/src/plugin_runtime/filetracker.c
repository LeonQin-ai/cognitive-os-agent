/* filetracker.c — sandbox file-access tracking: path->ops registry plus
 * before/after workspace scanning and command-token read detection. */
#include "cognitive-os-agent/plugin_runtime/filetracker.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_thread.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

#define FT_MAX_ENTRIES 4096  /* tracked path cap */
#define FT_SCAN_MAX_ENTRIES 8192 /* snapshot entry cap */
#define FT_SCAN_MAX_DEPTH 8

typedef struct ft_entry {
    char *path;
    int ops;
    struct ft_entry *next;
} ft_entry;

struct coa_filetracker {
    coa_mutex mtx;
    ft_entry *head;
    int count;
};

/* snapshot: flat array of {full path, size} */
typedef struct ft_snap_entry {
    char *path;         /* full path */
    long long size;
} ft_snap_entry;

struct coa_ft_snapshot {
    ft_snap_entry *items;
    size_t count;
};

coa_filetracker *coa_filetracker_new(void) {
    coa_filetracker *ft = calloc(1, sizeof(*ft));
    if (!ft) return NULL;
    coa_mutex_init(&ft->mtx);
    return ft;
}

void coa_filetracker_free(coa_filetracker *ft) {
    if (!ft) return;
    coa_filetracker_clear(ft);
    coa_mutex_destroy(&ft->mtx);
    free(ft);
}

void coa_filetracker_clear(coa_filetracker *ft) {
    if (!ft) return;
    coa_mutex_lock(&ft->mtx);
    ft_entry *e = ft->head;
    while (e) {
        ft_entry *n = e->next;
        free(e->path);
        free(e);
        e = n;
    }
    ft->head = NULL;
    ft->count = 0;
    coa_mutex_unlock(&ft->mtx);
}

int coa_filetracker_record(coa_filetracker *ft, const char *path, int ops) {
    if (!ft || !path || !*path || ops <= 0) return 0;
    coa_mutex_lock(&ft->mtx);
    for (ft_entry *e = ft->head; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            e->ops |= ops;
            int acc = e->ops;
            coa_mutex_unlock(&ft->mtx);
            return acc;
        }
    }
    if (ft->count < FT_MAX_ENTRIES) {
        ft_entry *e = calloc(1, sizeof(*e));
        if (e) {
            e->path = coa_strdup(path);
            if (e->path) {
                e->ops = ops;
                e->next = ft->head;
                ft->head = e;
                ft->count++;
                int acc = e->ops;
                coa_mutex_unlock(&ft->mtx);
                return acc;
            }
            free(e);
        }
    }
    coa_mutex_unlock(&ft->mtx);
    return 0;
}

int coa_filetracker_count(coa_filetracker *ft) {
    if (!ft) return 0;
    coa_mutex_lock(&ft->mtx);
    int n = ft->count;
    coa_mutex_unlock(&ft->mtx);
    return n;
}

const char *coa_filetracker_ops_str(int ops) {
    static char buf[40];
    buf[0] = '\0';
    if (ops & COA_FT_READ)  strcat(buf, "read");
    if (ops & COA_FT_WRITE) { if (buf[0]) strcat(buf, ","); strcat(buf, "write"); }
    if (ops & COA_FT_DELETE) { if (buf[0]) strcat(buf, ","); strcat(buf, "delete"); }
    if (ops & COA_FT_EXEC) { if (buf[0]) strcat(buf, ","); strcat(buf, "exec"); }
    if (!buf[0]) strcat(buf, "none");
    return buf;
}

char *coa_filetracker_json(coa_filetracker *ft) {
    if (!ft) return coa_strdup("[]");
    coa_mutex_lock(&ft->mtx);
    cJSON *arr = cJSON_CreateArray();
    /* walk in registration order: list is LIFO, so reverse-collect */
    size_t n = (size_t)ft->count;
    const char **paths = n ? calloc(n, sizeof(char *)) : NULL;
    int *opsv = n ? calloc(n, sizeof(int)) : NULL;
    size_t i = 0;
    for (ft_entry *e = ft->head; e && i < n; e = e->next) {
        paths[i] = e->path;
        opsv[i] = e->ops;
        i++;
    }
    for (size_t k = i; k-- > 0; ) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", paths[k]);
        cJSON_AddStringToObject(o, "ops", coa_filetracker_ops_str(opsv[k]));
        cJSON_AddItemToArray(arr, o);
    }
    free(paths);
    free(opsv);
    coa_mutex_unlock(&ft->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : coa_strdup("[]");
}

/* ---------- workspace snapshot / diff ---------- */

static void snap_add(coa_ft_snapshot *s, const char *path, long long size) {
    if (!s || s->count >= FT_SCAN_MAX_ENTRIES) return;
    ft_snap_entry *e = &s->items[s->count];
    e->path = coa_strdup(path);
    if (!e->path) return;
    e->size = size;
    s->count++;
}

static void scan_dir(coa_ft_snapshot *s, const char *dir, int depth) {
    if (!s || !dir || depth > FT_SCAN_MAX_DEPTH) return;
    coa_dir_list list;
    memset(&list, 0, sizeof(list));
    if (coa_fs_list_dir(dir, &list) != 0) return;
    for (size_t i = 0; i < list.count; i++) {
        if (s->count >= FT_SCAN_MAX_ENTRIES) break;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, list.items[i].name);
        if (list.items[i].is_dir) {
            scan_dir(s, full, depth + 1);
        } else {
            long long sz = coa_fs_file_size(full);
            if (sz >= 0) snap_add(s, full, sz);
        }
    }
    coa_fs_list_free(&list);
}

coa_ft_snapshot *coa_filetracker_dir_snapshot(const char *dir) {
    coa_ft_snapshot *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->items = calloc(FT_SCAN_MAX_ENTRIES, sizeof(ft_snap_entry));
    if (!s->items) { free(s); return NULL; }
    if (dir && *dir && coa_fs_is_dir(dir)) scan_dir(s, dir, 0);
    return s;
}

void coa_filetracker_snapshot_free(coa_ft_snapshot *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) free(s->items[i].path);
    free(s->items);
    free(s);
}

static int snap_find(const coa_ft_snapshot *s, const char *path) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].path, path) == 0) return (int)i;
    return -1;
}

int coa_filetracker_dir_diff(coa_filetracker *ft, const coa_ft_snapshot *before,
                            const char *dir) {
    if (!ft || !before) return 0;
    coa_ft_snapshot *after = coa_filetracker_dir_snapshot(dir);
    if (!after) return 0;
    int changes = 0;
    /* new / modified */
    for (size_t i = 0; i < after->count; i++) {
        int j = snap_find(before, after->items[i].path);
        if (j < 0 || before->items[j].size != after->items[i].size) {
            coa_filetracker_record(ft, after->items[i].path, COA_FT_WRITE);
            changes++;
        }
    }
    /* vanished */
    for (size_t i = 0; i < before->count; i++) {
        if (snap_find(after, before->items[i].path) < 0) {
            coa_filetracker_record(ft, before->items[i].path, COA_FT_DELETE);
            changes++;
        }
    }
    coa_filetracker_snapshot_free(after);
    return changes;
}

/* ---------- command read detection ---------- */

static void record_if_exists(coa_filetracker *ft, const char *token,
                             const char *workspace) {
    if (!token || !*token) return;
    if (coa_fs_exists(token)) {
        coa_filetracker_record(ft, token, COA_FT_READ);
        return;
    }
    if (workspace && *workspace) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", workspace, token);
        if (coa_fs_exists(full))
            coa_filetracker_record(ft, full, COA_FT_READ);
    }
}

int coa_filetracker_cmd_reads(coa_filetracker *ft, const char *cmd,
                             const char *workspace) {
    if (!ft || !cmd) return 0;
    int reads = 0;
    const char *p = cmd;
    while (*p) {
        /* skip separators */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '"' || *p == '\'') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != '"' && *p != '\'') p++;
        size_t len = (size_t)(p - start);
        if (len >= 2 && len < 512) {
            char tok[512];
            memcpy(tok, start, len);
            tok[len] = '\0';
            int before = coa_filetracker_count(ft);
            record_if_exists(ft, tok, workspace);
            if (coa_filetracker_count(ft) > before) reads++;
        }
    }
    return reads;
}
