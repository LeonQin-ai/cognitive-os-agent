/* cov_rt.c — function-coverage recorder (no sanitizer runtime needed).
 *
 * Uses -finstrument-functions: __cyg_profile_func_enter is called on every
 * function entry. We store distinct function-entry PCs in a static hash set
 * (no malloc, no libc calls) and dump them at exit. A Python script then
 * matches the PCs against the linker map (build/cov.map) to compute
 * per-source-file function coverage.
 *
 * All functions here are marked no_instrument_function to avoid reentrancy.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define HSZ (1u << 18)
static uint64_t htab[HSZ];
static uint64_t distinct = 0;
static int recording = 1;
static uint64_t g_base = 0;

__attribute__((no_instrument_function))
static uint64_t hash64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)this_fn; (void)call_site;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)call_site;
    if (!recording) return;
    uint64_t k = (uint64_t)this_fn;
    uint64_t h = hash64(k) & (HSZ - 1);
    while (htab[h]) {
        if (htab[h] == k) return;
        h = (h + 1) & (HSZ - 1);
    }
    htab[h] = k;
    distinct++;
}

__attribute__((no_instrument_function))
static void cov_dump(void) {
    recording = 0;
    FILE *f = fopen("build/cov_hits.txt", "w");
    if (!f) return;
    fprintf(f, "BASE %llx\n", (unsigned long long)g_base);
    for (uint32_t i = 0; i < HSZ; i++) {
        if (htab[i]) fprintf(f, "%llx\n", (unsigned long long)htab[i]);
    }
    fclose(f);
}

__attribute__((constructor, no_instrument_function))
static void cov_install(void) {
    g_base = (uint64_t)GetModuleHandle(NULL);
    atexit(cov_dump);
}
