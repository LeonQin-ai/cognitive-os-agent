/* index.h — lightweight code/document inverted index.
 * Terms map to (file, line) occurrences. Used by the knowledge system to answer
 * "where is X defined / used". Built by scanning source trees. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coa_index coa_index;

coa_index *coa_index_new(void);
void coa_index_free(coa_index *idx);

/* Index a single file's content (term -> file:line). */
int coa_index_add_file(coa_index *idx, const char *path, const char *content);

/* Scan a directory recursively, indexing files with known source extensions. */
int coa_index_build_dir(coa_index *idx, const char *dir);

/* Search: returns a JSON array of {"term","file","line"} matches (caller frees). */
char *coa_index_search(coa_index *idx, const char *query, int limit);

#ifdef __cplusplus
}
#endif
