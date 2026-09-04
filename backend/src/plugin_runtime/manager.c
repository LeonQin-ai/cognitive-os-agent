#include "cagent/plugin_runtime/manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct ca_plugin { HMODULE h; };

static char g_err[512] = "";

ca_plugin *ca_plugin_load(const char *path) {
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        snprintf(g_err, sizeof(g_err), "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
        return NULL;
    }
    ca_plugin *p = malloc(sizeof(ca_plugin));
    if (!p) { FreeLibrary(h); return NULL; }
    p->h = h;
    g_err[0] = '\0';
    return p;
}

void *ca_plugin_symbol(ca_plugin *p, const char *name) {
    return (void *)(uintptr_t)GetProcAddress(p->h, name);
}

const char *ca_plugin_error(void) { return g_err; }

void ca_plugin_unload(ca_plugin *p) {
    if (!p) return;
    FreeLibrary(p->h);
    free(p);
}

#else

#include <dlfcn.h>

struct ca_plugin { void *h; };

static char g_err[512] = "";

ca_plugin *ca_plugin_load(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(g_err, sizeof(g_err), "%s", dlerror() ? dlerror() : "dlopen failed");
        return NULL;
    }
    ca_plugin *p = malloc(sizeof(ca_plugin));
    if (!p) { dlclose(h); return NULL; }
    p->h = h;
    g_err[0] = '\0';
    return p;
}

void *ca_plugin_symbol(ca_plugin *p, const char *name) {
    return dlsym(p->h, name);
}

const char *ca_plugin_error(void) { return g_err; }

void ca_plugin_unload(ca_plugin *p) {
    if (!p) return;
    dlclose(p->h);
    free(p);
}

#endif
