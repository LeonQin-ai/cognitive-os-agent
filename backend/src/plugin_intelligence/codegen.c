/* codegen.c — plugin code generation. */
#include "cagent/plugin_intelligence/codegen.h"
#include "cagent/infra/util.h"

#include <stdlib.h>
#include <string.h>

static const char *ident(const char *name, char *buf, size_t n) {
    size_t j = 0;
    const char *p = name ? name : "plugin";
    for (; *p && j + 1 < n; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            buf[j++] = c;
        else if (c == ' ' || c == '-' || c == '_')
            buf[j++] = '_';
    }
    if (j == 0) buf[j++] = '_';
    buf[j] = '\0';
    return buf;
}

char *ca_codegen_plugin(const char *name, const char *description) {
    char id[128];
    ident(name, id, sizeof(id));

    ca_strbuf sb;
    ca_strbuf_init(&sb);
    ca_strbuf_appendf(&sb,
        "/* %s.c — generated c-agent plugin: %s */\n"
        "#include <string.h>\n\n"
        "/* Plugin entry point. Returns 0 ok, -1 error. */\n"
        "int %s_run(const char *args_json, char **out) {\n"
        "    (void)args_json;\n"
        "    *out = (char *)\"ok\";\n"
        "    return 0;\n"
        "}\n\n"
        "/* Capabilities this plugin requires. */\n"
        "static const char *%s_capabilities[] = { NULL };\n\n"
        "/* Metadata. */\n"
        "const char *%s_name(void) { return \"%s\"; }\n"
        "const char *%s_description(void) { return \"%s\"; }\n",
        id, description ? description : "", id, id, id, id, id,
        description ? description : "");

    return ca_strbuf_detach(&sb);
}
