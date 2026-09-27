/* cov_rt.c — function-coverage recorder (no sanitizer runtime needed).
 *
 * Uses -finstrument-functions: __cyg_profile_func_enter is called on every
 * function entry. We store distinct function-entry PCs in an atomic hash set
 * and dump them at exit. cov_resolve.c matches them against CodeView symbols.
 *
 * All functions here are marked no_instrument_function to avoid reentrancy.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <windows.h>

#define HSZ (1u << 18)
static atomic_uint_fast64_t htab[HSZ];
static atomic_int recording = 1;
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
    if (!atomic_load_explicit(&recording, memory_order_relaxed)) return;
    uint64_t k = (uint64_t)this_fn;
    uint64_t h = hash64(k) & (HSZ - 1);
    for (uint32_t probes = 0; probes < HSZ; probes++) {
        uint64_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&htab[h], &expected, k,
                memory_order_relaxed, memory_order_relaxed)) return;
        if (expected == k) return;
        h = (h + 1) & (HSZ - 1);
    }
}

__attribute__((no_instrument_function))
static void cov_dump(void) {
    atomic_store_explicit(&recording, 0, memory_order_relaxed);
    FILE *f = fopen("build/cov_hits.txt", "w");
    if (!f) return;
    fprintf(f, "BASE %llx\n", (unsigned long long)g_base);
    for (uint32_t i = 0; i < HSZ; i++) {
        uint64_t pc = atomic_load_explicit(&htab[i], memory_order_relaxed);
        if (pc) fprintf(f, "%llx\n", (unsigned long long)pc);
    }
    fclose(f);
}

__attribute__((constructor, no_instrument_function))
static void cov_install(void) {
    g_base = (uint64_t)GetModuleHandle(NULL);
    atexit(cov_dump);
}
