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
      "JSON-RPC echo 服务器，验证 MCP 通路", "本地", 1, "node build/mock_mcp_server.js", "",
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
                 (m->command && *m->command) ? "stdio" : "http",
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
