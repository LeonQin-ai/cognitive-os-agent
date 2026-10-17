/* planner.c — LLM plan generation. */
#include "cognition/planner.h"
#include "llm/llm.h"
#include "action/tools.h"
#include "action/skill.h"
#include "runtime/policy_engine.h"
#include "infra/util.h"
#include "infra/logging.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "cJSON.h"

static const char *SYS_PROMPT =
    "You are the planner of a cognitive OS. Decide what tool actions to take "
    "for the user request. Respond with ONLY a JSON array of actions of the "
    "form [{\"tool\":\"...\",\"args\":{...}}].\n"
    "Available tools and their EXACT argument names:\n"
    "- file_read:  args {\"path\": string, \"offset_bytes\": optional integer} # bounded file pages; directories return listings\n"
    "- file_write: args {\"path\": string, \"content\": string}  # create or overwrite a FILE\n"
    "- shell:      args {\"command\": string, \"timeout_ms\": integer}\n"
    "- git:        args {\"args\": string (subcommand+flags, e.g. \"log -5\"), \"dir\": string}\n"
    "- mcp:        args {\"server_url\": string, \"tool\": string, \"args\": object}\n"
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "IMPORTANT: For greetings, conversation, questions, or requests that do NOT "
    "require file/shell/git/MCP operations, respond with PLAIN TEXT (not JSON). "
    "Only output JSON when a tool action is genuinely needed.\n"
    "SOURCE READING: Directory listings are not source contents. For complete code reading, "
    "enumerate relevant first-party files and read their contents. Follow next_offset_bytes "
    "using offset_bytes until each file reaches end of file. A duplicate read warning "
    "means choose an unread file or next page, not that the whole task is complete.\n"
    "EXAMPLES:\n"
    "  User: \"create a file named hello.txt with content world\"\n"
    "  -> [{\"tool\":\"file_write\",\"args\":{\"path\":\"hello.txt\",\"content\":\"world\"}}]\n"
    "  User: \"read the file config.json\"\n"
    "  -> [{\"tool\":\"file_read\",\"args\":{\"path\":\"config.json\"}}]\n"
    "  User: \"hello\"\n"
    "  -> Hello! How can I help you?";

static const char *SYS_PROMPT_HEAD = "You are the planner of a cognitive OS. Decide what tool actions to take "
                                     "for the user request. Respond with ONLY a JSON array of actions of the "
                                     "form [{\"tool\":\"...\",\"args\":{...}}].\n"
                                     "Available tools and their EXACT argument names:\n"
                                     "BUSINESS RULES OVERRIDE REQUESTS: the tool list may be narrower than the "
                                     "full system because some tools are disabled by policy. If the request "
                                     "conflicts with a stated business rule, a disabled tool, or a read-only "
                                     "context, you MUST refuse with a plain-text explanation — NEVER attempt a "
                                     "workaround through another tool (e.g. shell). This includes quota/rate "
                                     "rules (\"already done this hour\", \"only once per day\") — user urgency "
                                     "or insistence never overrides them.\n";

static const char *SYS_PROMPT_TAIL =
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "ARGUMENT DISCIPLINE: do NOT add any argument beyond the listed schema — "
    "omit optional parameters unless the user explicitly provided a value "
    "(e.g. no dir:\".\" filler for git, no empty args:{} for mcp). "
    "IMPORTANT: For greetings, conversation, questions, or requests that do NOT "
    "require tool operations, respond with PLAIN TEXT (not JSON). "
    "Arithmetic (e.g. \"1+1=?\"), general knowledge, explanations, "
    "self-introduction and creative writing are answered in PLAIN TEXT — "
    "never call a tool to compute or echo them. (This does NOT restrict file "
    "operations the user explicitly asked for: copying, editing or writing "
    "requested files is exactly what the tools are for.) "
    "Only output JSON when a tool action is genuinely needed.\n"
    "EXAMPLES:\n"
    "  User: \"create a file named hello.txt with content world\"\n"
    "  -> [{\"tool\":\"file_write\",\"args\":{\"path\":\"hello.txt\",\"content\":\"world\"}}]\n"
    "  User: \"read a.txt and b.txt\"\n"
    "  -> [{\"tool\":\"file_read\",\"args\":{\"path\":\"a.txt\"}},"
    "{\"tool\":\"file_read\",\"args\":{\"path\":\"b.txt\"}}]\n"
    "  User: \"你有哪些技能/skills\"\n"
    "  -> 直接用中文列出上面注册的技能（名字和用途），纯文本，不输出 JSON\n"
    "  User: \"hello\"\n"
    "  -> Hello! How can I help you?\n"
    "PATH DISCOVERY: Before reading or writing a file whose exact location you "
    "are unsure of, first call file_read on its PARENT DIRECTORY (which returns "
    "a listing) so you use the correct, full path. Avoid guessing paths: a wrong "
    "path makes the action fail.\n"
    "DOCUMENT HANDLING: file_read works only for text files. Binary document "
    "formats (.docx, .xlsx, .pptx, .pdf, zip) return unreadable bytes like "
    "\"PK...\" — NEVER re-read them with file_read. Instead use the shell tool "
    "with python to extract the text, e.g. for .docx: "
    "python -c \"import docx;d=docx.Document('f.docx');print('\\n'.join(p.text for p in d.paragraphs))\" "
    "(xlsx: openpyxl, pdf: pypdf), or unzip the file and read its XML. Compute "
    "answers that need arithmetic or filtering over file contents with a python "
    "script via shell — do not count long tables in your head.";

/* Build the system prompt from the ACTUAL tool + skill registries. Returns a
 * malloc'd prompt, or NULL when no registry is available (caller falls back
 * to the static catalog). Tools denied by `policy` are hidden entirely
 * (Claude Code: deny rules both block calls AND remove from the tool pool). */
static char *build_catalog_prompt(const tool_registry *tools, struct skill_registry *skills,
                                  struct policy_engine *policy) {
    strbuf b;
    int have_skill_tool = 0;
    int have_glob_tool = 0;

    if (!tools)
        return NULL;
    strbuf_init(&b);
    strbuf_append(&b, SYS_PROMPT_HEAD);
    for (size_t i = 0; i < (size_t)tool_registry_count(tools); i++) {
        const tool *t = tool_registry_get(tools, i);
        if (!t || !t->name)
            continue;
        if (policy && policy_check(policy, t->name, "{}", NULL) == POLICY_DENY)
            continue;
        if (strcmp(t->name, "glob") == 0) have_glob_tool = 1;
        if (strcmp(t->name, "skill") == 0) {
            have_skill_tool = 1;
            strbuf_append(&b,
                              "- skill:      args {\"name\": string}  # run a REGISTERED SKILL by name (list below)\n");
        } else {
            strbuf_appendf(&b, "- %s: %s\n", t->name, t->description ? t->description : "");
            if (t->json_schema && *t->json_schema) {
                strbuf_append(&b, "  args schema: ");
                strbuf_append(&b, t->json_schema);
                strbuf_append(&b, "\n");
            }
        }
    }
    if (have_glob_tool)
        strbuf_append(&b, "Example file discovery: "
                          "[{\"tool\":\"glob\",\"args\":{\"pattern\":\"**/*flow*\"}}]. "
                          "Every glob action needs a non-empty pattern; fix schema errors before retrying.\n");
    strbuf_append(&b, "File reading: retain absolute paths from glob/list_dir; do not drop a nested search root. "
                      "Use max_bytes at most 4000 and copy next_offset_bytes for each following page. "
                      "On a missing path discover its real location instead of retrying it unchanged.\n");

    strbuf_append(&b, "Browser extraction: a navigation/accessibility snapshot may omit article text. "
        "When supported by the installed browser MCP, evaluate document.body.innerText (and relevant frames) "
        "after the requested content loads. Follow actual site pagination/scroll loading when needed. "
        "If a tool returns [full tool result saved], read its local file in 4000-byte pages to EOF; "
        "the preview is not the complete result. Do not repeatedly navigate to the same page for missing text.\n");
    if (skills) skill_registry_refresh(skills);
    if (have_skill_tool && skills && skill_count(skills) > 0) {
        strbuf_append(&b, "Registered skills (capabilities you can RUN via the skill tool):\n");
        for (int i = 0; i < skill_count(skills); i++) {
            const skill *s = skill_get(skills, (size_t)i);
            if (!s)
                continue;
            strbuf_appendf(&b, "  * %s (%s): %s\n", s->name, s->kind ? s->kind : "",
                               s->description ? s->description : "");
        }
        strbuf_append(&b, "When the user ASKS what skills/tools you have (e.g. \"你有哪些skills\", "
                              "\"list your skills\"), answer in PLAIN TEXT listing these skill names and "
                              "their use. Do NOT call any tool or skill to answer such a question.\n");
    }

    strbuf_append(&b, SYS_PROMPT_TAIL);
    return strbuf_detach(&b);
}

/* Extract the first balanced JSON array/object span from a reply, honoring
 * string literals and escapes. The naive "first '[' .. last ']'" heuristic
 * breaks when the payload itself contains ']' (e.g. file_write content with
 * python list literals), slicing out invalid JSON and silently demoting a
 * real tool plan to a plain-text answer. Returns malloc'd span or NULL. */
static char *extract_json_span(const char *plan) {
    const char *start = NULL;
    for (const char *p = plan; *p; p++) {
        if (*p == '[' || *p == '{') {
            start = p;
            break;
        }
    }

    if (!start)
        return NULL;
    int depth = 0, in_str = 0, esc = 0;
    for (const char *p = start; *p; p++) {
        if (in_str) {
            if (esc)
                esc = 0;
            else if (*p == '\\')
                esc = 1;
            else if (*p == '"')
                in_str = 0;
        } else if (*p == '"') {
            in_str = 1;
        } else if (*p == '[' || *p == '{') {
            depth++;
        } else if (*p == ']' || *p == '}') {
            if (--depth == 0) {
                size_t n = (size_t)(p - start) + 1;
                char *slice = (char *)malloc(n + 1);
                if (slice) {
                    memcpy(slice, start, n);
                    slice[n] = '\0';
                }
                return slice;
            }
        }
    }

    return NULL; /* unbalanced (likely truncated reply) */
}

/* Try to parse a reply into tool actions. Returns 1 when actions were built
 * (possibly zero if the array was empty), 0 when the reply is prose without
 * any tool plan. */
/* Normalize an args value to a JSON-object string. Accepts an object as-is
 * and a string that itself contains JSON (OpenAI wire format). */
static char *args_to_json(cJSON *args) {
    if (!args)
        return xstrdup("{}");
    if (cJSON_IsObject(args))
        return cJSON_PrintUnformatted(args);
    if (cJSON_IsString(args) && args->valuestring) {
        cJSON *p = cJSON_Parse(args->valuestring);
        if (p) {
            char *s = cJSON_IsObject(p) ? cJSON_PrintUnformatted(p) : NULL;
            cJSON_Delete(p);
            if (s)
                return s;
        }
    }

    return xstrdup("{}");
}

/* Salvage a plan array that was truncated mid-output by the LLM's output
 * token limit (real-world case: a 10-file file_write batch cut at ~29KB).
 * Scans the text tracking string/escape state and bracket depth, finds the
 * closing brace of the LAST COMPLETE top-level element, cuts there and
 * closes the array. Re-asking the model to re-emit the same oversized plan
 * truncates again, so the complete leading actions must be kept locally.
 * Returns a malloc'd valid JSON array string, or NULL when no complete
 * element exists. */
static char *salvage_truncated_array(const char *plan) {
    const char *start, *end = NULL, *p;
    int depth = 0, in_str = 0, esc = 0;
    char *out;
    size_t len;

    start = strstr(plan, "[{");
    if (!start)
        start = strchr(plan, '[');
    if (!start)
        return NULL;
    for (p = start; *p; p++) {
        char c = *p;
        if (in_str) {
            if (esc)
                esc = 0;
            else if (c == '\\')
                esc = 1;
            else if (c == '"')
                in_str = 0;
            continue;
        }
        if (c == '"') {
            in_str = 1;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
            if (depth == 1 && c == '}')
                end = p; /* closing brace of a complete top-level element */
        }
    }
    if (!end)
        return NULL;
    len = (size_t)(end - start) + 1; /* include the closing brace, drop any trailing comma */
    out = (char *)malloc(len + 2);
    if (!out)
        return NULL;
    memcpy(out, start, len);
    out[len] = ']';
    out[len + 1] = '\0';
    return out;
}

static int parse_plan_actions(const char *plan, planned_action **actions, int *n_actions) {
    cJSON *root;
    planned_action *a = NULL;
    int n = 0;
    cJSON *it;

    *actions = NULL;
    *n_actions = 0;
    root = cJSON_Parse(plan);
    if (!root) {
        char *slice = extract_json_span(plan);
        if (slice) {
            root = cJSON_Parse(slice);
            free(slice);
        }
    }

    if (!root)
        return 0;
    /* also accept a single tool object (not wrapped in an array) */
    if (cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "tool")) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToArray(arr, root);
        root = arr;
    }

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON_ArrayForEach(it, root) {
    cJSON *tool;
    cJSON *args;
    char *args_json;
    planned_action *na;

        if (!cJSON_IsObject(it))
            continue;
        tool = cJSON_GetObjectItemCaseSensitive(it, "tool");
        args = cJSON_GetObjectItemCaseSensitive(it, "args");
        if (!tool || !cJSON_IsString(tool)) {
            /* models occasionally drift into the OpenAI/Claude tool-call
             * envelope; a plan parsed to zero actions used to be treated as
             * the final answer, silently dropping real edits (SWE-bench
             * f2p=0 failures). Accept:
             *   {"type":"function","name":"<tool>","arguments":{...}}
             *   {"function":{"name":"<tool>","arguments":"{...}"}}  */
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(it, "function");
            if (!fn && cJSON_GetObjectItemCaseSensitive(it, "name") &&
                cJSON_GetObjectItemCaseSensitive(it, "arguments"))
                fn = it; /* flat envelope */
            if (fn) {
                tool = cJSON_GetObjectItemCaseSensitive(fn, "name");
                args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
            } else if (tool && cJSON_IsString(tool) && !args) {
                args = cJSON_GetObjectItemCaseSensitive(it, "arguments");
            }
        }

        if (!tool || !cJSON_IsString(tool))
            continue;
        args_json = args_to_json(args);
        na = (planned_action *)realloc(a, (size_t)(n + 1) * sizeof(planned_action));
        if (!na) {
            free(args_json);
            break;
        }

        a = na;
        a[n].tool = xstrdup(tool->valuestring);
        a[n].args_json = args_json;
        n++;
    }

    cJSON_Delete(root);
    *actions = a;
    *n_actions = n;
    return 1;
}

/* Some OpenAI-compatible providers serialize tool calls as
 *   <tool_call>shell{"command":"..."}<tool_call>file_read{"path":"..."}
 * instead of putting the tool name inside the JSON object. Parse every such
 * block locally; otherwise the reply is mistaken for prose and the model can
 * repeat it forever without executing anything. */
static int parse_tagged_tool_calls(const char *plan, planned_action **actions, int *n_actions) {
    const char *p = plan;
    planned_action *out = NULL;
    int count = 0;

    while ((p = strstr(p, "<tool_call>")) != NULL) {
        const char *name = p + strlen("<tool_call>");
        const char *json = name;
        while (*json && *json != '{' && *json != '[' && *json != '<')
            json++;
        const char *name_end = json;
        while (name_end > name && (name_end[-1] == ' ' || name_end[-1] == '\t' || name_end[-1] == '\r' || name_end[-1] == '\n'))
            name_end--;
        if (json == name || (*json != '{' && *json != '[')) {
            p = name;
            continue;
        }

        char *body = extract_json_span(json);
        if (!body)
            break; /* incomplete trailing call: let the normal repair path handle it */
        cJSON *args = cJSON_Parse(body);
        free(body);
        if (!args || !cJSON_IsObject(args)) {
            cJSON_Delete(args);
            p = json + 1;
            continue;
        }

        planned_action *grown = realloc(out, (size_t)(count + 1) * sizeof(*out));
        if (!grown) {
            cJSON_Delete(args);
            break;
        }
        out = grown;
        size_t tool_len = (size_t)(name_end - name);
        out[count].tool = (char *)malloc(tool_len + 1);
        if (out[count].tool) {
            memcpy(out[count].tool, name, tool_len);
            out[count].tool[tool_len] = '\0';
        }
        out[count].args_json = cJSON_PrintUnformatted(args);
        cJSON_Delete(args);
        if (!out[count].tool || !out[count].args_json) {
            free(out[count].tool);
            free(out[count].args_json);
            break;
        }
        count++;

        /* Advance past the balanced JSON object, including nested braces and
         * braces inside strings, by locating the next tag after this object. */
        const char *next = strstr(json + 1, "<tool_call>");
        p = next ? next : json + strlen(json);
    }

    if (count == 0) {
        free(out);
        return 0;
    }
    *actions = out;
    *n_actions = count;
    return 1;
}

/* DSML is sometimes returned in content by compatible gateways instead of
 * native tool_calls. Parse only a complete top-level envelope, never examples
 * embedded in prose/JSON. Payload bytes are not normalized or XML-unescaped. */
typedef struct dsml_tag {
    char kind[32], name[256];
    int closing, string_value;
} dsml_tag;

static const char *dsml_space(const char *p) {
    while (isspace((unsigned char)*p)) p++;
    return p;
}

static int dsml_tag_read(const char **cursor, dsml_tag *tag) {
    const char *p=dsml_space(*cursor), *start;
    memset(tag,0,sizeof(*tag)); tag->string_value=-1;
    if (*p=='\\' && p[1]=='<') p++; /* escaped closing tags in gateway content */
    if (*p++!='<') return 0;
    if (*p=='/') { tag->closing=1; p++; }
    const char *prefixes[]={"｜｜DSML｜｜","｜DSML｜","||DSML||","|DSML|"};
    int matched=0;
    for (int i=0;i<4;i++) if (strncmp(p,prefixes[i],strlen(prefixes[i]))==0) {
        p+=strlen(prefixes[i]); matched=1; break;
    }
    if (!matched) return 0;
    p=dsml_space(p); start=p;
    while (isalnum((unsigned char)*p) || *p=='_') p++;
    size_t n=(size_t)(p-start);
    if (!n || n>=sizeof(tag->kind)) return 0;
    memcpy(tag->kind,start,n);
    while (*(p=dsml_space(p))!='>') {
        if (!*p || tag->closing) return 0;
        start=p;
        while (isalpha((unsigned char)*p) || *p=='_') p++;
        size_t keylen=(size_t)(p-start);
        p=dsml_space(p);
        if (*p++!='=') return 0;
        p=dsml_space(p); char quote=*p++;
        if (quote!='\"' && quote!='\'') return 0;
        const char *value=p, *end=strchr(p,quote);
        if (!end) return 0;
        n=(size_t)(end-value);
        if (keylen==4 && strncmp(start,"name",4)==0) {
            if (!n || n>=sizeof(tag->name) || tag->name[0]) return 0;
            memcpy(tag->name,value,n);
        } else if (keylen==6 && strncmp(start,"string",6)==0) {
            if (tag->string_value!=-1) return 0;
            if (n==4 && strncmp(value,"true",4)==0) tag->string_value=1;
            else if (n==5 && strncmp(value,"false",5)==0) tag->string_value=0;
            else return 0;
        } else return 0;
        p=end+1;
    }
    *cursor=p+1;
    return 1;
}

/* 0 = another format, 1 = valid DSML, -1 = broken DSML (no partial execution). */
static int parse_dsml_tool_calls(const char *plan, planned_action **actions, int *n_actions) {
    const char *p=dsml_space(plan);
    /* Some compatible models explain the repair before emitting the actual
     * calls. Accept one complete DSML envelope after plain-text preamble, but
     * never skip over another markup block: that could turn a quoted example
     * into an executable plan. The envelope parser below still requires the
     * entire remainder to be valid and consumes every invoke atomically. */
    if (strncmp(p,"<｜",4) && strncmp(p,"<|",2) && strncmp(p,"<||",3)) {
        const char *marker=p, *open=NULL;
        while ((marker=strstr(marker,"DSML"))!=NULL) {
            const char *tag_name=marker;
            while (tag_name>p && tag_name[-1]!='<') tag_name--;
            if (tag_name>p && tag_name[-1]=='<') { open=tag_name-1; break; }
            marker+=4;
        }
        if (!open) return 0;
        /* Do not execute an envelope shown as an example or embedded in a
         * JSON/code reply. A real preamble is plain explanatory text. */
        for (const char *q=p;q<open;q++)
            if (*q=='<' || *q=='{' || *q=='}' || *q=='[' || *q==']' ||
                *q=='"' || *q=='\'' || *q=='`') return 0;
        size_t preamble_len=(size_t)(open-p);
        char *preamble=malloc(preamble_len+1);
        if (!preamble) return -1;
        memcpy(preamble,p,preamble_len); preamble[preamble_len]=0;
        int example = strstr(preamble,"Example") || strstr(preamble,"example") ||
                      strstr(preamble,"For example") || strstr(preamble,"for example") ||
                      strstr(preamble,"示例") || strstr(preamble,"例如") || strstr(preamble,"比如");
        free(preamble);
        if (example) return 0;
        p=open;
    }
    dsml_tag tag;
    if (!strstr(p,"DSML")) return 0;
    if (!dsml_tag_read(&p,&tag)) return -1;
    if (tag.closing || (strcmp(tag.kind,"calls") && strcmp(tag.kind,"function_calls"))) return -1;
    char outer[32]; snprintf(outer,sizeof(outer),"%s",tag.kind);
    planned_action *out=NULL; int count=0; cJSON *args=NULL, *value=NULL;
    for (;;) {
        if (!dsml_tag_read(&p,&tag)) goto invalid;
        if (tag.closing && !strcmp(tag.kind,outer)) break;
        if (tag.closing || strcmp(tag.kind,"invoke") || !tag.name[0]) goto invalid;
        char tool[256]; snprintf(tool,sizeof(tool),"%s",tag.name);
        args=cJSON_CreateObject(); if (!args) goto invalid;
        int envelope=0, params=0;
        for (;;) {
            if (!dsml_tag_read(&p,&tag)) goto invalid;
            if (tag.closing && !strcmp(tag.kind,"invoke")) break;
            if (tag.closing || strcmp(tag.kind,"parameter") || !tag.name[0]) goto invalid;
            char key[256]; snprintf(key,sizeof(key),"%s",tag.name);
            if (envelope || cJSON_GetObjectItemCaseSensitive(args,key)) goto invalid;
            if (tag.string_value==1) {
                const char *end=p;
                dsml_tag close;
                for (;;) {
                    end=strchr(end,'<'); if (!end) goto invalid;
                    const char *probe=end;
                    if (dsml_tag_read(&probe,&close) && close.closing && !strcmp(close.kind,"parameter")) break;
                    end++;
                }
                size_t len=(size_t)(end-p);
                char *text=malloc(len+1); if (!text) goto invalid;
                memcpy(text,p,len); text[len]=0; value=cJSON_CreateString(text); free(text); p=end;
            } else {
                const char *end=NULL;
                value=cJSON_ParseWithOpts(p,&end,0);
                if (!value) goto invalid;
                p=end;
            }
            if (!value || !dsml_tag_read(&p,&tag) || !tag.closing || strcmp(tag.kind,"parameter")) goto invalid;
            if (!strcmp(key,"args") || !strcmp(key,"arguments")) {
                if (params || !cJSON_IsObject(value)) goto invalid;
                cJSON_Delete(args); args=value; value=NULL; envelope=1;
            } else {
                if (!cJSON_AddItemToObject(args,key,value)) goto invalid;
                value=NULL;
            }
            params++;
        }
        planned_action *grown=realloc(out,(size_t)(count+1)*sizeof(*out));
        if (!grown) goto invalid;
        out=grown; out[count].tool=xstrdup(tool); out[count].args_json=cJSON_PrintUnformatted(args);
        if (!out[count].tool || !out[count].args_json) {
            free(out[count].tool); free(out[count].args_json); goto invalid;
        }
        count++; cJSON_Delete(args); args=NULL;
    }
    if (!count || *dsml_space(p)) goto invalid;
    *actions=out; *n_actions=count; return 1;
invalid:
    cJSON_Delete(value); cJSON_Delete(args); planner_actions_free(out,count); return -1;
}

/* Shared planning core. Takes ownership of nothing; frees sys_prompt. */
static int plan_with(llm *llm, char *sys_prompt, const char *prompt, planned_action **actions, int *n_actions,
                     char **raw_out, char **err_out, const char *attachments) {
    llm_request req = {0};
    llm_response resp = {0};
    int rc;
    char *plan;
    int looks_like_plan;
    int have;

    if (actions)
        *actions = NULL;
    if (n_actions)
        *n_actions = 0;
    if (raw_out)
        *raw_out = NULL;
    if (err_out)
        *err_out = NULL;
    if (!llm || !prompt) {
        free(sys_prompt);
        if (err_out)
            *err_out = xstrdup("no LLM configured");
        return -1;
    }

    llm_message msgs[30] = {
        {.role = "system", .content = sys_prompt ? sys_prompt : SYS_PROMPT},
        {.role = "user", .content = prompt},
    };
    cJSON *files = attachments ? cJSON_Parse(attachments) : NULL;
    cJSON *file;
    char *owned[28] = {0};
    int count = 2;
    cJSON_ArrayForEach(file, files) {
        if (count == 30) break;
        cJSON *name = cJSON_GetObjectItemCaseSensitive(file, "name");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(file, "text");
        cJSON *image = cJSON_GetObjectItemCaseSensitive(file, "image_b64");
        cJSON *mime = cJSON_GetObjectItemCaseSensitive(file, "mime");
        cJSON *path = cJSON_GetObjectItemCaseSensitive(file, "path");
        cJSON *original = cJSON_GetObjectItemCaseSensitive(file,"original_path");
        cJSON *warnings = cJSON_GetObjectItemCaseSensitive(file,"warnings");
        strbuf content; strbuf_init(&content);
        strbuf_appendf(&content, "User attachment: %s\n", cJSON_IsString(name) ? name->valuestring : "file");
        if (cJSON_IsString(path)) strbuf_appendf(&content, "Extracted file path: %s\n", path->valuestring);
        if(cJSON_IsString(original)) strbuf_appendf(&content,"Original file path: %s\n",original->valuestring);
        cJSON *warning; cJSON_ArrayForEach(warning,warnings) if(cJSON_IsString(warning)) strbuf_appendf(&content,"Processing note: %s\n",warning->valuestring);
        if (cJSON_IsString(text)) {
            size_t len=strlen(text->valuestring), preview=len>32000?32000:len;
            while(preview<len && ((unsigned char)text->valuestring[preview]&0xc0)==0x80) preview--;
            strbuf_append_n(&content,text->valuestring,preview);
            if(preview<len) strbuf_append(&content,"\n[Text preview truncated. Read the extracted file for remaining content.]\n");
        }
        owned[count-2] = strbuf_detach(&content);
        msgs[count] = (llm_message){.role="user", .content=owned[count-2],
            .image_b64=cJSON_IsString(image) ? image->valuestring : NULL,
            .image_mime=cJSON_IsString(mime) ? mime->valuestring : NULL};
        count++;
        cJSON *frames=cJSON_GetObjectItemCaseSensitive(file,"frames"), *frame;
        cJSON_ArrayForEach(frame,frames) {
            if(count==30) break;
            cJSON *b64=cJSON_GetObjectItemCaseSensitive(frame,"image_b64");
            cJSON *label=cJSON_GetObjectItemCaseSensitive(frame,"label");
            cJSON *type=cJSON_GetObjectItemCaseSensitive(frame,"mime");
            if(!cJSON_IsString(b64)) continue;
            strbuf caption; strbuf_init(&caption);
            strbuf_appendf(&caption,"Attachment %s - %s",cJSON_IsString(name)?name->valuestring:"file",
                cJSON_IsString(label)?label->valuestring:"Page/frame");
            owned[count-2]=strbuf_detach(&caption);
            msgs[count]=(llm_message){.role="user",.content=owned[count-2],.image_b64=b64->valuestring,
                .image_mime=cJSON_IsString(type)?type->valuestring:"image/jpeg"};
            count++;
        }
    }
    req.messages = msgs;
    req.num_messages = count;
    int deepseek_v41 = llm->model && strstr(llm->model, "deepseek-v4.1-flash") != NULL;
    req.temperature = deepseek_v41 ? 1.0 : 0.2;
    /* Bound this model's hidden reasoning so it cannot consume the planner's
     * entire output budget before returning a parseable plan. */
    req.reasoning_effort = deepseek_v41 ? "low" : NULL;
    int visual = 0;
    for (int i=0;i<count;i++) if(msgs[i].image_b64 && *msgs[i].image_b64) visual=1;
    if (visual) {
        /* Image/frame requests need a visible bound independent of the long
         * source-generation budget. Do not infer vision support from names. */
        long limit=120000;
        const char *setting=getenv("COA_LLM_VISION_TIMEOUT_MS");
        if(setting && *setting) {
            char *end=NULL; long value=strtol(setting,&end,10);
            if(end && !*end && value>=1000 && value<=600000) limit=value;
        }
        int global=llm_timeout_ms();
        req.timeout_ms=limit<global ? limit:global;
        log_info("planner: visual request model=%s timeout_ms=%ld max_tokens=8192",llm->model ? llm->model:"unknown",req.timeout_ms);
    }
    req.max_tokens = 32768; /* tool plans carrying file_write content (whole
                             * scripts) need real headroom; 2048 truncated the
                             * JSON mid-string and the plan was lost. Thinking
                             * models spend reasoning tokens from the SAME
                             * budget — glm-5.3-flash burned all 8192 on
                             * invisible reasoning and returned empty content
                             * (finish_reason=length) */
    if(visual) req.max_tokens=8192;
    rc = llm_chat(llm, &req, &resp);
    for (int i=0;i<28;i++) free(owned[i]);
    cJSON_Delete(files);
    free(sys_prompt);
    if (rc != 0) {
        if (err_out) {
            if(visual) {
                strbuf diagnostic; strbuf_init(&diagnostic);
                strbuf_appendf(&diagnostic,"图片/视频帧模型请求失败（模型 %s，HTTP 等待超时配置 %ld ms）：%s。请检查接口是否支持图片输入；慢响应可调整 COA_LLM_VISION_TIMEOUT_MS。",
                    llm->model ? llm->model:"unknown",req.timeout_ms,resp.error ? resp.error:"LLM call failed");
                *err_out=strbuf_detach(&diagnostic);
            } else *err_out = xstrdup(resp.error ? resp.error : "LLM call failed");
        }
        free(resp.content);
        free(resp.error);
        return visual ? -2 : -1;
    }

    plan = resp.content;
    resp.content = NULL;
    free(resp.error);
    if (!plan) {
        if (err_out)
            *err_out = xstrdup("LLM returned an empty response");
        return -1;
    }
    /* Some providers include reasoning tags in content. Never parse a tool
     * example from their hidden preamble as the executable plan. JSON replies
     * are left intact, including literal tags inside file contents. */
    const char *visible=plan;
    while (*visible==' ' || *visible=='\n' || *visible=='\r') ++visible;
    if (*visible!='[' && *visible!='{') {
        const char *close=NULL, *scan=visible;
        while ((scan=strstr(scan,"</think>"))!=NULL) {
            close=scan+8; scan=close;
            while (*scan==' ' || *scan=='\n' || *scan=='\r') ++scan;
            if (*scan=='[' || *scan=='{') break;
        }
        if (!close && strncmp(visible,"<think>",7)==0) {
            if (err_out) *err_out=xstrdup("model response contains an unfinished thinking block");
            free(plan); return -1;
        }
        if (close) {
            char *clean=xstrdup(close);
            if (clean) { free(plan); plan=clean; }
        }
    }

    if (raw_out)
        *raw_out = plan; /* ownership moves to the caller */
    if (!actions || !n_actions) {
        if (!raw_out)
            free(plan);
        return 0;
    }

    /* Parse the reply into tool actions. Real LLMs wrap JSON in fences or
     * prose, and sometimes emit structurally invalid JSON (mis-ordered
     * brackets), which used to silently demote a real plan to a plain-text
     * answer — the agent then "answered" instead of acting. */
    looks_like_plan = strstr(plan, "\"tool\"") != NULL || strstr(plan, "'tool'") != NULL ||
                      strstr(plan, "<tool_call>") != NULL;
    have = parse_dsml_tool_calls(plan, actions, n_actions);
    if (have<0) {
        if (err_out) *err_out=xstrdup("invalid or incomplete DSML tool-call response; no actions executed");
        if (raw_out) *raw_out=NULL;
        free(plan);
        return -1;
    }
    if (!have) have = parse_tagged_tool_calls(plan, actions, n_actions);
    if (!have)
        have = parse_plan_actions(plan, actions, n_actions);
    if (!have && looks_like_plan) {
        const char *epos = cJSON_GetErrorPtr();
        size_t off = (epos && epos >= plan && epos < plan + strlen(plan)) ? (size_t)(epos - plan) : 0;
        log_warn("planner: invalid plan JSON (byte %zu / len %zu) — requesting repair", off, strlen(plan));
        /* Salvage first: plans carrying large file_write payloads are often
         * truncated by the LLM output limit. The repair round-trip asks the
         * model to re-emit the SAME oversized plan, which truncates again —
         * so keep the complete leading actions locally before wasting that
         * call. The agent loop re-plans for whatever the truncated tail
         * dropped, so no work is silently lost. */
        char *salv = salvage_truncated_array(plan);
        if (salv && parse_plan_actions(salv, actions, n_actions) && *n_actions > 0) {
            log_warn("planner: salvaged %d complete action(s) from truncated plan", *n_actions);
            if (raw_out) {
                free(*raw_out);
                *raw_out = salv;
            } else
                free(salv);
            have = 1;
        } else {
            free(salv);
            /* one repair round-trip: the model re-emits its own plan as clean JSON */
            size_t plen = strlen(plan);
            char *user = (char *)malloc(plen + 512);
            if (user) {
                snprintf(user, plen + 512,
                         "The following reply was meant to be a JSON array of tool actions "
                         "but is not valid JSON (bad bracket order or truncation). "
                         "Re-emit it as ONE valid JSON array. Output ONLY the JSON, no "
                         "markdown fences, no prose:\n\n%s",
                         plan);
            }
            /* same headroom as the planning call — a plan that hit the output
             * limit cannot fit in a smaller repair budget either */
            char *fixed = user ? llm_chat_simple_ex(
                                     llm, "You repair broken JSON. Output ONLY the corrected JSON array.", user,
                                     32768)
                               : NULL;
            if (fixed) {
                if (parse_plan_actions(fixed, actions, n_actions)) {
                    if (raw_out) {
                        free(*raw_out);
                        *raw_out = fixed;
                    } else
                        free(fixed);
                    log_info("planner: repaired plan JSON accepted (%d actions)", *n_actions);
                } else if ((salv = salvage_truncated_array(fixed)) != NULL &&
                           parse_plan_actions(salv, actions, n_actions) && *n_actions > 0) {
                    /* the repair itself got truncated — salvage its prefix */
                    log_warn("planner: salvaged %d action(s) from truncated repair", *n_actions);
                    if (raw_out) {
                        free(*raw_out);
                        *raw_out = salv;
                    } else
                        free(salv);
                    free(fixed);
                } else {
                    free(salv);
                    free(fixed);
                }
            }
            free(user);
        }
    }

    if (!raw_out)
        free(plan);
    return 0;
}

void planner_actions_free(planned_action *a, int n) {
    for (int i = 0; i < n; i++) {
        free(a[i].tool);
        free(a[i].args_json);
    }

    free(a);
}

int planner_plan(llm *llm, const char *prompt, planned_action **actions, int *n_actions, char **raw_out,
                     char **err_out) {
    return plan_with(llm, NULL, prompt, actions, n_actions, raw_out, err_out, NULL);
}

int planner_plan_ex(llm *llm, const struct tool_registry *tools, struct skill_registry *skills,
                        struct policy_engine *policy, const char *prompt, planned_action **actions,
                        int *n_actions, char **raw_out, char **err_out) {
    char *sys = build_catalog_prompt(tools, skills, policy);
    return plan_with(llm, sys, prompt, actions, n_actions, raw_out, err_out, NULL);
}

int planner_plan_attachments_ex(llm *llm, const struct tool_registry *tools, struct skill_registry *skills,
    struct policy_engine *policy, const char *prompt, planned_action **actions,
    int *n_actions, char **raw_out, char **err_out, const char *attachments) {
    return plan_with(llm, build_catalog_prompt(tools,skills,policy), prompt, actions,
        n_actions, raw_out, err_out, attachments);
}
