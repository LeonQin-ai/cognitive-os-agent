/* flow_store.c — persistent registry of multi-agent collaboration tasks. */
#include "runtime/flow_store.h"
#include "os/os_fs.h"
#include "os/os_time.h"
#include "os/os_thread.h"
#include "infra/util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

struct flow_store {
    mutex_t mtx;
    flow_record *recs;
    size_t count;
    size_t cap;
    int64_t next_id;   /* persistent record ids (never reused across restarts) */
    char path[600];    /* "" = persistence disabled */
};

static void rec_clear(flow_record *r) {
    free(r->name);
    free(r->input);
    free(r->dag_json);
    free(r->status);
    memset(r, 0, sizeof(*r));
}

static void save_locked(flow_store *fs) {
    cJSON *root, *arr;
    char *s;

    if (!fs->path[0])
        return;
    root = cJSON_CreateObject();
    arr = cJSON_AddArrayToObject(root, "records");
    if (!root || !arr) {
        cJSON_Delete(root);
        return;
    }
    for (size_t i = 0; i < fs->count; i++) {
        flow_record *r = &fs->recs[i];
        cJSON *o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddNumberToObject(o, "id", (double)r->id);
        cJSON_AddNumberToObject(o, "task_id", (double)r->task_id);
        cJSON_AddStringToObject(o, "name", r->name ? r->name : "");
        cJSON_AddStringToObject(o, "input", r->input ? r->input : "");
        cJSON_AddStringToObject(o, "dag", r->dag_json ? r->dag_json : "");
        cJSON_AddStringToObject(o, "status", r->status ? r->status : "?");
        cJSON_AddNumberToObject(o, "created_ms", (double)r->created_ms);
        cJSON_AddNumberToObject(o, "updated_ms", (double)r->updated_ms);
        cJSON_AddItemToArray(arr, o);
    }
    s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s) {
        fs_write_file(fs->path, s, strlen(s));
        free(s);
    }
}

flow_store *flow_store_new(void) {
    flow_store *fs = (flow_store *)calloc(1, sizeof(*fs));
    if (!fs)
        return NULL;
    mutex_init(&fs->mtx);
    return fs;
}

void flow_store_free(flow_store *fs) {
    if (!fs)
        return;
    mutex_lock(&fs->mtx);
    for (size_t i = 0; i < fs->count; i++)
        rec_clear(&fs->recs[i]);
    free(fs->recs);
    fs->recs = NULL;
    fs->count = fs->cap = 0;
    mutex_unlock(&fs->mtx);
    mutex_destroy(&fs->mtx);
    free(fs);
}

/* index of the record with this id, or -1 (caller holds the mutex) */
static int flow_store_find_id_locked(flow_store *fs, int64_t id) {
    for (size_t i = 0; i < fs->count; i++) {
        if (fs->recs[i].id == id)
            return (int)i;
    }
    return -1;
}

int flow_store_init(flow_store *fs, const char *state_root) {
    char *s;
    cJSON *root, *arr;
    int restored = 0;

    if (!fs)
        return -1;
    if (!state_root || !*state_root)
        return 0;
    snprintf(fs->path, sizeof(fs->path), "%s/flows.json", state_root);

    s = fs_read_file(fs->path);
    root = s ? cJSON_Parse(s) : NULL;
    free(s);
    arr = root ? cJSON_GetObjectItemCaseSensitive(root, "records") : NULL;
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return fs->path[0] ? 0 : -1;
    }
    mutex_lock(&fs->mtx);
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jtask = cJSON_GetObjectItemCaseSensitive(it, "task_id");
        cJSON *jname = cJSON_GetObjectItemCaseSensitive(it, "name");
        cJSON *jinput = cJSON_GetObjectItemCaseSensitive(it, "input");
        cJSON *jdag = cJSON_GetObjectItemCaseSensitive(it, "dag");
        cJSON *jstatus = cJSON_GetObjectItemCaseSensitive(it, "status");
        cJSON *jcreated = cJSON_GetObjectItemCaseSensitive(it, "created_ms");
        cJSON *jupdated = cJSON_GetObjectItemCaseSensitive(it, "updated_ms");
        flow_record r;
        memset(&r, 0, sizeof(r));
        r.id = cJSON_IsNumber(jid) ? (int64_t)jid->valuedouble : 0;
        /* pre-task_id files stored the scheduler id in "id": that task is gone
         * (different process), so it can never match a live task_done */
        r.task_id = cJSON_IsNumber(jtask) ? (int64_t)jtask->valuedouble : -1;
        r.name = xstrdup(cJSON_IsString(jname) && jname->valuestring ? jname->valuestring : "");
        r.input = xstrdup(cJSON_IsString(jinput) && jinput->valuestring ? jinput->valuestring : "");
        r.dag_json = xstrdup(cJSON_IsString(jdag) && jdag->valuestring ? jdag->valuestring : "");
        r.created_ms = cJSON_IsNumber(jcreated) ? (long long)jcreated->valuedouble : 0;
        r.updated_ms = cJSON_IsNumber(jupdated) ? (long long)jupdated->valuedouble : 0;
        /* a record still RUNNING when the process died never finished */
        if (cJSON_IsString(jstatus) && jstatus->valuestring && strcmp(jstatus->valuestring, "RUNNING") == 0)
            r.status = xstrdup("INTERRUPTED");
        else
            r.status = xstrdup(cJSON_IsString(jstatus) && jstatus->valuestring ? jstatus->valuestring : "?");
        /* records imported with duplicate ids (pre-split files could collide
         * across restarts) get re-keyed so ids stay unique */
        if (r.id < 0 || flow_store_find_id_locked(fs, r.id) >= 0)
            r.id = fs->next_id++;
        else if (r.id >= fs->next_id)
            fs->next_id = r.id + 1;
        if (fs->count == fs->cap) {
            size_t cap = fs->cap ? fs->cap * 2 : 8;
            flow_record *nb = (flow_record *)realloc(fs->recs, cap * sizeof(flow_record));
            if (!nb) {
                rec_clear(&r);
                break;
            }
            fs->recs = nb;
            fs->cap = cap;
        }
        fs->recs[fs->count++] = r;
        restored++;
    }
    save_locked(fs); /* persist the RUNNING→INTERRUPTED transitions */
    mutex_unlock(&fs->mtx);
    cJSON_Delete(root);
    return restored;
}

int flow_store_add(flow_store *fs, int64_t task_id, const char *name, const char *input, const char *dag_json) {
    if (!fs || task_id < 0 || !dag_json || !*dag_json)
        return -1;
    mutex_lock(&fs->mtx);
    if (fs->count == fs->cap) {
        size_t cap = fs->cap ? fs->cap * 2 : 8;
        flow_record *nb = (flow_record *)realloc(fs->recs, cap * sizeof(flow_record));
        if (!nb) {
            mutex_unlock(&fs->mtx);
            return -1;
        }
        fs->recs = nb;
        fs->cap = cap;
    }
    flow_record *r = &fs->recs[fs->count++];
    memset(r, 0, sizeof(*r));
    r->id = fs->next_id++;
    r->task_id = task_id;
    r->name = xstrdup(name ? name : "");
    r->input = xstrdup(input ? input : "");
    r->dag_json = xstrdup(dag_json);
    r->status = xstrdup("RUNNING");
    r->created_ms = r->updated_ms = time_now_ms();
    save_locked(fs);
    mutex_unlock(&fs->mtx);
    return 0;
}

void flow_store_mark(flow_store *fs, int64_t task_id, const char *status) {
    if (!fs || !status || !*status)
        return;
    mutex_lock(&fs->mtx);
    for (size_t i = 0; i < fs->count; i++) {
        if (fs->recs[i].task_id == task_id) {
            free(fs->recs[i].status);
            fs->recs[i].status = xstrdup(status);
            fs->recs[i].updated_ms = time_now_ms();
            save_locked(fs);
            break;
        }
    }
    mutex_unlock(&fs->mtx);
}

int flow_store_modify(flow_store *fs, int64_t id, const char *name, const char *input, const char *dag_json) {
    int rc = -1;

    if (!fs)
        return -1;
    mutex_lock(&fs->mtx);
    for (size_t i = 0; i < fs->count; i++) {
        flow_record *r = &fs->recs[i];
        if (r->id != id)
            continue;
        if (r->status && strcmp(r->status, "RUNNING") == 0)
            break; /* cannot edit a live run */
        if (name) {
            free(r->name);
            r->name = xstrdup(name);
        }
        if (input) {
            free(r->input);
            r->input = xstrdup(input);
        }
        if (dag_json && *dag_json) {
            free(r->dag_json);
            r->dag_json = xstrdup(dag_json);
        }
        r->updated_ms = time_now_ms();
        save_locked(fs);
        rc = 0;
        break;
    }
    mutex_unlock(&fs->mtx);
    return rc;
}

const flow_record *flow_store_find(flow_store *fs, int64_t id) {
    const flow_record *r = NULL;

    if (!fs)
        return NULL;
    mutex_lock(&fs->mtx);
    for (size_t i = 0; i < fs->count; i++) {
        if (fs->recs[i].id == id) {
            r = &fs->recs[i];
            break;
        }
    }
    mutex_unlock(&fs->mtx);
    return r;
}

size_t flow_store_count(flow_store *fs) {
    size_t n;

    if (!fs)
        return 0;
    mutex_lock(&fs->mtx);
    n = fs->count;
    mutex_unlock(&fs->mtx);
    return n;
}

const flow_record *flow_store_at(flow_store *fs, size_t i) {
    const flow_record *r = NULL;

    if (!fs)
        return NULL;
    mutex_lock(&fs->mtx);
    if (i < fs->count)
        r = &fs->recs[i];
    mutex_unlock(&fs->mtx);
    return r;
}

char *flow_store_json(flow_store *fs) {
    cJSON *arr;
    char *s;

    if (!fs)
        return xstrdup("[]");
    arr = cJSON_CreateArray();
    if (!arr)
        return xstrdup("[]");
    mutex_lock(&fs->mtx);
    for (size_t i = fs->count; i-- > 0;) { /* newest first */
        flow_record *r = &fs->recs[i];
        cJSON *o = cJSON_CreateObject();
        if (!o)
            break;
        cJSON_AddNumberToObject(o, "id", (double)r->id);
        cJSON_AddNumberToObject(o, "task_id", (double)r->task_id);
        cJSON_AddStringToObject(o, "name", r->name ? r->name : "");
        cJSON_AddStringToObject(o, "input", r->input ? r->input : "");
        cJSON_AddStringToObject(o, "dag", r->dag_json ? r->dag_json : "");
        cJSON_AddStringToObject(o, "status", r->status ? r->status : "?");
        cJSON_AddNumberToObject(o, "created_ms", (double)r->created_ms);
        cJSON_AddNumberToObject(o, "updated_ms", (double)r->updated_ms);
        cJSON_AddItemToArray(arr, o);
    }
    mutex_unlock(&fs->mtx);
    s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s ? s : xstrdup("[]");
}
