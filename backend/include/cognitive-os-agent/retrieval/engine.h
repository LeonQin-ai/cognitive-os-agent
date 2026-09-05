/* index.h — lightweight code/document inverted index.
 * Terms map to (file, line) occurrences. Used by the knowledge system to answer
 * "where is X defined / used". Built by scanning source trees. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ca_index ca_index;

ca_index *ca_index_new(void);
void ca_index_free(ca_index *idx);

/* Index a single file's content (term -> file:line). */
int ca_index_add_file(ca_index *idx, const char *path, const char *content);

/* Scan a directory recursively, indexing files with known source extensions. */
int ca_index_build_dir(ca_index *idx, const char *dir);

/* Search: returns a JSON array of {"term","file","line"} matches (caller frees). */
char *ca_index_search(ca_index *idx, const char *query, int limit);

#ifdef __cplusplus
}
#endif
