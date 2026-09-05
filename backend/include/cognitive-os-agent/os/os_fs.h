/* os_fs.h — cross-platform filesystem helpers */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read an entire file into a NUL-terminated malloc'd string. NULL on error. */
char *coa_fs_read_file(const char *path);

/* Write bytes to a file (creates/truncates). 0 on success, -1 on error. */
int coa_fs_write_file(const char *path, const void *data, size_t len);

/* Append bytes to a file. 0 on success, -1 on error. */
int coa_fs_append_file(const char *path, const void *data, size_t len);

int coa_fs_exists(const char *path);
int coa_fs_is_dir(const char *path);
/* 64-bit file size in bytes (-1 if the path does not exist / stat fails). */
long long coa_fs_file_size(const char *path);
/* Recursively create a directory path. 0 ok, -1 on error. */
int coa_fs_mkdirs(const char *path);
/* Remove a file. 0 ok, -1 if missing/error. */
int coa_fs_remove(const char *path);

typedef struct coa_dir_entry {
    char *name;   /* base name only */
    int is_dir;
} coa_dir_entry;

typedef struct coa_dir_list {
    coa_dir_entry *items;
    size_t count;
    size_t cap;
} coa_dir_list;

/* List a directory. Returns 0 ok, -1 on error. Caller frees with coa_fs_list_free. */
int coa_fs_list_dir(const char *path, coa_dir_list *out);
void coa_fs_list_free(coa_dir_list *l);

#ifdef __cplusplus
}
#endif
