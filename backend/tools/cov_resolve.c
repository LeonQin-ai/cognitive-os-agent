/* cov_resolve.c — resolve cov_hits.txt PCs against an exe and report
 * per-source-file function coverage.
 *
 * Usage: cov_resolve <exe-path> <cov_hits.txt>
 *
 * The instrumented binary (built with -finstrument-functions + tools/cov_rt.c)
 * records every distinct function-entry PC in build/cov_hits.txt. This tool
 * loads the exe's CodeView symbols via DbgHelp, enumerates every function
 * (with its source file via line info), then matches each hit PC to a symbol
 * and reports covered/total per source file.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint64_t addr; char name[256]; char file[512]; } sym_t;

#ifndef SymTagFunction
#define SymTagFunction 5
#endif

static sym_t *g_syms = NULL;
static size_t g_n = 0, g_cap = 0;

static int cmp_addr(const void *a, const void *b) {
    const sym_t *x = (const sym_t *)a, *y = (const sym_t *)b;
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

static BOOL CALLBACK enum_cb(SYMBOL_INFO *si, ULONG size, PVOID ctx) {
    (void)size; (void)ctx;
    if (si->Tag != SymTagFunction || !*si->Name) return TRUE;
    if (si->Flags & SYMFLAG_LOCAL) return TRUE;
    IMAGEHLP_LINE64 line;
    memset(&line, 0, sizeof line);
    line.SizeOfStruct = sizeof line;
    DWORD disp = 0;
    const char *file = "?";
    if (SymGetLineFromAddr64(GetCurrentProcess(), si->Address, &disp, &line) && line.FileName)
        file = line.FileName;
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 8192;
        sym_t *ns = (sym_t *)realloc(g_syms, g_cap * sizeof(sym_t));
        if (!ns) return FALSE;
        g_syms = ns;
    }
    sym_t *s = &g_syms[g_n++];
    s->addr = si->Address;
    snprintf(s->name, sizeof s->name, "%s", si->Name);
    snprintf(s->file, sizeof s->file, "%s", file);
    return TRUE;
}

/* rightmost symbol whose addr <= target; -1 if none */
static long find_sym(uint64_t target) {
    long lo = 0, hi = (long)g_n - 1, res = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= target) { res = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return res;
}

static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <exe> <hits>\n", argv[0]); return 2; }
    const char *exe = argv[1], *hits = argv[2];

    if (!SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
        fprintf(stderr, "SymInitialize failed (%lu)\n", GetLastError()); return 1;
    }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

    DWORD64 modBase = SymLoadModule64(GetCurrentProcess(), NULL, (PSTR)exe, NULL, 0, 0);
    if (!modBase) {
        fprintf(stderr, "SymLoadModule64(%s) failed (%lu)\n", exe, GetLastError()); return 1;
    }
    if (!SymEnumSymbols(GetCurrentProcess(), modBase, NULL, enum_cb, NULL)) {
        fprintf(stderr, "SymEnumSymbols failed (%lu)\n", GetLastError()); return 1;
    }
    qsort(g_syms, g_n, sizeof(sym_t), cmp_addr);

    FILE *f = fopen(hits, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", hits); return 1; }
    char line[64];
    uint64_t orig_base = 0;
    if (fgets(line, sizeof line, f)) {
        if (sscanf(line, "BASE %llx", (unsigned long long *)&orig_base) != 1) orig_base = 0;
    }
    uint64_t n_hit = 0;
    while (fgets(line, sizeof line, f)) {
        uint64_t pc = 0;
        if (sscanf(line, "%llx", (unsigned long long *)&pc) != 1) continue;
        uint64_t rva = pc - orig_base;
        uint64_t addr = modBase + rva;
        long i = find_sym(addr);
        if (i >= 0 && g_syms[i].name[0]) { g_syms[i].name[0] = '\0'; n_hit++; }
    }
    fclose(f);

    /* report: unique (file) -> {total, covered} */
    typedef struct { char file[512]; int total, covered; } agg_t;
    const char *root = (argc > 3) ? argv[3] : NULL;   /* optional project root filter */
    agg_t *ag = NULL; size_t na = 0, ca = 0;
    for (size_t i = 0; i < g_n; i++) {
        sym_t *s = &g_syms[i];
        if (root && strncmp(s->file, root, strlen(root)) != 0) continue;
        const char *bf = base_of(s->file);
        size_t j;
        for (j = 0; j < na; j++) if (!strcmp(ag[j].file, bf)) break;
        if (j == na) {
            ag = (agg_t *)realloc(ag, (na + 1) * sizeof(agg_t));
            memset(&ag[na], 0, sizeof(agg_t));
            snprintf(ag[na].file, sizeof ag[na].file, "%s", bf);
            j = na++;
        }
        ag[j].total++;
        if (!s->name[0]) ag[j].covered++;   /* name cleared = hit */
    }
    /* note: coverage should count covered when name was cleared; we cleared on hit */

    unsigned long long tot_f = 0, tot_c = 0;
    printf("%-46s %6s %6s %7s\n", "source file", "funcs", "hit", "cover");
    printf("------------------------------------------------------------------\n");
    for (size_t i = 0; i < na; i++) {
        tot_f += ag[i].total; tot_c += ag[i].covered;
        printf("%-46s %6d %6d %6.1f%%\n", ag[i].file, ag[i].total, ag[i].covered,
               100.0 * ag[i].covered / (ag[i].total ? ag[i].total : 1));
    }
    printf("------------------------------------------------------------------\n");
    printf("%-46s %6llu %6llu %6.1f%%   (distinct functions: %zu, hit PCs: %llu)\n",
           "TOTAL", tot_f, tot_c, 100.0 * tot_c / (tot_f ? tot_f : 1), g_n, n_hit);
    return 0;
}
