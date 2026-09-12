#include "cognitive-os-agent/plugin_runtime/manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct plugin {
    HMODULE h;
};

static char g_err[512] = "";

plugin *plugin_load(const char *path) {
    plugin *p;

    HMODULE h = LoadLibraryA(path);
    if (!h) {
        snprintf(g_err, sizeof(g_err), "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
        return NULL;
    }

    p = malloc(sizeof(plugin));
    if (!p) {
        FreeLibrary(h);
        return NULL;
    }

    p->h = h;
    g_err[0] = '\0';
    return p;
}

void *plugin_symbol(plugin *p, const char *name) {
    return (void *)(uintptr_t)GetProcAddress(p->h, name);
}

const char *plugin_error(void) {
    return g_err;
}

void plugin_unload(plugin *p) {
    if (!p)
        return;
    FreeLibrary(p->h);
    free(p);
}

#else

#include <dlfcn.h>

struct plugin {
    void *h;
};

static char g_err[512] = "";

plugin *plugin_load(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    plugin *p;

    if (!h) {
        snprintf(g_err, sizeof(g_err), "%s", dlerror() ? dlerror() : "dlopen failed");
        return NULL;
    }

    p = malloc(sizeof(plugin));
    if (!p) {
        dlclose(h);
        return NULL;
    }

    p->h = h;
    g_err[0] = '\0';
    return p;
}

void *plugin_symbol(plugin *p, const char *name) {
    return dlsym(p->h, name);
}

const char *plugin_error(void) {
    return g_err;
}

void plugin_unload(plugin *p) {
    if (!p)
        return;
    dlclose(p->h);
    free(p);
}

#endif
