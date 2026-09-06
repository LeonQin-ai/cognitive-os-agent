/* catalog.c — curated catalogs for the console plaza.
 * Free/cheap model presets (all OpenAI-compatible except noted) and an MCP
 * server marketplace. Entries that need a local process or a provider key are
 * flagged so the UI can guide the user. */
#include "cognitive-os-agent/infra/catalog.h"
#include "cognitive-os-agent/infra/util.h"

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
        /* JSON-escape the note (simple: strip double quotes) */
        char note[600];
        snprintf(note, sizeof(note), "%s", m->note ? m->note : "");
        for (char *p = note; *p; p++) if (*p == '"') *p = '\'';
        char kh[256];
        snprintf(kh, sizeof(kh), "%s", m->key_hint ? m->key_hint : "");
        for (char *p = kh; *p; p++) if (*p == '"') *p = '\'';
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"provider\":\"%s\","
                 "\"base_url\":\"%s\",\"model\":\"%s\",\"key_hint\":\"%s\",\"note\":\"%s\","
                 "\"signup_url\":\"%s\",\"local\":%s}",
                 i ? "," : "", m->id, m->name, m->provider, m->base_url,
                 m->model, kh, note,
                 m->signup_url ? m->signup_url : "",
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
        char desc[600], kh[256];
        snprintf(desc, sizeof(desc), "%s", m->description ? m->description : "");
        for (char *p = desc; *p; p++) if (*p == '"') *p = '\'';
        snprintf(kh, sizeof(kh), "%s", m->key_hint ? m->key_hint : "");
        for (char *p = kh; *p; p++) if (*p == '"') *p = '\'';
        char repo[300];
        snprintf(repo, sizeof(repo), "%s", m->repo ? m->repo : "");
        for (char *p = repo; *p; p++) if (*p == '"') *p = '\'';
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\",\"description\":\"%s\","
                 "\"category\":\"%s\",\"needs_local\":%s,\"key_hint\":\"%s\",\"repo\":\"%s\","
                 "\"transport\":\"%s\",\"command\":\"%s\",\"args\":\"%s\",\"needs_token\":%s}",
                 i ? "," : "", m->id, m->name, m->url, desc,
                 m->category, m->needs_local ? "true" : "false", kh, repo,
                 (m->command && *m->command) ? "stdio"
                   : ((m->url && *m->url) ? "http" : "reference"),
                 m->command ? m->command : "", m->args ? m->args : "",
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
    { "skillhub",   "SkillHub（社区技能底座）",
      "thinkany-ai/skillhub 开源技能社区底座", "reference", "", "",
      "https://github.com/thinkany-ai/skillhub" },
    { "awesome-cursorrules", "Awesome CursorRules",
      "PatrickJS/awesome-cursorrules 规则/技能精选列表", "reference", "", "",
      "https://github.com/PatrickJS/awesome-cursorrules" },
};
#define N_SKILLS (int)(sizeof(SKILLS) / sizeof(SKILLS[0]))

int coa_catalog_skill_count(void) { return N_SKILLS; }

const catalog_skill *coa_catalog_skill_at(int i) {
    return (i >= 0 && i < N_SKILLS) ? &SKILLS[i] : NULL;
}

char *coa_catalog_skills_json(void) {
    char *out = coa_strdup("[");
    for (int i = 0; i < N_SKILLS; i++) {
        char buf[1200];
        const catalog_skill *s = &SKILLS[i];
        char desc[600], src[300];
        snprintf(desc, sizeof(desc), "%s", s->description ? s->description : "");
        for (char *p = desc; *p; p++) if (*p == '"') *p = '\'';
        snprintf(src, sizeof(src), "%s", s->source ? s->source : "");
        for (char *p = src; *p; p++) if (*p == '"') *p = '\'';
        const char *type = strcmp(s->kind, "reference") == 0 ? "reference" : "skill";
        snprintf(buf, sizeof(buf),
                 "%s{\"id\":\"%s\",\"name\":\"%s\",\"description\":\"%s\","
                 "\"kind\":\"%s\",\"body\":\"%s\",\"test_args\":\"%s\","
                 "\"source\":\"%s\",\"type\":\"%s\"}",
                 i ? "," : "", s->id, s->name, desc,
                 s->kind, s->body ? s->body : "",
                 s->test_args ? s->test_args : "", src, type);
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
