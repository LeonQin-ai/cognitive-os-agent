#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cognitive-os-agent/os/os_fs.h"
#include "cognitive-os-agent/infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <sys/stat.h>
#endif

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

char *coa_fs_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

int coa_fs_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int ok = (w == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int coa_fs_append_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int ok = (w == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int coa_fs_exists(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
}

int coa_fs_is_dir(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int coa_fs_mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    for (size_t i = 0; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            if (i == 0) { continue; } /* skip drive root */
            char ch = tmp[i];
            tmp[i] = '\0';
            if (!coa_fs_exists(tmp)) {
                if (_mkdir(tmp) != 0) { tmp[i] = ch; return -1; }
            }
            tmp[i] = ch;
        }
    }
    if (!coa_fs_is_dir(path)) {
        if (_mkdir(path) != 0 && !coa_fs_exists(path)) return -1;
    }
    return 0;
}

int coa_fs_remove(const char *path) {
    return DeleteFileA(path) ? 0 : -1;
}

int coa_fs_list_dir(const char *path, coa_dir_list *out) {
    memset(out, 0, sizeof(*out));
    char pattern[1200];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (out->count == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 16;
            out->items = realloc(out->items, cap * sizeof(coa_dir_entry));
            out->cap = cap;
        }
        out->items[out->count].name = coa_strdup(fd.cFileName);
        out->items[out->count].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

#else /* POSIX */

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

char *coa_fs_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

int coa_fs_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int ok = (w == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int coa_fs_append_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    int ok = (w == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int coa_fs_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int coa_fs_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int coa_fs_mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { tmp[i] = '/'; return -1; }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int coa_fs_remove(const char *path) {
    return remove(path) == 0 ? 0 : -1;
}

int coa_fs_list_dir(const char *path, coa_dir_list *out) {
    memset(out, 0, sizeof(*out));
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (out->count == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 16;
            out->items = realloc(out->items, cap * sizeof(coa_dir_entry));
            out->cap = cap;
        }
        out->items[out->count].name = coa_strdup(de->d_name);
        out->items[out->count].is_dir = (de->d_type == DT_DIR);
        out->count++;
    }
    closedir(d);
    return 0;
}

#endif

/* 64-bit file size (-1 if missing/stat error). Uses __stat64 on Windows:
 * plain stat's 32-bit off_t overflows past 2 GB. */
long long coa_fs_file_size(const char *path) {
#ifdef _WIN32
    struct __stat64 st;
    if (_stat64(path, &st) != 0) return -1;
    return (long long)st.st_size;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
#endif
}

void coa_fs_list_free(coa_dir_list *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i].name);
    free(l->items);
    memset(l, 0, sizeof(*l));
}
