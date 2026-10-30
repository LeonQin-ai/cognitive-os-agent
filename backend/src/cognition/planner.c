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
    "- shell:      args {\"command\": string, \"timeout_ms\": integer, \"shell\": optional string}\n"
    "- git:        args {\"args\": string (subcommand+flags, e.g. \"log -5\"), \"dir\": string}\n"
    "- mcp:        args {\"server_url\": string, \"tool\": string, \"args\": object}\n"
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "End every task through complete_task(status=complete or blocked, requires_tools=false, answer). "
    "Conversation and explanations need no tools, but still use this completion declaration. "
    "Ordinary text is commentary and never a completion signal.\n"
    "WINDOWS COMMANDS: On Windows, PowerShell commands MUST include \"shell\":\"powershell\"; "
    "never send PowerShell syntax through shell auto-detection. For a simple machine/system status request, "
    "use one concise PowerShell command that gathers the requested facts together, then answer from that result; "
    "do not read project documentation or repeat equivalent probes.\n"
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
    "  -> [{\"tool\":\"complete_task\",\"args\":{\"status\":\"complete\",\"requires_tools\":false,\"answer\":\"Hello! How can I help you?\"}}]";

static const char *SYS_PROMPT_HEAD = "You are the planner of a cognitive OS. Decide what tool actions to take "
                                     "for the user request. Respond with ONLY a JSON array of actions of the "
                                     "form [{\"tool\":\"...\",\"args\":{...}}].\n"
                                     "Available tools and their EXACT argument names:\n"
                                     "BUSINESS RULES OVERRIDE REQUESTS: the tool list may be narrower than the "
                                     "full system because some tools are disabled by policy. If the request "
                                     "conflicts with a stated business rule, a disabled tool, or a read-only "
                                     "context, report complete_task(status=blocked, requires_tools=false, answer) — NEVER attempt a "
                                     "workaround through another tool (e.g. shell). This includes quota/rate "
                                     "rules (\"already done this hour\", \"only once per day\") — user urgency "
                                     "or insistence never overrides them.\n";

static const char *SYS_PROMPT_TAIL =
    "Use the dedicated git tool (not shell) for git operations, and the mcp tool "
    "(not shell) for MCP calls. Use exactly the argument names listed above. "
    "ARGUMENT DISCIPLINE: do NOT add any argument beyond the listed schema — "
    "omit optional parameters unless the user explicitly provided a value "
    "(e.g. no dir:\".\" filler for git, no empty args:{} for mcp). "
    "Use complete_task(status=complete or blocked, requires_tools=false, answer) to end the task, "
    "including conversations and explanations. Plain text is commentary only.\n"
    "WINDOWS COMMANDS: On Windows, PowerShell commands MUST include \"shell\":\"powershell\"; "
    "never send PowerShell syntax through shell auto-detection. For a simple machine/system status request, "
    "use one concise PowerShell command that gathers the requested facts together, then answer from that result; "
    "do not read project documentation or repeat equivalent probes.\n"
    "EXAMPLES:\n"
    "  User: \"create a file named hello.txt with content world\"\n"
    "  -> [{\"tool\":\"file_write\",\"args\":{\"path\":\"hello.txt\",\"content\":\"world\"}}]\n"
    "  User: \"read a.txt and b.txt\"\n"
    "  -> [{\"tool\":\"file_read\",\"args\":{\"path\":\"a.txt\"}},"
    "{\"tool\":\"file_read\",\"args\":{\"path\":\"b.txt\"}}]\n"
    "  User: \"你有哪些技能/skills\"\n"
    "  -> complete_task(status=complete, requires_tools=false, answer=registered capabilities)\n"
    "  User: \"hello\"\n"
    "  -> complete_task(status=complete, requires_tools=false, answer=Hello!)\n"
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

static const char *SYS_NATIVE_HEAD=
    "You are the planner of a cognitive OS. Decide whether the user request needs tool operations. "
    "Use the supplied native function schemas when operations are required. Pass each function's "
    "parameter object directly; never nest an action envelope with tool/args inside its parameters. "
    "When no further operations are needed, call complete_task(status=complete, requires_tools=false, answer). "
    "If genuinely blocked with no actionable recovery, call complete_task(status=blocked, requires_tools=false, answer). "
    "Tool errors are observations: diagnose and repair them before completing. "
    "Track the full requested scope, verify each required result, and only then emit complete_task. "
    "If inspected code already satisfies the requirement, report that verified no-change result and finish; "
    "do not invent a defect or reread unchanged pages to find something to edit. "
    "The final answer must describe completed verified work and explicit remaining blockers, not promise unexecuted follow-up. "
    "After a successful observation, decide whether it already supplies the evidence requested by the user. "
    "If it does, complete_task from that result. A changing live value does not by itself require another sample. "
    "Do not repeat a successful probe merely to change its format, shell or wrapper, or to verify that observation again. "
    "Additional calls must obtain evidence still required by the request; explicit repeated sampling, unread pages, "
    "and verification of an actual change remain valid reasons to continue. "
    "For exact file duplication, use file_write(path=destination, copy_from=source) rather than regenerating content "
    "from a displayed file_read result. Display wrappers are not source bytes. Include file_equalities:[{source,target}] "
    "in complete_task when byte equality is a requested acceptance criterion; runtime checks actual bytes and will reject mismatches. "
    "Business rules and tool deny policies override user requests; never bypass them through another tool.\n";
static const char *SYS_NATIVE_TAIL=
    "Use dedicated git and MCP functions for their operations. Only supply schema-defined arguments. "
    "Conversational answers, translations, explanations and requested code examples can be returned through "
    "complete_task without executing those examples. Genuine file operations require actual tool calls and verification. "
    "For a missing input path, discover its real location; for an explicitly requested new output file, create it "
    "rather than repeatedly reading the absent file. Read requested source scope and follow page offsets when needed. "
    "On Windows explicitly select PowerShell for PowerShell syntax. Binary documents require extraction tools "
    "or appropriate Python libraries, not repeated raw text reads.\n";

/* Build the system prompt from the ACTUAL tool + skill registries. Returns a
 * malloc'd prompt, or NULL when no registry is available (caller falls back
 * to the static catalog). Tools denied by `policy` are hidden entirely
 * (Claude Code: deny rules both block calls AND remove from the tool pool). */
static char *build_catalog_prompt(const tool_registry *tools, struct skill_registry *skills,
                                  struct policy_engine *policy, int native) {
    strbuf b;
    int have_skill_tool = 0;
    int have_glob_tool = 0;

    if (!tools)
        return NULL;
    strbuf_init(&b);
    strbuf_append(&b, native ? SYS_NATIVE_HEAD:SYS_PROMPT_HEAD);
    strbuf_append(&b,"\nControl decisions: declare_task({acceptance:{requires_action:boolean,requires_observation:boolean,artifacts:[path],full_reads:[path]}}) records requirements before operations. Artifact paths require a real nonempty write and later readback; full_reads require contiguous EOF. Requirements accumulate and cannot be omitted later. complete_task may append acceptance. Use complete_task(status=complete or blocked,requires_tools=false,answer) for every final answer, including tool-free conversation. Plain text is commentary.\n");
    strbuf_append(&b,"\nFor a guessed exploratory file_read path that does not exist and is not required for the requested result, verify absence with list_dir of its exact parent after the failure. Then complete_task may include unavailable_optional_reads:[{path,reason,evidence_step_id}], using the successful directory step ID from Current execution state. Explain the omitted source and base conclusions on sources actually read. Do not retry an absent path indefinitely. Never use this for required source/deliverable reads, existing unread files, failed writes or commands.\n");
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
    if (have_glob_tool && native)
        strbuf_append(&b,"For file discovery use glob with a non-empty pattern parameter, then reuse the returned paths.\n");
    if (have_glob_tool && !native)
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
        strbuf_append(&b,"Explain registered capabilities via complete_task when no operations are required.\n");
    }

    strbuf_append(&b, native ? SYS_NATIVE_TAIL:SYS_PROMPT_TAIL);
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
        cJSON *p = cJSON_ParseWithOpts(args->valuestring,NULL,1);
        if (p) {
            char *s = cJSON_IsObject(p) ? cJSON_PrintUnformatted(p) : NULL;
            cJSON_Delete(p);
            if (s)
                return s;
        }
    }

    return NULL;
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

/* A tool identifier cannot contain JSON syntax from a broken envelope. */
static int valid_action_name(const char *name, size_t n) {
    if(!name || !n || n>255)return 0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)name[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;
    }
    return 1;
}

/* 1: valid batch; 0: no recognized plan; -1: malformed explicit batch. */
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
    /* Normalize explicit wire envelopes locally; no model repair is needed
     * for a valid function object or tool_calls batch. */
    cJSON *wire_calls=cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root,"tool_calls") : NULL;
    if(wire_calls){
        if(!cJSON_IsArray(wire_calls)){cJSON_Delete(root);return -1;}
        cJSON *list=cJSON_DetachItemFromObjectCaseSensitive(root,"tool_calls");
        cJSON_Delete(root);root=list;
    }
    if (cJSON_IsObject(root) && (cJSON_GetObjectItemCaseSensitive(root, "tool") ||
        cJSON_GetObjectItemCaseSensitive(root,"function") ||
        (cJSON_GetObjectItemCaseSensitive(root,"name") && cJSON_GetObjectItemCaseSensitive(root,"arguments")))) {
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
            goto invalid_batch;
        tool = cJSON_GetObjectItemCaseSensitive(it, "tool");
        args = cJSON_GetObjectItemCaseSensitive(it, "args");
        if (!args) args=cJSON_GetObjectItemCaseSensitive(it,"arguments");
        if (!tool) {
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

        if (!tool || !cJSON_IsString(tool) || !valid_action_name(tool->valuestring,strlen(tool->valuestring)))
            goto invalid_batch;
        args_json = args_to_json(args);
        if(!args_json)goto invalid_batch;
        na = (planned_action *)realloc(a, (size_t)(n + 1) * sizeof(planned_action));
        if (!na) {
            free(args_json);
            goto invalid_batch;
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
invalid_batch:
    planner_actions_free(a,n);cJSON_Delete(root);
    return -1;
}

/* Control markup outside Markdown code examples is pending protocol, never a
 * delivered answer. Apply this at every completion boundary, including after
 * the one-shot semantic review. Do not execute or invent malformed arguments. */
static const char *next_tool_markup(const char *text, int include_dsml) {
    for (const char *p=text; p && *p; ++p) {
        if (*p=='`' || (*p=='~' && p[1]=='~' && p[2]=='~')) {
            char quote=*p; size_t n=1;
            while(p[n]==quote)++n;
            const char *end=p+n;
            while((end=strchr(end,quote))!=NULL) {
                size_t count=1;while(end[count]==quote)++count;
                if(count==n || (n>=3 && count>n)){p=end+count-1;break;}
                end+=count;
            }
            if(end)continue;
        }
        if(!strncmp(p,"<tool_call",10) &&
           (!p[10] || p[10]=='>' || isspace((unsigned char)p[10]))) return p;
        if(include_dsml && (!strncmp(p,"<｜｜DSML｜｜",strlen("<｜｜DSML｜｜")) ||
            !strncmp(p,"<｜DSML｜",strlen("<｜DSML｜")) ||
            !strncmp(p,"<||DSML||",9) || !strncmp(p,"<|DSML|",7))) return p;
    }
    return NULL;
}

/* Some OpenAI-compatible providers serialize tool calls as
 *   <tool_call>shell{"command":"..."}<tool_call>file_read{"path":"..."}
 * instead of putting the tool name inside the JSON object. Parse every such
 * block locally; otherwise the reply is mistaken for prose and the model can
 * repeat it forever without executing anything. */
static int parse_tagged_tool_calls(const char *plan, planned_action **actions, int *n_actions) {
    const char *root=plan;while(isspace((unsigned char)*root))root++;
    /* Structured JSON owns all of its contents, including literal tags. */
    if(*root=='[' || *root=='{')return 0;
    const char *first=next_tool_markup(root,0);
    /* Prose may precede provider shorthand; Markdown examples remain inert. */
    if(first && first!=root) {
        if(strncmp(first,"<tool_call>",11))return -1;
        plan=first;
    }
    const char *p = plan;
    while(isspace((unsigned char)*p))++p;
    /* A tag in JSON arguments is literal tool data. Parse that envelope below. */
    if(*p=='[' || *p=='{')return 0;
    planned_action *out = NULL;
    int count = 0;

    while ((p = next_tool_markup(p,0)) != NULL) {
        if(strncmp(p,"<tool_call>",11)){planner_actions_free(out,count);return -1;}
        const char *name = p + strlen("<tool_call>");
        while(*name==' ' || *name=='\t' || *name=='\r' || *name=='\n')++name;
        const char *json = name;
        while (*json && *json != '{' && *json != '[' && *json != '<')
            json++;
        const char *name_end = json;
        while (name_end > name && (name_end[-1] == ' ' || name_end[-1] == '\t' || name_end[-1] == '\r' || name_end[-1] == '\n'))
            name_end--;
        if (json == name || (*json != '{' && *json != '[')) {
            planner_actions_free(out,count);return -1;
        }

        if(!valid_action_name(name,(size_t)(name_end-name))) {
            planner_actions_free(out,count);return -1;
        }
        char *body = extract_json_span(json);
        if (!body) {planner_actions_free(out,count);return -1;}
        const char *json_end=json+strlen(body);
        cJSON *args = cJSON_Parse(body);
        free(body);
        if (!args || !cJSON_IsObject(args)) {
            cJSON_Delete(args);
            planner_actions_free(out,count);return -1;
        }

        planned_action *grown = realloc(out, (size_t)(count + 1) * sizeof(*out));
        if (!grown) {
            cJSON_Delete(args);
            planner_actions_free(out,count);return -1;
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
            planner_actions_free(out,count);return -1;
        }
        count++;

        /* Skip the entire parsed object. Tags inside argument strings are
         * literal payload, never additional executable calls. */
        p = json_end;
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
    /* A protocol envelope must be structurally top-level. Free prose cannot
     * authorize execution, regardless of words such as "example". */
    if(strncmp(p,"<｜",4) && strncmp(p,"<|",2) && strncmp(p,"<||",3))return 0;
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
            if ((!strcmp(key,"args") || !strcmp(key,"arguments")) && cJSON_IsObject(value)) {
                if (params) goto invalid;
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
static int planner_output_tokens(llm *l, int requested) {
    long long window = llm_context_window_tokens(l);
    long long output = requested > 0 ? requested : window / 4;
    long long upper = requested > 0 ? 65536 : 32768;
    /* Output capacity is separate from the model's input context window. Use
     * the context only as a hard combined-request safety bound; the runtime
     * output ceiling follows Claude Code's default/one-time escalation shape. */
    if (window > 0 && output > window / 2) output = window / 2;
    if (output > upper) output = upper;
    if (output < 1024) output = 1024;
    if (l && l->max_output_tokens > 0 && output > l->max_output_tokens) output=l->max_output_tokens;
    return (int)output;
}

static char *native_tool_catalog(const tool_registry *tools, struct policy_engine *policy);

int planner_uses_native_tools(const llm *l) {
    const char *protocol=getenv("COA_TOOL_PROTOCOL");
    return (!protocol || strcmp(protocol,"legacy")) && l && l->provider &&
           (!strcmp(l->provider,"openai") || !strcmp(l->provider,"anthropic"));
}


size_t planner_input_budget_bytes_with_output(llm *l, const tool_registry *tools,
    struct skill_registry *skills, struct policy_engine *policy, int output_tokens) {
    char *sys = build_catalog_prompt(tools, skills, policy,planner_uses_native_tools(l));
    size_t sys_bytes = strlen(sys ? sys : SYS_PROMPT);
    free(sys);
    if(planner_uses_native_tools(l)) {
        char *native=native_tool_catalog(tools,policy);
        if(native)sys_bytes+=strlen(native)+1024; /* protocol instructions + native schemas */
        free(native);
    }
    long long available = llm_context_window_tokens(l) - planner_output_tokens(l, output_tokens) - 1024;
    /* Two UTF-8 bytes per token leaves room for Chinese, code and JSON. */
    available -= (long long)(sys_bytes + 1) / 2;
    return available > 0 ? (size_t)available * 2 : 0;
}

size_t planner_input_budget_bytes(llm *l, const tool_registry *tools,
    struct skill_registry *skills, struct policy_engine *policy) {
    return planner_input_budget_bytes_with_output(l, tools, skills, policy, 0);
}

static cJSON *acceptance_schema(void) {
    return cJSON_Parse("{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"requires_action\":{\"type\":\"boolean\"},\"requires_observation\":{\"type\":\"boolean\"},\"artifacts\":{\"type\":\"array\",\"maxItems\":64,\"items\":{\"type\":\"string\"}},\"full_reads\":{\"type\":\"array\",\"maxItems\":64,\"items\":{\"type\":\"string\"}}}}");
}
static char *native_tool_catalog(const tool_registry *tools, struct policy_engine *policy) {
    if (!tools) return NULL;
    cJSON *list=cJSON_CreateArray();
    for (int i=0;i<tool_registry_count(tools);++i) {
        const tool *t=tool_registry_get(tools,(size_t)i);
        if(!t || !t->name || strlen(t->name)>64 ||
           (policy && policy_check(policy,t->name,"{}",NULL)==POLICY_DENY)) continue;
        int valid=1;
        for(const unsigned char *p=(const unsigned char*)t->name;*p;++p)
            if(!isalnum(*p) && *p!='_' && *p!='-')valid=0;
        if(!valid)continue;
        cJSON *entry=cJSON_CreateObject(), *fn=cJSON_CreateObject();
        cJSON_AddStringToObject(entry,"type","function");cJSON_AddItemToObject(entry,"function",fn);
        cJSON_AddStringToObject(fn,"name",t->name);
        cJSON_AddStringToObject(fn,"description",t->description?t->description:"");
        cJSON *schema=t->json_schema?cJSON_Parse(t->json_schema):NULL;
        if(!cJSON_IsObject(schema)) {cJSON_Delete(schema);schema=cJSON_CreateObject();cJSON_AddStringToObject(schema,"type","object");}
        /* file-path is a local validation format, not a provider JSON-schema extension. */
        cJSON *property;
        cJSON_ArrayForEach(property,cJSON_GetObjectItemCaseSensitive(schema,"properties")) {
            const char *format=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(property,"format"));
            if(format && !strcmp(format,"file-path")) cJSON_DeleteItemFromObjectCaseSensitive(property,"format");
        }
        cJSON_AddItemToObject(fn,"parameters",schema);cJSON_AddItemToArray(list,entry);
    }
    cJSON *finish=cJSON_Parse("{\"type\":\"function\",\"function\":{\"name\":\"complete_task\",\"description\":\"End this task only when no further tool operations are required. Report complete or blocked and the final answer. If tools are needed, call those tools instead.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\",\"enum\":[\"complete\",\"blocked\"]},\"requires_tools\":{\"type\":\"boolean\"},\"answer\":{\"type\":\"string\"}},\"required\":[\"status\",\"requires_tools\",\"answer\"]}}}");
    if(finish){
        cJSON *properties=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(finish,"function"),"parameters"),"properties");
        cJSON_AddItemToObject(properties,"acceptance",acceptance_schema());
        cJSON *optional=cJSON_Parse("{\"type\":\"array\",\"description\":\"Only for absent exploratory file_read paths that are not required deliverables: cite a later successful list_dir of the exact parent to verify absence. Never waive required reads or failed writes/commands.\",\"items\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"reason\":{\"type\":\"string\"},\"evidence_step_id\":{\"type\":\"integer\"}},\"required\":[\"path\",\"reason\",\"evidence_step_id\"]}}");
        if(properties && optional)cJSON_AddItemToObject(properties,"unavailable_optional_reads",optional);
        else cJSON_Delete(optional);
        cJSON *equalities=cJSON_Parse("{\"type\":\"array\",\"description\":\"Requested exact-byte acceptance checks. Compared by the runtime before task completion; previously declared checks cannot be silently dropped. At most 16, each file at most 64 MiB. Use [] when no equality is required.\",\"maxItems\":16,\"items\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"}},\"required\":[\"source\",\"target\"]}}");
        if(properties&&equalities)cJSON_AddItemToObject(properties,"file_equalities",equalities);else cJSON_Delete(equalities);
        cJSON_AddItemToArray(list,finish);
    }
    cJSON *declare=cJSON_Parse("{\"type\":\"function\",\"function\":{\"name\":\"declare_task\",\"description\":\"Declare explicit task acceptance before executing operations. Requirements accumulate; later empty declarations cannot erase them.\",\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[\"acceptance\"]}}}");
    if(declare){cJSON *props=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(declare,"function"),"parameters"),"properties");cJSON_AddItemToObject(props,"acceptance",acceptance_schema());cJSON_AddItemToArray(list,declare);}
    char *result=cJSON_GetArraySize(list)?cJSON_PrintUnformatted(list):NULL;cJSON_Delete(list);return result;
}

static int plan_with(llm *llm, char *sys_prompt, const char *prompt, planned_action **actions, int *n_actions,
                     char **raw_out, char **err_out, const char *attachments, int *truncated_out,
                     int output_tokens, const tool_registry *tools, struct policy_engine *policy, int *native_end_out, planner_model_diagnostic *diagnostic, int require_completion, char **completion_out) {
    if(completion_out)*completion_out=NULL;
    llm_request req = {0};
    if(native_end_out)*native_end_out=0;
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
    req.max_tokens = planner_output_tokens(llm, output_tokens); /* tool plans carrying file_write content (whole
                             * scripts) need real headroom; 2048 truncated the
                             * JSON mid-string and the plan was lost. Thinking
                             * models spend reasoning tokens from the SAME
                             * budget — glm-5.3-flash burned all 8192 on
                             * invisible reasoning and returned empty content
                             * (finish_reason=length) */
    if(visual && req.max_tokens>8192) req.max_tokens=8192;
    char *native_tools=NULL;
    if (planner_uses_native_tools(llm))
        native_tools=native_tool_catalog(tools,policy);
    req.tools_json=native_tools;
    req.structured_decision=native_tools!=NULL || require_completion;
    char *native_system=NULL;
    if(native_tools) {
        strbuf system;strbuf_init(&system);strbuf_append(&system,sys_prompt ? sys_prompt:SYS_NATIVE_HEAD);
        strbuf_append(&system,"\nNATIVE TOOL PROTOCOL: When further operations are needed, emit native tool calls using the supplied function schemas. Any text alongside a tool call is commentary, never completion. After all operations and verification finish, call complete_task with status=complete, requires_tools=false, and the final answer. If blocked with no actionable tool recovery, use status=blocked. Plain text is commentary, never a completion declaration. Never set requires_tools=false when operations remain. Never call complete_task together with other tools. Tool errors are observations to diagnose and correct.\n");
        native_system=strbuf_detach(&system);msgs[0].content=native_system;
    }
    char *review_system = NULL;
    if (require_completion && !native_tools) {
        strbuf system; strbuf_init(&system);
        strbuf_append(&system, msgs[0].content ? msgs[0].content : "");
        strbuf_append(&system, "\nCOMPLETION REVIEW PROTOCOL: Compare the candidate answer with the ORIGINAL request and actual tool evidence. A future-work announcement is not a delivered result. If work remains, return the next necessary tools as the usual JSON action array. Only when all requested work is delivered, return exactly [{\"tool\":\"complete_task\",\"args\":{\"status\":\"complete\",\"requires_tools\":false,\"answer\":\"full final answer\"}}]. If genuinely blocked, use status=blocked with an explanation. Never combine complete_task with other tools. Plain text is commentary and cannot end this reviewed task. Do not reread unchanged evidence merely to confirm completion.\n");
        review_system = strbuf_detach(&system); msgs[0].content = review_system;
    }
    rc = llm_chat(llm, &req, &resp);
    free(review_system);
    if (diagnostic) {
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->http_status = resp.http_status;
        diagnostic->max_tokens = req.max_tokens;
        diagnostic->truncated = resp.truncated;
        diagnostic->has_content = resp.visible_content_bytes > 0 || (resp.content && *resp.content);
        diagnostic->has_tool_calls = resp.tool_call_count > 0 || resp.tool_actions_json != NULL;
        snprintf(diagnostic->stop_reason, sizeof(diagnostic->stop_reason), "%s", resp.stop_reason);
        snprintf(diagnostic->stage, sizeof(diagnostic->stage), "%s",
            rc == 0 ? "protocol" : resp.http_status == 0 ? "transport" :
            resp.http_status != 200 ? "http" : "response");
    }
    int using_native=native_tools!=NULL;
    free(native_tools);free(native_system);
    if(native_end_out)*native_end_out=0;
    if(truncated_out) *truncated_out=resp.truncated;
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
        free(resp.tool_actions_json);
        return visual ? -2 : -1;
    }

    if(using_native) {
        cJSON *calls=resp.tool_actions_json?cJSON_Parse(resp.tool_actions_json):NULL;
        cJSON *finish=NULL;
        int finishes=0, declaring=0, has_declaration=0;
        cJSON *candidate;
        cJSON_ArrayForEach(candidate,calls) {
            const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(candidate,"tool"));
            if(!name || !valid_action_name(name,strlen(name))) {
                if(err_out)*err_out=xstrdup("invalid native tool identifier; no actions executed");
                free(resp.content);free(resp.tool_actions_json);free(resp.error);cJSON_Delete(calls);return -1;
            }
        }
        /* Validate the whole native batch before any action can execute. An
         * invalid parameter object is a planning error, not a failed file read. */
        cJSON_ArrayForEach(candidate,calls) {
            const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(candidate,"tool"));
            if(!strcmp(name,"complete_task") || !strcmp(name,"declare_task"))continue;
            const tool *definition=tools ? tool_find((tool_registry *)tools,name):NULL;
            char *args=cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(candidate,"args")),*error=NULL;
            if(definition && tool_validate_args(definition,args,&error)) {
                if(err_out){strbuf e;strbuf_init(&e);strbuf_appendf(&e,"native arguments for %s do not match schema: %s; no actions executed. Pass function parameters directly, not a tool/args action envelope.",name,error?error:"invalid arguments");*err_out=strbuf_detach(&e);}
                free(args);free(error);free(resp.content);free(resp.tool_actions_json);free(resp.error);cJSON_Delete(calls);return -1;
            }
            free(args);free(error);
        }
        for(int i=cJSON_GetArraySize(calls)-1;i>=0;--i) {
            cJSON *call=cJSON_GetArrayItem(calls,i);
            const char *name=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(call,"tool"));
            if(name && (!strcmp(name,"complete_task") || !strcmp(name,"declare_task"))) {
                if(!strcmp(name,"declare_task"))has_declaration=1;
                if(!finish){finish=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(call,"args"),1);declaring=!strcmp(name,"declare_task");}
                finishes++;cJSON_DeleteItemFromArray(calls,i);
            }
        }
        if(has_declaration && (finishes!=1 || cJSON_GetArraySize(calls)>0)) {
            if(err_out)*err_out=xstrdup("declare_task must be a separate control decision; no actions executed");
            free(resp.content);free(resp.tool_actions_json);free(resp.error);
            cJSON_Delete(calls);cJSON_Delete(finish);return -1;
        }
        if(cJSON_GetArraySize(calls)>0) {
            /* A finish declaration cannot outrun pending tools in the same response. */
            free(resp.tool_actions_json);resp.tool_actions_json=cJSON_PrintUnformatted(calls);
        } else {
            const char *status=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(finish,"status"));
            const char *answer=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(finish,"answer"));
            cJSON *needs=cJSON_GetObjectItemCaseSensitive(finish,"requires_tools");
            int valid=finishes==1 && status && (!strcmp(status,"complete") || !strcmp(status,"blocked")) &&
                cJSON_IsFalse(needs) && answer && *answer;
            if(declaring)valid=finishes==1 && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(finish,"acceptance"));
            if(valid && !declaring && next_tool_markup(answer,1)) {
                if(err_out)*err_out=xstrdup("completion contains unexecuted tool-call markup; do not return pending calls inside the answer. Emit the next necessary tool using the configured tool protocol, or provide the delivered result with examples inside Markdown code spans/fences.");
                free(resp.content);free(resp.tool_actions_json);free(resp.error);
                cJSON_Delete(calls);cJSON_Delete(finish);return -1;
            }
            if(valid) {
                log_info("planner: structured decision=%s",declaring?"declare_task":status);
                if(completion_out)*completion_out=cJSON_PrintUnformatted(finish);
                if(native_end_out)*native_end_out=declaring?3:!strcmp(status,"blocked")?2:1;
                if(raw_out)*raw_out=declaring?cJSON_PrintUnformatted(finish):xstrdup(answer);
                free(resp.content);free(resp.tool_actions_json);free(resp.error);
                cJSON_Delete(calls);cJSON_Delete(finish);return 0;
            }
            if(diagnostic)diagnostic->no_action_response=finishes==0 && resp.content && *resp.content;
            if(!finishes && resp.content && *resp.content && !resp.truncated) {
                if(native_end_out)*native_end_out=4;
                if(raw_out)*raw_out=xstrdup(resp.content);
                free(resp.content);free(resp.tool_actions_json);free(resp.error);
                cJSON_Delete(calls);cJSON_Delete(finish);return 0;
            }
            if(err_out)*err_out=xstrdup("native turn has no executable tool calls or valid complete_task declaration; call the required tools, or complete_task(status, requires_tools=false, answer) only after work is finished");
            free(resp.content);free(resp.tool_actions_json);free(resp.error);
            cJSON_Delete(calls);cJSON_Delete(finish);return -1;
        }
        cJSON_Delete(calls);cJSON_Delete(finish);
    }
    if(resp.tool_actions_json) {
        /* Blocks, not stop_reason or text wording, own the continuation decision. */
        free(resp.content);resp.content=resp.tool_actions_json;resp.tool_actions_json=NULL;
        if(native_end_out)*native_end_out=0;
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
    const char *shape=plan;while(*shape && isspace((unsigned char)*shape))shape++;
    looks_like_plan = *shape=='[' || *shape=='{' || !strncmp(shape,"```",3) || next_tool_markup(plan,0)!=NULL;
    have = parse_dsml_tool_calls(plan, actions, n_actions);
    if (have<0) {
        if (err_out) *err_out=xstrdup("invalid or incomplete DSML tool-call response; no actions executed");
        if (raw_out) *raw_out=NULL;
        free(plan);
        return -1;
    }
    if (!have) have = parse_tagged_tool_calls(plan, actions, n_actions);
    if(have<0) {
        if(err_out)*err_out=xstrdup("invalid or incomplete tagged tool-call response; no actions executed; use the configured tool protocol with a valid identifier and complete JSON object");
        if(raw_out)*raw_out=NULL;free(plan);return -1;
    }
    if (!have)
        have = parse_plan_actions(plan, actions, n_actions);
    if (have<0) {
        if(err_out)*err_out=xstrdup("invalid tool-action batch; no actions executed");
        if(raw_out)*raw_out=NULL;free(plan);return -1;
    }
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
        if (salv && parse_plan_actions(salv, actions, n_actions)==1 && *n_actions > 0) {
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
                                     req.max_tokens)
                               : NULL;
            if (fixed) {
                if (parse_plan_actions(fixed, actions, n_actions)==1) {
                    if (raw_out) {
                        free(*raw_out);
                        *raw_out = fixed;
                    } else
                        free(fixed);
                    have=1;
                    log_info("planner: repaired plan JSON accepted (%d actions)", *n_actions);
                } else if ((salv = salvage_truncated_array(fixed)) != NULL &&
                           parse_plan_actions(salv, actions, n_actions)==1 && *n_actions > 0) {
                    /* the repair itself got truncated — salvage its prefix */
                    log_warn("planner: salvaged %d action(s) from truncated repair", *n_actions);
                    if (raw_out) {
                        free(*raw_out);
                        *raw_out = salv;
                    } else
                        free(salv);
                    free(fixed);
                    have=1;
                } else {
                    free(salv);
                    free(fixed);
                }
            }
            free(user);
        }
    }

    if(raw_out)plan=*raw_out; /* repair may replace the original buffer */
    if(looks_like_plan && (!have || !*n_actions)) {
        planner_actions_free(*actions,*n_actions);*actions=NULL;*n_actions=0;
        if(err_out)*err_out=xstrdup("invalid tool-action response after repair; no actions executed; emit a valid action using the configured tool protocol");
        if(raw_out)*raw_out=NULL;free(plan);return -1;
    }

    /* Validate the entire batch before any action can mutate the workspace.
     * Execution still performs the normal tool policy/permission checks. */
    for(int i=0;i<*n_actions;i++) {
        const tool *definition=tools ? tool_find((tool_registry *)tools,(*actions)[i].tool):NULL;
        char *error=NULL;
        if(definition && tool_validate_args(definition,(*actions)[i].args_json,&error)) {
            if(diagnostic) {
                snprintf(diagnostic->rejected_tool,sizeof(diagnostic->rejected_tool),"%s",(*actions)[i].tool);
                cJSON *args=cJSON_Parse((*actions)[i].args_json);
                const char *path=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args,"path"));
                if(path)snprintf(diagnostic->rejected_path,sizeof(diagnostic->rejected_path),"%s",path);
                cJSON_Delete(args);
            }
            if(err_out){strbuf msg;strbuf_init(&msg);strbuf_appendf(&msg,"tool arguments for %s do not match schema: %s; no actions executed",(*actions)[i].tool,error?error:"invalid arguments");*err_out=strbuf_detach(&msg);}
            free(error);planner_actions_free(*actions,*n_actions);*actions=NULL;*n_actions=0;
            if(raw_out)*raw_out=NULL;free(plan);return -1;
        }
        free(error);
    }
    if (!using_native) {
        /* A JSON repair may have replaced/freed the original plan. */
        if (raw_out) plan = *raw_out;
        int finish_count = 0;
        for (int i=0; i<*n_actions; i++)
            if (!strcmp((*actions)[i].tool,"complete_task") || !strcmp((*actions)[i].tool,"declare_task")) finish_count++;
        if (finish_count == 1 && *n_actions == 1 && !resp.truncated) {
            int declaring=!strcmp((*actions)[0].tool,"declare_task");
            cJSON *finish=cJSON_Parse((*actions)[0].args_json);
            const char *status=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(finish,"status"));
            const char *answer=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(finish,"answer"));
            int valid=status && (!strcmp(status,"complete") || !strcmp(status,"blocked")) &&
                cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(finish,"requires_tools")) && answer && *answer;
            if(declaring)valid=cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(finish,"acceptance"));
            if(valid && !declaring && next_tool_markup(answer,1)) {
                planner_actions_free(*actions,*n_actions);*actions=NULL;*n_actions=0;
                cJSON_Delete(finish);
                if(err_out)*err_out=xstrdup("completion contains unexecuted tool-call markup; emit the next necessary tool as a valid JSON action array, not inside the answer. Put literal protocol examples inside Markdown code spans/fences.");
                if(raw_out)*raw_out=NULL;free(plan);return -1;
            }
            if (valid) {
                if (native_end_out) *native_end_out=declaring?3:!strcmp(status,"blocked") ? 2 : 1;
                if(completion_out)*completion_out=cJSON_PrintUnformatted(finish);
                char *final=declaring?cJSON_PrintUnformatted(finish):xstrdup(answer);
                planner_actions_free(*actions,*n_actions); *actions=NULL; *n_actions=0;
                cJSON_Delete(finish);
                if (raw_out) *raw_out=final; else free(final);
                free(plan); return 0;
            }
            cJSON_Delete(finish);
        }
        if(!finish_count && require_completion && !*n_actions && !resp.truncated) {
            if(diagnostic)diagnostic->no_action_response=1;
            if(native_end_out)*native_end_out=4;
            if(!raw_out)free(plan);
            return 0;
        }
        if (finish_count || (require_completion && !*n_actions)) {
            if(diagnostic)diagnostic->no_action_response=!finish_count && !*n_actions;
            planner_actions_free(*actions,*n_actions); *actions=NULL; *n_actions=0;
            if (err_out) *err_out=xstrdup("completion review requires next tool actions or a valid complete_task declaration; commentary is not completion");
            if (raw_out) *raw_out=NULL;
            free(plan); return -1;
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
    return plan_with(llm, NULL, prompt, actions, n_actions, raw_out, err_out, NULL, NULL, 0, NULL, NULL, NULL, NULL, 0, NULL);
}

int planner_plan_ex(llm *llm, const struct tool_registry *tools, struct skill_registry *skills,
                        struct policy_engine *policy, const char *prompt, planned_action **actions,
                        int *n_actions, char **raw_out, char **err_out) {
    char *sys = build_catalog_prompt(tools, skills, policy,planner_uses_native_tools(llm));
    return plan_with(llm, sys, prompt, actions, n_actions, raw_out, err_out, NULL, NULL, 0, tools, policy, NULL, NULL, 0, NULL);
}

int planner_plan_attachments_ex(llm *llm, const struct tool_registry *tools, struct skill_registry *skills,
    struct policy_engine *policy, const char *prompt, planned_action **actions,
    int *n_actions, char **raw_out, char **err_out, const char *attachments, int *truncated_out) {
    return plan_with(llm, build_catalog_prompt(tools,skills,policy,planner_uses_native_tools(llm)), prompt, actions,
        n_actions, raw_out, err_out, attachments, truncated_out, 0, tools, policy, NULL, NULL, 0, NULL);
}

int planner_plan_attachments_ex_with_output(llm *llm, const struct tool_registry *tools,
    struct skill_registry *skills, struct policy_engine *policy, const char *prompt,
    planned_action **actions, int *n_actions, char **raw_out, char **err_out,
    const char *attachments, int *truncated_out, int output_tokens) {
    return plan_with(llm, build_catalog_prompt(tools,skills,policy,planner_uses_native_tools(llm)), prompt, actions,
        n_actions, raw_out, err_out, attachments, truncated_out, output_tokens, tools, policy, NULL, NULL, 0, NULL);
}

int planner_plan_turn_diagnostic(llm *llm, const struct tool_registry *tools,
    struct skill_registry *skills, struct policy_engine *policy, const char *prompt,
    planned_action **actions, int *n_actions, char **raw_out, char **err_out,
    const char *attachments, int *truncated_out, int output_tokens, int *native_end_out, planner_model_diagnostic *diagnostic, int require_completion, char **completion_out) {
    return plan_with(llm,build_catalog_prompt(tools,skills,policy,planner_uses_native_tools(llm)),prompt,actions,n_actions,
        raw_out,err_out,attachments,truncated_out,output_tokens,tools,policy,native_end_out,diagnostic,require_completion,completion_out);
}

int planner_plan_turn(llm *llm, const struct tool_registry *tools,
    struct skill_registry *skills, struct policy_engine *policy, const char *prompt,
    planned_action **actions, int *n_actions, char **raw_out, char **err_out,
    const char *attachments, int *truncated_out, int output_tokens, int *native_end_out) {
    return planner_plan_turn_diagnostic(llm,tools,skills,policy,prompt,actions,n_actions,
        raw_out,err_out,attachments,truncated_out,output_tokens,native_end_out,NULL,0,NULL);
}
