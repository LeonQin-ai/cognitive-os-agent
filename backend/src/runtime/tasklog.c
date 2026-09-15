/* tasklog.c — durable task journal. See tasklog.h. */
#include "runtime/tasklog.h"
#include "infra/util.h"
#include "os/os_fs.h"
#include "os/os_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "cJSON.h"

#define TASKLOG_OUT_CAP 4096   /* per-record output head cap */
#define TASKLOG_FILE_MAX 32u * 1024 * 1024

struct tasklog {
    mutex_t mtx;
    char path[600]; /* <state_root>/tasks.jsonl */
};

static void tasklog_path(char *out, size_t n, const char *state_root) {
    char dir[600];

    path_join(dir, sizeof(dir), state_root ? state_root : "state", "journal");
    fs_mkdirs(dir);
    path_join(out, n, dir, "tasks.jsonl");
}

tasklog *tasklog_new(const char *state_root) {
    tasklog *tl = (tasklog *)calloc(1, sizeof(tasklog));

    if (!tl)
        return NULL;
    tasklog_path(tl->path, sizeof(tl->path), state_root);
    mutex_init(&tl->mtx);
    return tl;
}

void tasklog_free(tasklog *tl) {
    if (!tl)
        return;
    mutex_destroy(&tl->mtx);
    free(tl);
}

void tasklog_record(tasklog *tl, int64_t id, const char *status, const char *session, const char *input,
                    const char *output) {
    cJSON *o;
    char *line;
    FILE *f;

    if (!tl || id <= 0)
        return;
    o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddStringToObject(o, "status", status ? status : "?");
    cJSON_AddStringToObject(o, "session", session && *session ? session : "");
    cJSON_AddStringToObject(o, "input", input ? input : "");
    if (output && *output) {
        char head[TASKLOG_OUT_CAP];
        snprintf(head, sizeof(head), "%s", output);
        cJSON_AddStringToObject(o, "output", head);
    }
    cJSON_AddNumberToObject(o, "ts", (double)time(NULL) * 1000);
    line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!line)
        return;

    mutex_lock(&tl->mtx);
    f = fopen(tl->path, "ab");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
    mutex_unlock(&tl->mtx);
    free(line);
}

/* Parse up to `limit` (0 = all) newest records; returns a cJSON array or
 * NULL. `pick` (optional) filters by task id (any status). */
static cJSON *journal_parse(const char *path, int limit, int64_t pick_id) {
    FILE *f;
    cJSON *arr;
    char *buf;
    long len;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (long)TASKLOG_FILE_MAX) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';

    arr = cJSON_CreateArray();
    if (!arr) {
        free(buf);
        return NULL;
    }
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        cJSON *o = cJSON_Parse(line);
        if (!o)
            continue;
        if (pick_id > 0) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
            if (!id || !cJSON_IsNumber(id) || (int64_t)id->valuedouble != pick_id) {
                cJSON_Delete(o);
                continue;
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(buf);

    if (limit > 0 && cJSON_GetArraySize(arr) > limit) {
        /* keep the newest `limit` */
        int total = cJSON_GetArraySize(arr);
        for (int i = 0; i < total - limit; i++) {
            cJSON *victim = cJSON_DetachItemFromArray(arr, 0);
            cJSON_Delete(victim);
        }
    }
    return arr;
}

char *tasklog_json(tasklog *tl, int limit) {
    cJSON *arr;
    char *s;

    if (!tl)
        return xstrdup("[]");
    mutex_lock(&tl->mtx);
    arr = journal_parse(tl->path, limit > 0 ? limit : 0, 0);
    mutex_unlock(&tl->mtx);
    if (!arr)
        return xstrdup("[]");
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}

int tasklog_find(tasklog *tl, int64_t id, char **status, char **session, char **input, char **output) {
    cJSON *arr = NULL, *hit = NULL, *it;
    int found = 0;

    if (!tl || id <= 0)
        return 0;
    mutex_lock(&tl->mtx);
    arr = journal_parse(tl->path, 0, id);
    mutex_unlock(&tl->mtx);
    if (!arr)
        return 0;
    cJSON_ArrayForEach(it, arr) {
        cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "status");
        /* only terminal tasks are resumable */
        if (st && cJSON_IsString(st) && st->valuestring)
            hit = it;
    }
    if (hit) {
        found = 1;
        if (status) {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(hit, "status");
            *status = (st && cJSON_IsString(st) && st->valuestring) ? xstrdup(st->valuestring) : NULL;
        }
        if (session) {
            cJSON *se = cJSON_GetObjectItemCaseSensitive(hit, "session");
            *session = (se && cJSON_IsString(se) && se->valuestring && *se->valuestring) ? xstrdup(se->valuestring)
                                                                                         : NULL;
        }
        if (input) {
            cJSON *in = cJSON_GetObjectItemCaseSensitive(hit, "input");
            *input = (in && cJSON_IsString(in) && in->valuestring) ? xstrdup(in->valuestring) : NULL;
        }
        if (output) {
            cJSON *ou = cJSON_GetObjectItemCaseSensitive(hit, "output");
            *output = (ou && cJSON_IsString(ou) && ou->valuestring) ? xstrdup(ou->valuestring) : NULL;
        }
    }
    cJSON_Delete(arr);
    return found;
}
