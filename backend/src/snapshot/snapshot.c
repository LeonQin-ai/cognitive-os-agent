#include "cognitive-os-agent/snapshot/snapshot.h"
#include "cognitive-os-agent/snapshot/cow.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* Files larger than this are NOT captured (reading a 10G+ file into memory
 * and duplicating it into the block store is not viable). Rollback cannot
 * restore their content — such files should be protected by git or backups.
 * Override with env COA_SNAPSHOT_MAX_FILE (bytes; 0 = unlimited). */
#define SNAPSHOT_DEFAULT_MAX_FILE (64LL * 1024 * 1024)

typedef struct captured {
    char *path;
    char hash[17];   /* content hash of the original; empty if file didn't exist */
    int existed;
    int skipped;     /* existed but too large to capture; rollback leaves it as-is */
} captured;

typedef struct snapshot_entry {
    char id[32];
    char created[40];
    captured *files;
    size_t nfiles;
} snapshot_entry;

struct coa_snapshot {
    char root[512];
    coa_cow *cow;
    long long max_file;      /* capture size limit in bytes; 0 = unlimited */
    captured *pending;
    size_t pending_count, pending_cap;
    snapshot_entry *committed;
    size_t committed_count, committed_cap;
    char last_id[32];
};

static int coa_snapshot_restore_from_manifest(coa_snapshot *s, const char *json_text, const char *fname);

coa_snapshot *coa_snapshot_open(const char *state_root) {
    coa_snapshot *s = calloc(1, sizeof(coa_snapshot));
    if (!s) return NULL;
    snprintf(s->root, sizeof(s->root), "%s", state_root);
    s->max_file = SNAPSHOT_DEFAULT_MAX_FILE;
    {
        const char *env = getenv("COA_SNAPSHOT_MAX_FILE");
        if (env && *env) {
            long long v = atoll(env);
            if (v >= 0) s->max_file = v; /* 0 disables the limit entirely */
        }
    }
    char blocks[600];
    coa_path_join(blocks, sizeof(blocks), state_root, "snapshots/blocks");
    s->cow = coa_cow_open(blocks);
    if (!s->cow) { free(s); return NULL; }

    /* load committed snapshots from state_root/snapshots/ */
    char manifest_dir[600];
    coa_path_join(manifest_dir, sizeof(manifest_dir), state_root, "snapshots");
    coa_dir_list dl;
    if (coa_fs_list_dir(manifest_dir, &dl) == 0) {
        for (size_t i = 0; i < dl.count; i++) {
            if (dl.items[i].is_dir) continue;
            size_t len = strlen(dl.items[i].name);
            if (len < 5 || strcmp(dl.items[i].name + len - 5, ".json") != 0) continue;
            char mpath[700];
            coa_path_join(mpath, sizeof(mpath), manifest_dir, dl.items[i].name);
            char *text = coa_fs_read_file(mpath);
            if (text) {
                coa_snapshot_restore_from_manifest(s, text, dl.items[i].name);
                free(text);
            }
        }
        coa_fs_list_free(&dl);
    }
    return s;
}

void coa_snapshot_set_max_file(coa_snapshot *s, long long bytes) {
    if (!s || bytes < 0) return;
    s->max_file = bytes;
}

long long coa_snapshot_get_max_file(const coa_snapshot *s) {
    return s ? s->max_file : -1;
}

/* helper used above to rebuild committed list from a persisted manifest */
static int coa_snapshot_restore_from_manifest(coa_snapshot *s, const char *json_text, const char *fname) {
    cJSON *root = cJSON_Parse(json_text);
    if (!root || !cJSON_IsObject(root)) { if (root) cJSON_Delete(root); return -1; }
    snapshot_entry *e = calloc(1, sizeof(snapshot_entry));
    if (!e) { cJSON_Delete(root); return -1; }
    /* id from filename minus .json */
    snprintf(e->id, sizeof(e->id), "%.*s", (int)(strlen(fname) > 5 ? strlen(fname) - 5 : 0), fname);
    cJSON *created = cJSON_GetObjectItemCaseSensitive(root, "created");
    if (created && cJSON_IsString(created)) snprintf(e->created, sizeof(e->created), "%s", created->valuestring);
    cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (files && cJSON_IsArray(files)) {
        e->files = calloc((size_t)cJSON_GetArraySize(files), sizeof(captured));
        e->nfiles = (size_t)cJSON_GetArraySize(files);
        cJSON *it;
        size_t idx = 0;
        cJSON_ArrayForEach(it, files) {
            captured *cap = &e->files[idx++];
            cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "path");
            cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "hash");
            cJSON *ex = cJSON_GetObjectItemCaseSensitive(it, "existed");
            cJSON *sk = cJSON_GetObjectItemCaseSensitive(it, "skipped");
            cap->path = (p && cJSON_IsString(p)) ? coa_strdup(p->valuestring) : coa_strdup("");
            if (h && cJSON_IsString(h)) snprintf(cap->hash, sizeof(cap->hash), "%s", h->valuestring);
            cap->existed = ex ? cJSON_IsTrue(ex) : 1;
            cap->skipped = sk ? cJSON_IsTrue(sk) : 0;
        }
    }
    if (s->committed_count == s->committed_cap) {
        size_t cap = s->committed_cap ? s->committed_cap * 2 : 8;
        s->committed = realloc(s->committed, cap * sizeof(snapshot_entry));
        s->committed_cap = cap;
    }
    s->committed[s->committed_count++] = *e;
    free(e);
    cJSON_Delete(root);
    return 0;
}

void coa_snapshot_close(coa_snapshot *s) {
    if (!s) return;
    for (size_t i = 0; i < s->pending_count; i++) free(s->pending[i].path);
    free(s->pending);
    for (size_t i = 0; i < s->committed_count; i++) {
        for (size_t j = 0; j < s->committed[i].nfiles; j++) free(s->committed[i].files[j].path);
        free(s->committed[i].files);
    }
    free(s->committed);
    coa_cow_close(s->cow);
    free(s);
}

int coa_snapshot_capture(coa_snapshot *s, const char *path) {
    captured cap;
    memset(&cap, 0, sizeof(cap));
    cap.path = coa_strdup(path);
    cap.existed = coa_fs_exists(path) && !coa_fs_is_dir(path);
    if (cap.existed) {
        long long sz = coa_fs_file_size(path);
        if (s->max_file > 0 && sz > s->max_file) {
            /* too large to copy into the block store: mark skipped. Rollback
             * will leave the file untouched instead of deleting it. */
            cap.skipped = 1;
        } else {
            char *content = coa_fs_read_file(path);
            if (content) {
                const char *h = coa_cow_put(s->cow, content, strlen(content));
                if (h) snprintf(cap.hash, sizeof(cap.hash), "%s", h);
                free(content);
            }
        }
    }
    if (s->pending_count == s->pending_cap) {
        size_t capn = s->pending_cap ? s->pending_cap * 2 : 8;
        s->pending = realloc(s->pending, capn * sizeof(captured));
        s->pending_cap = capn;
    }
    s->pending[s->pending_count++] = cap;
    return 0;
}

int coa_snapshot_capture_json(coa_snapshot *s, const char *paths_json) {
    cJSON *arr = cJSON_Parse(paths_json);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return -1; }
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it)) coa_snapshot_capture(s, it->valuestring);
    }
    cJSON_Delete(arr);
    return 0;
}

const char *coa_snapshot_commit(coa_snapshot *s) {
    if (s->pending_count == 0) return NULL;
    char id[32];
    snprintf(id, sizeof(id), "s%lld", (long long)coa_time_now_ms());
    char created[40];
    coa_time_now_iso(created, sizeof(created));

    /* persist manifest */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "created", created);
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    for (size_t i = 0; i < s->pending_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", s->pending[i].path);
        cJSON_AddStringToObject(o, "hash", s->pending[i].hash);
        cJSON_AddBoolToObject(o, "existed", s->pending[i].existed ? 1 : 0);
        if (s->pending[i].skipped) cJSON_AddBoolToObject(o, "skipped", 1);
        cJSON_AddItemToArray(files, o);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char manifest[700];
    char fname[64];
    snprintf(fname, sizeof(fname), "%s.json", id);
    coa_path_join(manifest, sizeof(manifest), s->root, "snapshots");
    coa_fs_mkdirs(manifest);
    coa_path_join(manifest, sizeof(manifest), manifest, fname);
    if (text) {
        coa_fs_write_file(manifest, text, strlen(text));
        free(text);
    }

    /* move pending into committed */
    if (s->committed_count == s->committed_cap) {
        size_t cap = s->committed_cap ? s->committed_cap * 2 : 8;
        s->committed = realloc(s->committed, cap * sizeof(snapshot_entry));
        s->committed_cap = cap;
    }
    snapshot_entry *e = &s->committed[s->committed_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->id, sizeof(e->id), "%s", id);
    snprintf(e->created, sizeof(e->created), "%s", created);
    e->files = s->pending;
    e->nfiles = s->pending_count;
    s->pending = NULL;
    s->pending_count = s->pending_cap = 0;

    snprintf(s->last_id, sizeof(s->last_id), "%s", id);
    return s->last_id;
}

void coa_snapshot_abort(coa_snapshot *s) {
    for (size_t i = 0; i < s->pending_count; i++) free(s->pending[i].path);
    free(s->pending);
    s->pending = NULL;
    s->pending_count = s->pending_cap = 0;
}

char *coa_snapshot_list(coa_snapshot *s) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s->committed_count; i++) {
        snapshot_entry *e = &s->committed[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", e->id);
        cJSON_AddStringToObject(o, "created", e->created);
        cJSON_AddNumberToObject(o, "files", e->nfiles);
        cJSON *fl = cJSON_AddArrayToObject(o, "paths");
        for (size_t j = 0; j < e->nfiles; j++) cJSON_AddItemToArray(fl, cJSON_CreateString(e->files[j].path));
        cJSON_AddItemToArray(arr, o);
    }
    char *s_out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s_out ? s_out : coa_strdup("[]");
}

static int restore_entry(coa_snapshot *s, snapshot_entry *e) {
    for (size_t i = 0; i < e->nfiles; i++) {
        captured *cap = &e->files[i];
        if (cap->existed && cap->skipped) {
            /* original content was too large to capture — leave the (modified)
             * file alone rather than deleting a huge file we cannot restore */
            continue;
        }
        if (cap->existed && cap->hash[0]) {
            size_t blen = 0;
            char *blob = coa_cow_get(s->cow, cap->hash, &blen);
            if (blob) {
                coa_fs_write_file(cap->path, blob, blen);
                free(blob);
            }
        } else {
            coa_fs_remove(cap->path);
        }
    }
    return 0;
}

int coa_snapshot_restore_latest(coa_snapshot *s) {
    if (s->committed_count == 0) return -1;
    return restore_entry(s, &s->committed[s->committed_count - 1]);
}

int coa_snapshot_restore(coa_snapshot *s, const char *id) {
    for (size_t i = 0; i < s->committed_count; i++) {
        if (strcmp(s->committed[i].id, id) == 0)
            return restore_entry(s, &s->committed[i]);
    }
    return -1;
}

int coa_snapshot_restore_pending(coa_snapshot *s) {
    snapshot_entry e;
    memset(&e, 0, sizeof(e));
    e.files = s->pending;
    e.nfiles = s->pending_count;
    int rc = restore_entry(s, &e);
    coa_snapshot_abort(s);
    return rc;
}
