#include "cognitive-os-agent/infra/persist.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/os_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char g_root[512] = "state";

int coa_persist_set_root(const char *dir) {
    if (!dir || !*dir) return -1;
    snprintf(g_root, sizeof(g_root), "%s", dir);
    return coa_fs_mkdirs(g_root) == 0 ? 0 : -1;
}

const char *coa_persist_root(void) { return g_root; }

void coa_persist_path(char *out, size_t n, const char *sub, const char *file) {
    if (sub && *sub) {
        coa_path_join(out, n, g_root, sub);
        if (file && *file) coa_path_join(out, n, out, file);
    } else if (file && *file) {
        coa_path_join(out, n, g_root, file);
    } else {
        snprintf(out, n, "%s", g_root);
    }
}

int coa_persist_ensure(const char *sub) {
    char p[1024];
    coa_persist_path(p, sizeof(p), sub, NULL);
    return coa_fs_mkdirs(p) == 0 ? 0 : -1;
}

char *coa_persist_read(const char *sub, const char *file) {
    char p[1024];
    coa_persist_path(p, sizeof(p), sub, file);
    return coa_fs_read_file(p);
}

int coa_persist_write(const char *sub, const char *file, const char *text) {
    if (coa_persist_ensure(sub) != 0) return -1;
    char p[1024];
    coa_persist_path(p, sizeof(p), sub, file);
    return coa_fs_write_file(p, text, strlen(text));
}
