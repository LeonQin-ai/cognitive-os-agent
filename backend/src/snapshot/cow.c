#include "cognitive-os-agent/snapshot/cow.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct coa_cow {
    char dir[1024];
};

coa_cow *coa_cow_open(const char *blocks_dir) {
    coa_cow *c = calloc(1, sizeof(coa_cow));
    if (!c) return NULL;
    snprintf(c->dir, sizeof(c->dir), "%s", blocks_dir);
    if (coa_fs_mkdirs(c->dir) != 0) { free(c); return NULL; }
    return c;
}

void coa_cow_close(coa_cow *c) { free(c); }

const char *coa_cow_put(coa_cow *c, const void *data, size_t len) {
    static char hash[17];
    coa_hash_hex(hash, coa_hash64(data, len));

    char path[1100];
    coa_path_join(path, sizeof(path), c->dir, hash);
    if (!coa_fs_exists(path)) {
        if (coa_fs_write_file(path, data, len) != 0) return NULL;
    }
    return hash;
}

char *coa_cow_get(coa_cow *c, const char *hash, size_t *len) {
    char path[1100];
    coa_path_join(path, sizeof(path), c->dir, hash);
    char *data = coa_fs_read_file(path);
    if (!data) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = strlen(data);
    return data;
}
