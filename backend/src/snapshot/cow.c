#include "cognitive-os-agent/snapshot/cow.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct cow {
    char dir[1024];
};

cow *cow_open(const char *blocks_dir) {
    cow *c = calloc(1, sizeof(cow));
    if (!c)
        return NULL;
    snprintf(c->dir, sizeof(c->dir), "%s", blocks_dir);
    if (fs_mkdirs(c->dir) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

void cow_close(cow *c) {
    free(c);
}

const char *cow_put(cow *c, const void *data, size_t len) {
    static char hash[17];
    hash_hex(hash, hash64(data, len));

    char path[1100];
    path_join(path, sizeof(path), c->dir, hash);
    if (!fs_exists(path)) {
        if (fs_write_file(path, data, len) != 0)
            return NULL;
    }
    return hash;
}

char *cow_get(cow *c, const char *hash, size_t *len) {
    char path[1100];
    path_join(path, sizeof(path), c->dir, hash);
    char *data = fs_read_file(path);
    if (!data) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = strlen(data);
    return data;
}
