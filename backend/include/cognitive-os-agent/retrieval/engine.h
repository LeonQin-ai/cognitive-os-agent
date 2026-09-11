/* index.h — lightweight code/document inverted index.
 * Terms map to (file, line) occurrences. Used by the knowledge system to answer
 * "where is X defined / used". Built by scanning source trees. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct index index;

index *index_new(void);
void index_free(index *idx);

/* Index a single file's content (term -> file:line). */
int index_add_file(index *idx, const char *path, const char *content);

/* Search: returns a JSON array of {"term","file","line"} matches (caller frees). */
char *index_search(index *idx, const char *query, int limit);

#ifdef __cplusplus
}
#endif
