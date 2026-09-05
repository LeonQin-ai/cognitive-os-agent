#include "cognitive-os-agent/plugin_runtime/manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct coa_plugin { HMODULE h; };

static char g_err[512] = "";

coa_plugin *coa_plugin_load(const char *path) {
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        snprintf(g_err, sizeof(g_err), "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
        return NULL;
    }
    coa_plugin *p = malloc(sizeof(coa_plugin));
    if (!p) { FreeLibrary(h); return NULL; }
    p->h = h;
    g_err[0] = '\0';
    return p;
}

void *coa_plugin_symbol(coa_plugin *p, const char *name) {
    return (void *)(uintptr_t)GetProcAddress(p->h, name);
}

const char *coa_plugin_error(void) { return g_err; }

void coa_plugin_unload(coa_plugin *p) {
    if (!p) return;
    FreeLibrary(p->h);
    free(p);
}

#else

#include <dlfcn.h>

struct coa_plugin { void *h; };

static char g_err[512] = "";

coa_plugin *coa_plugin_load(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(g_err, sizeof(g_err), "%s", dlerror() ? dlerror() : "dlopen failed");
        return NULL;
    }
    coa_plugin *p = malloc(sizeof(coa_plugin));
    if (!p) { dlclose(h); return NULL; }
    p->h = h;
    g_err[0] = '\0';
    return p;
}

void *coa_plugin_symbol(coa_plugin *p, const char *name) {
    return dlsym(p->h, name);
}

const char *coa_plugin_error(void) { return g_err; }

void coa_plugin_unload(coa_plugin *p) {
    if (!p) return;
    dlclose(p->h);
    free(p);
}

#endif
