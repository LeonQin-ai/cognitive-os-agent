#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_fs.h"
#include "infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

char *fs_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    size_t rd;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }

    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

int fs_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    size_t w;
    int ok;

    if (!f)
        return -1;
    w = fwrite(data, 1, len, f);
    ok = (w == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int fs_append_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "ab");
    size_t w;
    int ok;

    if (!f)
        return -1;
    w = fwrite(data, 1, len, f);
    ok = (w == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int fs_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int fs_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fs_mkdirs(const char *path) {
    char tmp[1024];
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0)
        return -1;
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                tmp[i] = '/';
                return -1;
            }
            tmp[i] = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int fs_remove(const char *path) {
    return remove(path) == 0 ? 0 : -1;
}

int fs_list_dir(const char *path, dir_list *out) {
    struct dirent *de;

    memset(out, 0, sizeof(*out));
    DIR *d = opendir(path);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (out->count == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 16;
            out->items = realloc(out->items, cap * sizeof(dir_entry));
            out->cap = cap;
        }
        out->items[out->count].name = xstrdup(de->d_name);
        out->items[out->count].is_dir = (de->d_type == DT_DIR);
        out->count++;
    }

    closedir(d);
    return 0;
}


/* 64-bit file size (-1 if missing/stat error). */
long long fs_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long long)st.st_size;
}

void fs_list_free(dir_list *l) {
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i].name);
    free(l->items);
    memset(l, 0, sizeof(*l));
}
