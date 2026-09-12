/* attention.c — salience scoring and top-k selection. */
#include "cognitive-os-agent/cognition/attention.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct attention {
    int dummy;
};

attention *attention_new(void) {
    return (attention *)calloc(1, sizeof(attention));
}

void attention_free(attention *a) {
    free(a);
}

/* Case-insensitive substring match. */
static int ci_substr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return 0;
    for (const char *h = hay; *h; h++) {
        if ((size_t)strlen(h) < nlen)
            return 0;
        size_t j = 0;
        for (; j < nlen; j++) {
            int a = (unsigned char)h[j], b = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (j == nlen)
            return 1;
    }

    return 0;
}

/* Iterate query words; for each word present in the candidate text/tags add a
 * weight proportional to word length so rare/specific terms dominate. */
double attention_score(attention *a, const char *query, const attention_candidate *c) {
    double score;
    const char *p = query;

    (void)a;
    if (!c)
        return 0.0;
    score = c->boost;
    if (!query || !c->text)
        return score;

    while (*p) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 2)
            continue; /* skip single letters */

        char word[64];
        if (wlen >= sizeof(word))
            wlen = sizeof(word) - 1;
        memcpy(word, start, wlen);
        word[wlen] = '\0';

        if (ci_substr(c->text, word))
            score += 1.0 + (double)wlen * 0.5;
        else if (c->tags && ci_substr(c->tags, word))
            score += 0.5 + (double)wlen * 0.25;
    }

    return score;
}

/* Insertion sort by score descending (small n; simple and stable enough). */
static void sort_results(attention_result *r, int n) {
    for (int i = 1; i < n; i++) {
        attention_result key = r[i];
        int j = i - 1;
        while (j >= 0 && r[j].score < key.score) {
            r[j + 1] = r[j];
            j--;
        }
        r[j + 1] = key;
    }
}

int attention_select(attention *a, const char *query, const attention_candidate *cands, size_t n,
                         attention_result *out, size_t topk) {
    attention_result *tmp;
    size_t k = topk < n ? topk : n;

    if (!a || !cands || !out || n == 0 || topk == 0)
        return 0;

    tmp = (attention_result *)malloc(n * sizeof(*tmp));
    if (!tmp)
        return 0;
    for (size_t i = 0; i < n; i++) {
        tmp[i].index = (int)i;
        tmp[i].score = attention_score(a, query, &cands[i]);
    }

    sort_results(tmp, (int)n);

    for (size_t i = 0; i < k; i++)
        out[i] = tmp[i];
    free(tmp);
    return (int)k;
}
