/* attention.c — salience scoring and top-k selection. */
#include "cagent/cognition/attention.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct ca_attention { int dummy; };

ca_attention *ca_attention_new(void) {
    return (ca_attention *)calloc(1, sizeof(ca_attention));
}

void ca_attention_free(ca_attention *a) { free(a); }

/* Case-insensitive substring match. */
static int ci_substr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return 0;
    for (const char *h = hay; *h; h++) {
        if ((size_t)strlen(h) < nlen) return 0;
        size_t j = 0;
        for (; j < nlen; j++) {
            int a = (unsigned char)h[j], b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Iterate query words; for each word present in the candidate text/tags add a
 * weight proportional to word length so rare/specific terms dominate. */
double ca_attention_score(ca_attention *a, const char *query,
                          const ca_attention_candidate *c) {
    (void)a;
    if (!c) return 0.0;
    double score = c->boost;
    if (!query || !c->text) return score;

    const char *p = query;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 2) continue; /* skip single letters */

        char word[64];
        if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
        memcpy(word, start, wlen);
        word[wlen] = '\0';

        if (ci_substr(c->text, word)) score += 1.0 + (double)wlen * 0.5;
        else if (c->tags && ci_substr(c->tags, word)) score += 0.5 + (double)wlen * 0.25;
    }
    return score;
}

/* Insertion sort by score descending (small n; simple and stable enough). */
static void sort_results(ca_attention_result *r, int n) {
    for (int i = 1; i < n; i++) {
        ca_attention_result key = r[i];
        int j = i - 1;
        while (j >= 0 && r[j].score < key.score) {
            r[j + 1] = r[j];
            j--;
        }
        r[j + 1] = key;
    }
}

int ca_attention_select(ca_attention *a, const char *query,
                        const ca_attention_candidate *cands, size_t n,
                        ca_attention_result *out, size_t topk) {
    if (!a || !cands || !out || n == 0 || topk == 0) return 0;

    ca_attention_result *tmp = (ca_attention_result *)malloc(n * sizeof(*tmp));
    if (!tmp) return 0;
    for (size_t i = 0; i < n; i++) {
        tmp[i].index = (int)i;
        tmp[i].score = ca_attention_score(a, query, &cands[i]);
    }
    sort_results(tmp, (int)n);

    size_t k = topk < n ? topk : n;
    for (size_t i = 0; i < k; i++) out[i] = tmp[i];
    free(tmp);
    return (int)k;
}
