/* filetracker.c — sandbox file-access tracking: path->ops registry plus
 * before/after workspace scanning and command-token read detection. */
#include "cagent/plugin_runtime/filetracker.h"
#include "cagent/os/os_fs.h"
#include "cagent/os/os_thread.h"
#include "cagent/infra/util.h"

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

struct ca_filetracker {
    ca_mutex mtx;
    ft_entry *head;
    int count;
};

/* snapshot: flat array of {full path, size} */
typedef struct ft_snap_entry {
    char *path;         /* full path */
    long long size;
} ft_snap_entry;

struct ca_ft_snapshot {
    ft_snap_entry *items;
    size_t count;
};

ca_filetracker *ca_filetracker_new(void) {
    ca_filetracker *ft = calloc(1, sizeof(*ft));
    if (!ft) return NULL;
    ca_mutex_init(&ft->mtx);
    return ft;
}

void ca_filetracker_free(ca_filetracker *ft) {
    if (!ft) return;
    ca_filetracker_clear(ft);
    ca_mutex_destroy(&ft->mtx);
    free(ft);
}

void ca_filetracker_clear(ca_filetracker *ft) {
    if (!ft) return;
    ca_mutex_lock(&ft->mtx);
    ft_entry *e = ft->head;
    while (e) {
        ft_entry *n = e->next;
        free(e->path);
        free(e);
        e = n;
    }
    ft->head = NULL;
    ft->count = 0;
    ca_mutex_unlock(&ft->mtx);
}

int ca_filetracker_record(ca_filetracker *ft, const char *path, int ops) {
    if (!ft || !path || !*path || ops <= 0) return 0;
    ca_mutex_lock(&ft->mtx);
    for (ft_entry *e = ft->head; e; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            e->ops |= ops;
            int acc = e->ops;
            ca_mutex_unlock(&ft->mtx);
            return acc;
        }
    }
    if (ft->count < FT_MAX_ENTRIES) {
        ft_entry *e = calloc(1, sizeof(*e));
        if (e) {
            e->path = ca_strdup(path);
            if (e->path) {
                e->ops = ops;
                e->next = ft->head;
                ft->head = e;
                ft->count++;
                int acc = e->ops;
                ca_mutex_unlock(&ft->mtx);
                return acc;
            }
            free(e);
        }
    }
    ca_mutex_unlock(&ft->mtx);
    return 0;
}

int ca_filetracker_count(ca_filetracker *ft) {
    if (!ft) return 0;
    ca_mutex_lock(&ft->mtx);
    int n = ft->count;
    ca_mutex_unlock(&ft->mtx);
    return n;
}

const char *ca_filetracker_ops_str(int ops) {
    static char buf[40];
    buf[0] = '\0';
    if (ops & CA_FT_READ)  strcat(buf, "read");
    if (ops & CA_FT_WRITE) { if (buf[0]) strcat(buf, ","); strcat(buf, "write"); }
    if (ops & CA_FT_DELETE) { if (buf[0]) strcat(buf, ","); strcat(buf, "delete"); }
    if (ops & CA_FT_EXEC) { if (buf[0]) strcat(buf, ","); strcat(buf, "exec"); }
    if (!buf[0]) strcat(buf, "none");
    return buf;
}

char *ca_filetracker_json(ca_filetracker *ft) {
    if (!ft) return ca_strdup("[]");
    ca_mutex_lock(&ft->mtx);
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
        cJSON_AddStringToObject(o, "ops", ca_filetracker_ops_str(opsv[k]));
        cJSON_AddItemToArray(arr, o);
    }
    free(paths);
    free(opsv);
    ca_mutex_unlock(&ft->mtx);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : ca_strdup("[]");
}

/* ---------- workspace snapshot / diff ---------- */

static void snap_add(ca_ft_snapshot *s, const char *path, long long size) {
    if (!s || s->count >= FT_SCAN_MAX_ENTRIES) return;
    ft_snap_entry *e = &s->items[s->count];
    e->path = ca_strdup(path);
    if (!e->path) return;
    e->size = size;
    s->count++;
}

static void scan_dir(ca_ft_snapshot *s, const char *dir, int depth) {
    if (!s || !dir || depth > FT_SCAN_MAX_DEPTH) return;
    ca_dir_list list;
    memset(&list, 0, sizeof(list));
    if (ca_fs_list_dir(dir, &list) != 0) return;
    for (size_t i = 0; i < list.count; i++) {
        if (s->count >= FT_SCAN_MAX_ENTRIES) break;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, list.items[i].name);
        if (list.items[i].is_dir) {
            scan_dir(s, full, depth + 1);
        } else {
            long long sz = ca_fs_file_size(full);
            if (sz >= 0) snap_add(s, full, sz);
        }
    }
    ca_fs_list_free(&list);
}

ca_ft_snapshot *ca_filetracker_dir_snapshot(const char *dir) {
    ca_ft_snapshot *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->items = calloc(FT_SCAN_MAX_ENTRIES, sizeof(ft_snap_entry));
    if (!s->items) { free(s); return NULL; }
    if (dir && *dir && ca_fs_is_dir(dir)) scan_dir(s, dir, 0);
    return s;
}

void ca_filetracker_snapshot_free(ca_ft_snapshot *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) free(s->items[i].path);
    free(s->items);
    free(s);
}

static int snap_find(const ca_ft_snapshot *s, const char *path) {
    for (size_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].path, path) == 0) return (int)i;
    return -1;
}

int ca_filetracker_dir_diff(ca_filetracker *ft, const ca_ft_snapshot *before,
                            const char *dir) {
    if (!ft || !before) return 0;
    ca_ft_snapshot *after = ca_filetracker_dir_snapshot(dir);
    if (!after) return 0;
    int changes = 0;
    /* new / modified */
    for (size_t i = 0; i < after->count; i++) {
        int j = snap_find(before, after->items[i].path);
        if (j < 0 || before->items[j].size != after->items[i].size) {
            ca_filetracker_record(ft, after->items[i].path, CA_FT_WRITE);
            changes++;
        }
    }
    /* vanished */
    for (size_t i = 0; i < before->count; i++) {
        if (snap_find(after, before->items[i].path) < 0) {
            ca_filetracker_record(ft, before->items[i].path, CA_FT_DELETE);
            changes++;
        }
    }
    ca_filetracker_snapshot_free(after);
    return changes;
}

/* ---------- command read detection ---------- */

static void record_if_exists(ca_filetracker *ft, const char *token,
                             const char *workspace) {
    if (!token || !*token) return;
    if (ca_fs_exists(token)) {
        ca_filetracker_record(ft, token, CA_FT_READ);
        return;
    }
    if (workspace && *workspace) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", workspace, token);
        if (ca_fs_exists(full))
            ca_filetracker_record(ft, full, CA_FT_READ);
    }
}

int ca_filetracker_cmd_reads(ca_filetracker *ft, const char *cmd,
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
            int before = ca_filetracker_count(ft);
            record_if_exists(ft, tok, workspace);
            if (ca_filetracker_count(ft) > before) reads++;
        }
    }
    return reads;
}
