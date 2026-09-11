/* os_fs.h — cross-platform filesystem helpers */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read an entire file into a NUL-terminated malloc'd string. NULL on error. */
char *fs_read_file(const char *path);

/* Write bytes to a file (creates/truncates). 0 on success, -1 on error. */
int fs_write_file(const char *path, const void *data, size_t len);

/* Append bytes to a file. 0 on success, -1 on error. */
int fs_append_file(const char *path, const void *data, size_t len);

int fs_exists(const char *path);
int fs_is_dir(const char *path);
/* 64-bit file size in bytes (-1 if the path does not exist / stat fails). */
long long fs_file_size(const char *path);
/* Recursively create a directory path. 0 ok, -1 on error. */
int fs_mkdirs(const char *path);
/* Remove a file. 0 ok, -1 if missing/error. */
int fs_remove(const char *path);

typedef struct dir_entry {
    char *name; /* base name only */
    int is_dir;
} dir_entry;

typedef struct dir_list {
    dir_entry *items;
    size_t count;
    size_t cap;
} dir_list;

/* List a directory. Returns 0 ok, -1 on error. Caller frees with fs_list_free. */
int fs_list_dir(const char *path, dir_list *out);
void fs_list_free(dir_list *l);

#ifdef __cplusplus
}
#endif
