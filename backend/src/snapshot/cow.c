#include "cagent/snapshot/cow.h"
#include "cagent/infra/util.h"
#include "cagent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct ca_cow {
    char dir[1024];
};

ca_cow *ca_cow_open(const char *blocks_dir) {
    ca_cow *c = calloc(1, sizeof(ca_cow));
    if (!c) return NULL;
    snprintf(c->dir, sizeof(c->dir), "%s", blocks_dir);
    if (ca_fs_mkdirs(c->dir) != 0) { free(c); return NULL; }
    return c;
}

void ca_cow_close(ca_cow *c) { free(c); }

const char *ca_cow_put(ca_cow *c, const void *data, size_t len) {
    static char hash[17];
    ca_hash_hex(hash, ca_hash64(data, len));

    char path[1100];
    ca_path_join(path, sizeof(path), c->dir, hash);
    if (!ca_fs_exists(path)) {
        if (ca_fs_write_file(path, data, len) != 0) return NULL;
    }
    return hash;
}

char *ca_cow_get(ca_cow *c, const char *hash, size_t *len) {
    char path[1100];
    ca_path_join(path, sizeof(path), c->dir, hash);
    char *data = ca_fs_read_file(path);
    if (!data) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = strlen(data);
    return data;
}
