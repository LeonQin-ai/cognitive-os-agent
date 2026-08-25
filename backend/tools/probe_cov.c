#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

__attribute__((noinline)) static void probe_fn(void) {
    volatile int x = 0;
    x++;
    printf("x=%d\n", x);
}

static void resolve(uint64_t pc, const char *label) {
    char storage[sizeof(IMAGEHLP_SYMBOL64) + 512];
    IMAGEHLP_SYMBOL64 *sym = (IMAGEHLP_SYMBOL64 *)storage;
    memset(sym, 0, sizeof storage);
    sym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
    sym->MaxNameLength = 511;
    DWORD64 disp = 0;
    BOOL ok = SymGetSymFromAddr64(GetCurrentProcess(), pc, &disp, sym);
    IMAGEHLP_LINE64 line;
    memset(&line, 0, sizeof line);
    line.SizeOfStruct = sizeof line;
    DWORD ld = 0;
    BOOL lok = SymGetLineFromAddr64(GetCurrentProcess(), pc, &ld, &line);
    if (ok) {
        printf("[%s] sym=%s file=%s line=%lu (lok=%d)\n", label, sym->Name,
               lok ? line.FileName : "?", lok ? line.LineNumber : 0);
    } else {
        printf("[%s] sym=FAIL err=%lu\n", label, GetLastError());
    }
}

int main(void) {
    if (!SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
        printf("SymInitialize failed\n");
        return 1;
    }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEBUG);
    resolve((uint64_t)(void *)main, "main");
    resolve((uint64_t)(void *)probe_fn, "probe_fn");
    return 0;
}
