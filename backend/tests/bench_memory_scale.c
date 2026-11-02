/* Deterministic local-vector memory baseline. No LLM calls or stored secrets.
 * Build with the normal platform sources, then run with 10000 and 100000. */
#include "memory/vector.h"
#include "os/os_time.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static size_t peak_bytes(void) {
    PROCESS_MEMORY_COUNTERS p = {0};
    return GetProcessMemoryInfo(GetCurrentProcess(), &p, sizeof p) ? (size_t)p.PeakWorkingSetSize : 0;
}
#else
#include <sys/resource.h>
static size_t peak_bytes(void) {
    struct rusage r;
    return getrusage(RUSAGE_SELF, &r) == 0 ? (size_t)r.ru_maxrss *
#ifdef __APPLE__
        1
#else
        1024
#endif
        : 0;
}
#endif

static int cmp_us(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 10000;
    if (count < 1 || count > 100000) return 2;
    vectorstore *v = vectorstore_new();
    if (!v) return 2;
    int64_t start = time_now_us();
    char id[48], text[160];
    for (int i = 0; i < count; ++i) {
        snprintf(id, sizeof id, "record-%06d", i);
        snprintf(text, sizeof text, "Project %06d uses storage engine %s and worker pool %d", i,
                 i == count - 1 ? "SQLite" : "RocksDB", i % 32);
        if (vectorstore_add(v, id, text, "benchmark")) return 3;
    }
    int64_t build_us = time_now_us() - start;
    long long durations[21];
    int hits = 0;
    for (int i = 0; i < 21; ++i) {
        int64_t t = time_now_us();
        char *json = vectorstore_nearest(v, "Project storage engine SQLite", 5);
        durations[i] = time_now_us() - t;
        if (!json) return 4;
        cJSON *arr = cJSON_Parse(json);
        if (!arr) return 5;
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            const char *found = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
            snprintf(id, sizeof id, "record-%06d", count - 1);
            if (found && !strcmp(found, id)) hits++;
        }
        cJSON_Delete(arr);
        free(json);
    }
    qsort(durations, 21, sizeof durations[0], cmp_us);
    char *exact = vectorstore_nearest(v, text, 5);
    if (!exact) return 5;
    cJSON *exact_arr = cJSON_Parse(exact);
    if (!exact_arr) return 5;
    int exact_hit = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, exact_arr) {
        const char *found = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "id"));
        snprintf(id, sizeof id, "record-%06d", count - 1);
        if (found && !strcmp(found, id)) exact_hit = 1;
    }
    cJSON_Delete(exact_arr);free(exact);
    printf("{\"records\":%d,\"build_ms\":%.3f,\"retrieval_p50_ms\":%.3f,\"retrieval_p95_ms\":%.3f,\"peak_rss_bytes\":%zu,\"ambiguous_target_in_top5\":%s,\"exact_target_in_top5\":%s}\n",
           count, build_us / 1000.0, durations[10] / 1000.0, durations[19] / 1000.0,
           peak_bytes(), hits == 21 ? "true" : "false", exact_hit ? "true" : "false");
    vectorstore_free(v);
    return 0;
}
