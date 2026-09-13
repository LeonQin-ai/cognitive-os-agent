/* index.h — lightweight code/document inverted index.
 * Terms map to (file, line) occurrences. Used by the knowledge system to answer
 * "where is X defined / used". Built by scanning source trees. */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ret_index: "index" alone collides with glibc's index() (strings.h) */
typedef struct ret_index ret_index;

ret_index *index_new(void);
void index_free(ret_index *idx);

/* Index a single file's content (term -> file:line). */
int index_add_file(ret_index *idx, const char *path, const char *content);

/* Search: returns a JSON array of {"term","file","line"} matches (caller frees). */
char *index_search(ret_index *idx, const char *query, int limit);

#ifdef __cplusplus
}
#endif
