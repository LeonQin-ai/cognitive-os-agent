/* catalog.c — curated catalogs for the console plaza.
 * Free/cheap model presets (all OpenAI-compatible except noted) and an MCP
 * server marketplace. Entries that need a local process or a provider key are
 * flagged so the UI can guide the user. */
#include "cognitive-os-agent/infra/catalog.h"
#include "cognitive-os-agent/infra/util.h"
#include "cognitive-os-agent/os/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct model_entry {
    const char *id;
    const char *name;
    const char *provider;   /* backend provider: openai | anthropic */
    const char *base_url;
    const char *model;
    const char *key_hint;   /* "" = no key needed */
    const char *note;
    const char *signup_url; /* free-key signup page ("" = none) */
    int local;              /* 1 = local runtime, startable via /v1/local/start */
} model_entry;

static const model_entry MODELS[] = {
    { "ollama",     "Ollama（本地免费）",       "openai",    "http://localhost:11434/v1",
      "llama3",     "", "本地运行 `ollama serve`，无需 key", "", 1 },
    { "llamacpp",   "llama.cpp / vLLM（本地免费）", "openai", "http://localhost:8080/v1",
      "",           "", "本地已加载模型的 HTTP 端点，无需 key", "", 1 },
    { "groq",       "Groq（免费额度）",         "openai",    "https://api.groq.com/openai/v1",
      "llama-3.3-70b-versatile", "console.groq.com 注册免费 key", "LPU 推理，免费额度大",
      "https://console.groq.com/keys", 0 },
    { "openrouter", "OpenRouter（免费模型）",   "openai",    "https://openrouter.ai/api/v1",
      "meta-llama/llama-3.3-70b-instruct:free", "openrouter.ai 免费 key", "带 :free 后缀的模型免费",
      "https://openrouter.ai/settings/keys", 0 },
    { "gemini",     "Gemini（免费额度）",       "openai",    "https://generativelanguage.googleapis.com/v1beta/openai",
      "gemini-2.0-flash", "aistudio.google.com 免费 key", "OpenAI 兼容端点",
      "https://aistudio.google.com/apikey", 0 },
    { "mistral",    "Mistral（免费额度）",      "openai",    "https://api.mistral.ai/v1",
      "mistral-small-latest", "console.mistral.ai 免费 key", "La Plateforme 免费档",
      "https://console.mistral.ai/api-keys", 0 },
    { "deepseek",   "DeepSeek（低价）",         "openai",    "https://api.deepseek.com/v1",
      "deepseek-chat", "platform.deepseek.com", "接近免费，OpenAI 兼容",
      "https://platform.deepseek.com/api_keys", 0 },
    { "glm",        "GLM 智谱（Coding Plan）",  "anthropic", "https://open.bigmodel.cn/api/anthropic",
      "glm-4.6",    "bigmodel.cn 的 GLM Coding Plan key", "Anthropic 兼容端点，Coding Plan 套餐直接用",
      "https://open.bigmodel.cn/usercenter/apikeys", 0 },
    { "ark",        "豆包·火山方舟（Coding Plan）", "anthropic", "https://ark.cn-beijing.volces.com/api/coding",
      "glm-5.3-flash", "console.volcengine.com 的 Coding Plan API key", "Coding Plan 专用端点；可选模型：glm-5.3 / glm-5.3-flash / doubao-seed-2.0-lite / doubao-seed-2.1-turbo / deepseek-v4-flash / deepseek-v4-pro / kimi-k2.7-code / minimax-m3",
      "https://console.volcengine.com/ark", 0 },
};
#define N_MODELS (int)(sizeof(MODELS) / sizeof(MODELS[0]))

/* Copy src into dst as a JSON string body: escape ", \ and control chars
 * (prompt-skill templates contain newlines — raw bytes would produce
 * invalid JSON that the browser can't parse). */
static void json_esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (const char *p = src; *p; p++) {
        char tmp[8];
        const char *rep = NULL;
        switch (*p) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)*p);
                    rep = tmp;
                }
                break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= cap - 1) break;
            memcpy(dst + o, rep, rl);
            o += rl;
        } else {
            if (o + 1 >= cap - 1) break;
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

typedef struct mcp_entry {
    const char *id;
    const char *name;
    const char *url;
    const char *description;
    const char *category;
    int needs_local;
    const char *key_hint;
    const char *repo;   /* GitHub repo ("" = not from GitHub) */
    const char *command; /* stdio transport executable ("" = http entry) */
    const char *args;    /* stdio space-separated args ("" = none) */
    int needs_token;     /* 1 = hosted server requiring a bearer token (PAT etc.) */
} mcp_entry;

static const mcp_entry MCPS[] = {
    { "mock-echo", "Mock Echo（演示）", "http://127.0.0.1:9000",
      "JSON-RPC echo 服务器，验证 MCP 通路", "本地", 1, "node tools/mock_mcp_server.js", "",
      "", "", 0 },
    { "filesystem", "FileSystem（本地文件）", "",
      "读取/写入本地目录（filesystem MCP，stdio）", "本地", 1, "npx @modelcontextprotocol/server-filesystem", "",
      "npx", "-y @modelcontextprotocol/server-filesystem .", 0 },
    { "sqlite",     "SQLite（本地数据库）", "",
      "SQLite 数据库读写（sqlite MCP，stdio）", "本地", 1, "npx @modelcontextprotocol/server-sqlite", "",
      "npx", "-y @modelcontextprotocol/server-sqlite", 0 },
    { "playwright", "Playwright（浏览器自动化）", "",
      "浏览器导航/点击/填表（playwright MCP，stdio）", "本地", 1, "npx @playwright/mcp", "",
      "npx", "-y @playwright/mcp", 0 },
    /* ---- GitHub 热门 MCP 应用（modelcontextprotocol/servers 官方仓库等） ---- */
    { "github",     "GitHub（官方托管）", "https://api.githubcopilot.com/mcp/",
      "GitHub 仓库/Issue/PR 操作（官方 MCP）", "GitHub", 0, "GitHub Personal Access Token",
      "https://github.com/github/github-mcp-server",
      "", "", 1 },
    { "fetch",      "Fetch（网页抓取）", "",
      "抓取 URL 并转换为可读 Markdown（stdio）", "GitHub", 1, "npx @modelcontextprotocol/server-fetch",
      "https://github.com/modelcontextprotocol/servers/tree/main/src/fetch",
      "npx", "-y @modelcontextprotocol/server-fetch", 0 },
    { "memory",     "Memory（知识图谱记忆）", "",
      "基于知识图谱的持久化记忆（stdio）", "GitHub", 1, "npx @modelcontextprotocol/server-memory",
      "https://github.com/modelcontextprotocol/servers/tree/main/src/memory",
      "npx", "-y @modelcontextprotocol/server-memory", 0 },
    { "everything", "Everything（全工具测试）", "",
      "覆盖所有工具类型的 MCP 服务器，用于测试客户端（stdio）", "GitHub", 1, "npx @modelcontextprotocol/server-everything",
      "https://github.com/modelcontextprotocol/servers/tree/main/src/everything",
      "npx", "-y @modelcontextprotocol/server-everything", 0 },
    { "sequential-thinking", "Sequential Thinking（分步推理）", "",
      "逐步推理工具，帮助模型解决复杂问题（stdio）", "GitHub", 1, "npx @modelcontextprotocol/server-sequential-thinking",
      "https://github.com/modelcontextprotocol/servers/tree/main/src/sequentialthinking",
      "npx", "-y @modelcontextprotocol/server-sequential-thinking", 0 },
    { "time",       "Time（时间/时区）", "",
      "当前时间与时区转换工具（stdio）", "GitHub", 1, "npx @modelcontextprotocol/server-time",
      "https://github.com/modelcontextprotocol/servers/tree/main/src/time",
      "npx", "-y @modelcontextprotocol/server-time", 0 },
    { "puppeteer",  "Puppeteer（浏览器自动化）", "",
      "无头 Chrome 浏览器自动化（官方 puppeteer MCP，stdio）", "GitHub", 1, "npx @modelcontextprotocol/server-puppeteer",
      "https://github.com/modelcontextprotocol/servers/tree/main/src/puppeteer",
      "npx", "-y @modelcontextprotocol/server-puppeteer", 0 },
    { "notion",     "Notion（知识库）", "https://mcp.notion.so/mcp",
      "Notion 工作区读写（官方 Notion MCP）", "GitHub", 0, "Notion Integration Token",
      "https://github.com/makenotion/notion-mcp-server",
      "", "", 1 },
    { "browserbase", "Browserbase（云端浏览器）", "",
      "云端无头浏览器自动化/网页抓取（browserbase MCP，stdio）", "GitHub", 1,
      "BROWSERBASE_API_KEY + BROWSERBASE_PROJECT_ID",
      "https://github.com/browserbase/mcp-server-browserbase",
      "npx", "-y @browserbasehq/mcp", 0 },
    /* ---- 参考入口（合集/工具仓库，非单个 server，不可一键连接） ---- */
    { "official-servers", "MCP 官方 Server 合集", "",
      "modelcontextprotocol/servers 官方参考实现合集（参考入口）", "参考", 0, "",
      "https://github.com/modelcontextprotocol/servers", "", "", 0 },
    { "awesome-mcp", "Awesome MCP Servers", "",
      "社区精选 MCP Server 大全（参考入口）", "参考", 0, "",
      "https://github.com/punkpeye/awesome-mcp-servers", "", "", 0 },
    { "mcp-cli", "mcp-cli（MCP 命令行调试）", "",
      "命令行调用/调试 MCP Server 的工具（参考入口）", "参考", 0, "",
      "https://github.com/wong2/mcp-cli", "", "", 0 },
};
#define N_MCPS (int)(sizeof(MCPS) / sizeof(MCPS[0]))

char *coa_catalog_models_json(void) {
    char *out = coa_strdup("[");
    for (int i = 0; i < N_MODELS; i++) {
        char buf[1600];
        const model_entry *m = &MODELS[i];
        char id[64], name[128], prov[32], base[256], model[128], kh[256], note[600], su[256];
        json_esc(id, sizeof(id), m->id);
        json_esc(name, sizeof(name), m->name);
        json_esc(prov, sizeof(prov), m->provider);
        json_esc(base, sizeof(base), m->base_url);
        json_esc(model, sizeof(model), m->model);
        json_esc(kh, sizeof(kh), m->key_hint ? m->key_hint : "");
        json_esc(note, sizeof(note), m->note ? m->note : "");
        json_esc(su, sizeof(su), m->signup_url ? m->signup_url : "");
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"provider\":\"%s\","
                 "\"base_url\":\"%s\",\"model\":\"%s\",\"key_hint\":\"%s\",\"note\":\"%s\","
                 "\"signup_url\":\"%s\",\"local\":%s}",
                 i ? "," : "", id, name, prov, base,
                 model, kh, note,
                 su,
                 m->local ? "true" : "false");
        size_t cur = strlen(out), blen = strlen(buf);
        char *no = realloc(out, cur + blen + 2);
        if (!no) break;
        out = no;
        memcpy(out + cur, buf, blen + 1);
    }
    size_t cur = strlen(out);
    char *no = realloc(out, cur + 2);
    if (no) { out = no; memcpy(out + cur, "]", 2); }
    return out;
}

char *coa_catalog_mcp_json(void) {
    char *out = coa_strdup("[");
    for (int i = 0; i < N_MCPS; i++) {
        char buf[1400];
        const mcp_entry *m = &MCPS[i];
        char id[64], name[128], url[256], desc[600], cat[64], kh[256], repo[300],
             cmd[256], args[512];
        json_esc(id, sizeof(id), m->id);
        json_esc(name, sizeof(name), m->name);
        json_esc(url, sizeof(url), m->url ? m->url : "");
        json_esc(desc, sizeof(desc), m->description ? m->description : "");
        json_esc(cat, sizeof(cat), m->category ? m->category : "");
        json_esc(kh, sizeof(kh), m->key_hint ? m->key_hint : "");
        json_esc(repo, sizeof(repo), m->repo ? m->repo : "");
        json_esc(cmd, sizeof(cmd), m->command ? m->command : "");
        json_esc(args, sizeof(args), m->args ? m->args : "");
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\",\"description\":\"%s\","
                 "\"category\":\"%s\",\"needs_local\":%s,\"key_hint\":\"%s\",\"repo\":\"%s\","
                 "\"transport\":\"%s\",\"command\":\"%s\",\"args\":\"%s\",\"needs_token\":%s}",
                 i ? "," : "", id, name, url, desc,
                 cat, m->needs_local ? "true" : "false", kh, repo,
                 (m->command && *m->command) ? "stdio"
                   : ((m->url && *m->url) ? "http" : "reference"),
                 cmd, args,
                 m->needs_token ? "true" : "false");
        size_t cur = strlen(out), blen = strlen(buf);
        char *no = realloc(out, cur + blen + 2);
        if (!no) break;
        out = no;
        memcpy(out + cur, buf, blen + 1);
    }
    size_t cur = strlen(out);
    char *no = realloc(out, cur + 2);
    if (no) { out = no; memcpy(out + cur, "]", 2); }
    return out;
}

/* ---------------- Skills plaza ----------------
 * Runnable cross-platform skills (shell/echo + python) installable via
 * /v1/skills/install, plus "reference" entries pointing at the skill/prompt
 * frameworks the plaza is curated from (fabric, skillhub, cursorrules). */
static const catalog_skill SKILLS[] = {
    /* ---- 可安装运行（shell：cmd.exe 与 /bin/sh 均可用） ---- */
    { "greet",      "Greet（参数化问候）",
      "按 {{who}} 参数输出问候语", "shell",
      "echo hello {{who}}", "{\"who\":\"world\"}", "" },
    { "banner",     "Banner（标题横幅）",
      "输出 === {{title}} === 横幅行", "shell",
      "echo === {{title}} ===", "{\"title\":\"demo\"}", "" },
    /* ---- 可安装运行（python：需本机 python 命令） ---- */
    { "py_now",     "Now（当前时间戳）",
      "输出 ISO 格式当前时间", "python",
      "import datetime; print(datetime.datetime.now().isoformat())", "", "" },
    { "py_sysinfo", "SysInfo（系统信息）",
      "输出 OS 与 Python 版本信息", "python",
      "import platform, sys; print(platform.system(), platform.release()); print(sys.version.split()[0])",
      "", "" },
    { "py_calc",    "Calc（数值求和）",
      "计算 {{a}} + {{b}} 并输出", "python",
      "print({{a}} + {{b}})", "{\"a\":1,\"b\":2}", "" },
    { "py_uuid",    "UUID（随机标识）",
      "生成一个 UUIDv4 随机标识", "python",
      "import uuid; print(uuid.uuid4())", "", "" },
    { "py_sha256",  "SHA256（文本指纹）",
      "计算 {{text}} 的 SHA-256 摘要", "python",
      "import hashlib; print(hashlib.sha256('{{text}}'.encode()).hexdigest())",
      "{\"text\":\"cognitive-os-agent\"}", "" },
    /* ---- 可安装运行（prompt：经当前 LLM 后端执行，源自 fabric patterns） ---- */
    { "summarize",  "Summarize（内容摘要）",
      "将给定内容浓缩为一段要点摘要（源自 fabric patterns）", "prompt",
      "Summarize the content below in one clear paragraph, followed by 3 key bullet points.\n\nContent:\n{{content}}",
      "{\"content\":\"Cognitive OS is a C-native runtime for autonomous AI agents. It treats the LLM as an accelerator rather than the operating system, providing memory, context management, transactions, policy and multi-agent orchestration.\"}",
      "https://github.com/danielmiessler/fabric" },
    { "extract_wisdom", "Extract Wisdom（要点提炼）",
      "从文本中提炼核心观点/洞见/事实（源自 fabric patterns）", "prompt",
      "Extract the main ideas, key facts and notable quotes from the content below. Reply as short bullet lists.\n\nContent:\n{{content}}",
      "{\"content\":\"LLM is the accelerator, not the OS. A cognitive runtime must own memory, context and policy, and treat model calls as replaceable compute.\"}",
      "https://github.com/danielmiessler/fabric" },
    { "explain_code", "Explain Code（代码讲解）",
      "逐步解释给定代码的功能（源自 fabric patterns）", "prompt",
      "Explain what the following code does, step by step, in simple terms. Then list any bugs or risks you notice.\n\nCode:\n{{code}}",
      "{\"code\":\"int f(int n){return n<2?n:f(n-1)+f(n-2);}\"}",
      "https://github.com/danielmiessler/fabric" },
    { "improve_writing", "Improve Writing（润色改写）",
      "把给定文本改写得更清晰专业（源自 fabric patterns）", "prompt",
      "Rewrite the text below so it is clear, concise and professional. Keep the original language and meaning.\n\nText:\n{{text}}",
      "{\"text\":\"this runtime very good, it make agent run fast and safe\"}",
      "https://github.com/danielmiessler/fabric" },
    /* ---- 参考：技能/提示词框架仓库（不可直接运行） ---- */
    { "fabric",     "Fabric（提示词技能框架）",
      "danielmiessler/fabric 模块化 LLM 技能（patterns）精选", "reference", "", "",
      "https://github.com/danielmiessler/fabric" },
    { "anthropics-skills", "Anthropic Skills（官方技能库）",
      "anthropics/skills 官方 Claude 技能合集（docx/pdf/前端设计等）", "reference", "", "",
      "https://github.com/anthropics/skills" },
    { "awesome-cursorrules", "Awesome CursorRules",
      "PatrickJS/awesome-cursorrules 规则/技能精选列表", "reference", "", "",
      "https://github.com/PatrickJS/awesome-cursorrules" },
};
#define N_SKILLS (int)(sizeof(SKILLS) / sizeof(SKILLS[0]))

/* ---------------- GitHub remote skills ----------------
 * Real skill files in live upstream repos, fetched over HTTPS when the user
 * clicks install. Each entry carries a direct raw.githubusercontent.com URL
 * plus a ghproxy mirror (direct raw access is unreliable in some regions).
 * skillhub (thinkany-ai) was removed: the repo was deleted upstream. */
static const catalog_remote_skill REMOTE_SKILLS[] = {
    /* ---- danielmiessler/fabric — data/patterns/<name>/system.md ---- */
    { "fabric", "fab_summarize", "Fabric: Summarize（内容摘要）",
      "fabric patterns 原版摘要技能（安装时从仓库拉取 system.md）",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/summarize/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/summarize/system.md" },
    { "fabric", "fab_extract_wisdom", "Fabric: Extract Wisdom（要点提炼）",
      "fabric 最出名的 pattern：提炼观点/洞见/事实/引用",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/extract_wisdom/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/extract_wisdom/system.md" },
    { "fabric", "fab_improve_writing", "Fabric: Improve Writing（润色）",
      "fabric patterns 原版写作润色技能",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/improve_writing/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/improve_writing/system.md" },
    { "fabric", "fab_analyze_claims", "Fabric: Analyze Claims（论断核查）",
      "逐条分析论断的真值与依据",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/analyze_claims/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/analyze_claims/system.md" },
    { "fabric", "fab_create_quiz", "Fabric: Create Quiz（出题）",
      "根据给定内容生成测验题",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/create_quiz/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/create_quiz/system.md" },
    { "fabric", "fab_rate_content", "Fabric: Rate Content（内容评分）",
      "多维打分并给出改进建议",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/rate_content/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/rate_content/system.md" },
    { "fabric", "fab_explain_code", "Fabric: Explain Code（代码讲解）",
      "逐步解释代码功能与风险",
      "https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/explain_code/system.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/danielmiessler/fabric/main/data/patterns/explain_code/system.md" },
    /* ---- anthropics/skills — skills/<name>/SKILL.md（官方 Claude 技能库） ---- */
    { "anthropics", "ant_docx", "Anthropic: Docx（Word 文档）",
      "创建/编辑/分析 Word 文档的官方技能（SKILL.md 原文）",
      "https://raw.githubusercontent.com/anthropics/skills/main/skills/docx/SKILL.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/anthropics/skills/main/skills/docx/SKILL.md" },
    { "anthropics", "ant_pdf", "Anthropic: Pdf（PDF 处理）",
      "PDF 表单填写/文本提取/拆分合并的官方技能",
      "https://raw.githubusercontent.com/anthropics/skills/main/skills/pdf/SKILL.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/anthropics/skills/main/skills/pdf/SKILL.md" },
    { "anthropics", "ant_frontend_design", "Anthropic: Frontend Design（前端设计）",
      "官方前端设计品味技能",
      "https://raw.githubusercontent.com/anthropics/skills/main/skills/frontend-design/SKILL.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/anthropics/skills/main/skills/frontend-design/SKILL.md" },
    { "anthropics", "ant_brand_guidelines", "Anthropic: Brand Guidelines（品牌规范）",
      "官方品牌规范应用技能",
      "https://raw.githubusercontent.com/anthropics/skills/main/skills/brand-guidelines/SKILL.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/anthropics/skills/main/skills/brand-guidelines/SKILL.md" },
    { "anthropics", "ant_internal_comms", "Anthropic: Internal Comms（内部通讯）",
      "官方内部通讯写作技能",
      "https://raw.githubusercontent.com/anthropics/skills/main/skills/internal-comms/SKILL.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/anthropics/skills/main/skills/internal-comms/SKILL.md" },
    { "anthropics", "ant_canvas_design", "Anthropic: Canvas Design（视觉设计）",
      "官方画布/海报视觉设计技能",
      "https://raw.githubusercontent.com/anthropics/skills/main/skills/canvas-design/SKILL.md",
      "https://ghproxy.net/https://raw.githubusercontent.com/anthropics/skills/main/skills/canvas-design/SKILL.md" },
    /* ---- PatrickJS/awesome-cursorrules — rules/<name>.mdc ---- */
    { "cursorrules", "cr_ts_vuejs", "CursorRules: TypeScript + Vue.js",
      "awesome-cursorrules 精选规则原文（.mdc）",
      "https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/typescript-vuejs-cursorrules-prompt-file.mdc",
      "https://ghproxy.net/https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/typescript-vuejs-cursorrules-prompt-file.mdc" },
    { "cursorrules", "cr_angular_ts", "CursorRules: Angular + TypeScript",
      "Angular 开发规范规则原文",
      "https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/angular-typescript-cursorrules-prompt-file.mdc",
      "https://ghproxy.net/https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/angular-typescript-cursorrules-prompt-file.mdc" },
    { "cursorrules", "cr_cpp", "CursorRules: C++ 编程规范",
      "C++ 编码指南规则原文",
      "https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/cpp-programming-guidelines-cursorrules-prompt-file.mdc",
      "https://ghproxy.net/https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/cpp-programming-guidelines-cursorrules-prompt-file.mdc" },
    { "cursorrules", "cr_code_guidelines", "CursorRules: 通用代码规范",
      "通用代码质量规则原文",
      "https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/code-guidelines-cursorrules-prompt-file.mdc",
      "https://ghproxy.net/https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/code-guidelines-cursorrules-prompt-file.mdc" },
    { "cursorrules", "cr_anti_overeng", "CursorRules: 反过度工程",
      "约束 AI 生成代码不过度设计的规则原文",
      "https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/anti-overengineering.mdc",
      "https://ghproxy.net/https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/anti-overengineering.mdc" },
    { "cursorrules", "cr_android_compose", "CursorRules: Android Jetpack Compose",
      "Compose 开发规范规则原文",
      "https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/android-jetpack-compose-cursorrules-prompt-file.mdc",
      "https://ghproxy.net/https://raw.githubusercontent.com/PatrickJS/awesome-cursorrules/main/rules/android-jetpack-compose-cursorrules-prompt-file.mdc" },
};
#define N_REMOTE (int)(sizeof(REMOTE_SKILLS) / sizeof(REMOTE_SKILLS[0]))

int coa_catalog_remote_skill_count(void) { return N_REMOTE; }

const catalog_remote_skill *coa_catalog_remote_skill_at(int i) {
    return (i >= 0 && i < N_REMOTE) ? &REMOTE_SKILLS[i] : NULL;
}

const catalog_remote_skill *coa_catalog_remote_skill_find(const char *repo,
                                                          const char *id) {
    for (int i = 0; i < N_REMOTE; i++) {
        const catalog_remote_skill *e = &REMOTE_SKILLS[i];
        if (repo && strcmp(e->repo, repo) != 0) continue;
        if (id && strcmp(e->id, id) != 0) continue;
        return e;
    }
    return NULL;
}

char *coa_catalog_remote_skills_json(void) {
    char *out = coa_strdup("[");
    for (int i = 0; i < N_REMOTE; i++) {
        char buf[900];
        const catalog_remote_skill *e = &REMOTE_SKILLS[i];
        char repo[64], id[64], name[128], desc[600], raw[400], fb[400];
        json_esc(repo, sizeof(repo), e->repo);
        json_esc(id, sizeof(id), e->id);
        json_esc(name, sizeof(name), e->name);
        json_esc(desc, sizeof(desc), e->description);
        json_esc(raw, sizeof(raw), e->raw_url);
        json_esc(fb, sizeof(fb), e->fallback_url ? e->fallback_url : "");
        snprintf(buf, sizeof(buf),
                 "%s{\"repo\":\"%s\",\"id\":\"%s\",\"name\":\"%s\","
                 "\"description\":\"%s\",\"raw_url\":\"%s\",\"fallback_url\":\"%s\"}",
                 i ? "," : "", repo, id, name, desc, raw, fb);
        size_t cur = strlen(out), blen = strlen(buf);
        char *no = realloc(out, cur + blen + 2);
        if (!no) break;
        out = no;
        memcpy(out + cur, buf, blen + 1);
    }
    size_t cur = strlen(out);
    char *no = realloc(out, cur + 2);
    if (no) { out = no; memcpy(out + cur, "]", 2); }
    return out;
}

/* Split "https://host/path" into malloc'd base ("https://host") and path
 * ("/path"). Returns 0 ok, -1 malformed. */
static int split_url(const char *url, char **base_out, char **path_out) {
    const char *scheme = strstr(url, "://");
    if (!scheme) return -1;
    const char *host = scheme + 3;
    const char *slash = strchr(host, '/');
    if (!slash || slash == host) return -1;
    size_t blen = (size_t)(slash - url);
    *base_out = (char *)malloc(blen + 1);
    *path_out = coa_strdup(slash);
    if (!*base_out || !*path_out) { free(*base_out); free(*path_out); return -1; }
    memcpy(*base_out, url, blen);
    (*base_out)[blen] = '\0';
    return 0;
}

#define REMOTE_SKILL_MAX 65536

char *coa_catalog_remote_skill_fetch(const catalog_remote_skill *e) {
    if (!e || !e->raw_url || !*e->raw_url) return NULL;
    /* one shot: try direct raw URL, then the ghproxy mirror */
    const char *urls[2] = { e->raw_url, (e->fallback_url && *e->fallback_url) ? e->fallback_url : NULL };
    for (int u = 0; u < 2; u++) {
        if (!urls[u]) continue;
        char *base = NULL, *path = NULL;
        if (split_url(urls[u], &base, &path) != 0) continue;
        coa_http_response *r = coa_http_get(base, path, NULL, 10000);
        free(base);
        free(path);
        if (r && r->status == 200 && r->body && r->body_len > 0) {
            size_t n = r->body_len;
            if (n > REMOTE_SKILL_MAX) n = REMOTE_SKILL_MAX;
            char *text = (char *)malloc(n + 1);
            if (text) {
                memcpy(text, r->body, n);
                text[n] = '\0';
            }
            coa_http_response_free(r);
            return text;
        }
        if (r) coa_http_response_free(r);
    }
    return NULL;
}

int coa_catalog_skill_count(void) { return N_SKILLS; }

const catalog_skill *coa_catalog_skill_at(int i) {
    return (i >= 0 && i < N_SKILLS) ? &SKILLS[i] : NULL;
}

char *coa_catalog_skills_json(void) {
    char *out = coa_strdup("[");
    for (int i = 0; i < N_SKILLS; i++) {
        char buf[1800];
        const catalog_skill *s = &SKILLS[i];
        char id[64], name[128], desc[600], kind[32], body[1200], ta[512], src[300];
        json_esc(id, sizeof(id), s->id);
        json_esc(name, sizeof(name), s->name);
        json_esc(desc, sizeof(desc), s->description ? s->description : "");
        json_esc(kind, sizeof(kind), s->kind ? s->kind : "");
        json_esc(body, sizeof(body), s->body ? s->body : "");
        json_esc(ta, sizeof(ta), s->test_args ? s->test_args : "");
        json_esc(src, sizeof(src), s->source ? s->source : "");
        const char *type = strcmp(s->kind, "reference") == 0 ? "reference" : "skill";
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"description\":\"%s\","
                 "\"kind\":\"%s\",\"body\":\"%s\",\"test_args\":\"%s\","
                 "\"source\":\"%s\",\"type\":\"%s\"}",
                 i ? "," : "", id, name, desc,
                 kind, body,
                 ta, src, type);
        size_t cur = strlen(out), blen = strlen(buf);
        char *no = realloc(out, cur + blen + 2);
        if (!no) break;
        out = no;
        memcpy(out + cur, buf, blen + 1);
    }
    size_t cur = strlen(out);
    char *no = realloc(out, cur + 2);
    if (no) { out = no; memcpy(out + cur, "]", 2); }
    return out;
}
