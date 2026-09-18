#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "os/os_fs.h"
#include "infra/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>

/* UTF-8 path -> malloc'd UTF-16 (caller frees); NULL on invalid input. Paths
 * arrive as UTF-8 (LLM args, HTTP bodies) but the ANSI Win32 APIs interpret
 * them as GBK on zh-CN systems, so any non-ASCII path fails. Always convert
 * and use the wide APIs. */
static wchar_t *fs_wpath(const char *utf8) {
    int n;
    wchar_t *w;
    if (!utf8)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

/* UTF-16 -> malloc'd UTF-8 (caller frees); NULL on invalid input. */
static char *fs_utf8_from_wide(const wchar_t *w) {
    int n;
    char *s;
    if (!w)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    s = malloc((size_t)n);
    if (!s)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    return s;
}

char *fs_read_file(const char *path) {
    wchar_t *w = fs_wpath(path);
    FILE *f = w ? _wfopen(w, L"rb") : NULL;
    long n;
    char *buf;
    size_t rd;

    free(w);
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
    wchar_t *w = fs_wpath(path);
    FILE *f = w ? _wfopen(w, L"wb") : NULL;
    size_t w_count;
    int ok;

    free(w);
    if (!f)
        return -1;
    w_count = fwrite(data, 1, len, f);
    ok = (w_count == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int fs_append_file(const char *path, const void *data, size_t len) {
    wchar_t *w = fs_wpath(path);
    FILE *f = w ? _wfopen(w, L"ab") : NULL;
    size_t w_count;
    int ok;

    free(w);
    if (!f)
        return -1;
    w_count = fwrite(data, 1, len, f);
    ok = (w_count == len) ? 0 : -1;
    fclose(f);
    return ok;
}

int fs_exists(const char *path) {
    wchar_t *w = fs_wpath(path);
    DWORD attr = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES;
}

int fs_is_dir(const char *path) {
    wchar_t *w = fs_wpath(path);
    DWORD attr = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int fs_mkdirs(const char *path) {
    wchar_t *w = fs_wpath(path);
    wchar_t tmp[1024];
    size_t len, i;

    if (!w)
        return -1;
    len = wcslen(w);
    if (len == 0 || len >= 1024) {
        free(w);
        return -1;
    }
    wcscpy(tmp, w);
    free(w);
    for (i = 0; i <= len; i++) {
        if (tmp[i] == L'/' || tmp[i] == L'\\') {
            wchar_t ch;
            if (i == 0 || tmp[i - 1] == L':')
                continue; /* skip drive root ("D:\") */
            ch = tmp[i];
            tmp[i] = L'\0';
            if (GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES) {
                if (_wmkdir(tmp) != 0 && GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES) {
                    tmp[i] = ch;
                    return -1;
                }
            }
            tmp[i] = ch;
        }
    }

    if (GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES) {
        if (_wmkdir(tmp) != 0 && GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES)
            return -1;
    }

    return 0;
}

int fs_remove(const char *path) {
    wchar_t *w = fs_wpath(path);
    int ok = w ? (DeleteFileW(w) ? 0 : -1) : -1;
    free(w);
    return ok;
}

int fs_list_dir(const char *path, dir_list *out) {
    wchar_t *w = fs_wpath(path);
    wchar_t pattern[1200];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    memset(out, 0, sizeof(*out));
    if (!w)
        return -1;
    _snwprintf(pattern, 1200, L"%ls\\*", w);
    pattern[1199] = L'\0';
    free(w);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (out->count == out->cap) {
            size_t cap = out->cap ? out->cap * 2 : 16;
            out->items = realloc(out->items, cap * sizeof(dir_entry));
            out->cap = cap;
        }
        out->items[out->count].name = fs_utf8_from_wide(fd.cFileName);
        if (!out->items[out->count].name)
            out->items[out->count].name = xstrdup("?");
        out->items[out->count].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}


/* 64-bit file size (-1 if missing/stat error). Uses __stat64 on Windows:
 * plain stat's 32-bit off_t overflows past 2 GB. */
long long fs_file_size(const char *path) {
    wchar_t *w = fs_wpath(path);
    struct __stat64 st;
    long long sz = -1;
    if (w && _wstat64(w, &st) == 0)
        sz = (long long)st.st_size;
    free(w);
    return sz;
}

void fs_list_free(dir_list *l) {
    for (size_t i = 0; i < l->count; i++)
        free(l->items[i].name);
    free(l->items);
    memset(l, 0, sizeof(*l));
}
