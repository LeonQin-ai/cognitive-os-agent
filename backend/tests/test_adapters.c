/* test_adapters.c — verify the OpenAI and Anthropic adapters (chat + SSE stream)
 * against the bundled mock-llm-server. Requires mock-llm-server on :9000. */
#include "cagent/llm/llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond) do { if (cond) printf("  ok   %s\n", #cond); \
                         else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void check_chat(const char *provider) {
    char url[64];
    snprintf(url, sizeof(url), "http://localhost:9000");
    ca_llm *llm = ca_llm_create(provider, url, provider[0] == 'a' ? "test-key" : NULL, "test-model");
    if (!llm) { printf("  FAIL create %s\n", provider); g_fail++; return; }
    ca_llm_message msgs[2] = {{"system", "plan"}, {"user", "创建 adapter.txt 写入内容为 hi"}};
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    ca_llm_response resp;
    memset(&resp, 0, sizeof(resp));
    int rc = ca_llm_chat(llm, &req, &resp);
    CHECK(rc == 0);
    if (resp.error) { printf("    error: %s\n", resp.error); }
    CHECK(resp.content != NULL);
    if (resp.content) CHECK(strstr(resp.content, "file_write") != NULL);
    free(resp.content);
    free(resp.error);
    ca_llm_destroy(llm);
}

static void on_delta(const char *d, void *ud) {
    (void)d;
    int *n = (int *)ud;
    (*n)++;
}

static void check_stream(const char *provider) {
    char url[64];
    snprintf(url, sizeof(url), "http://localhost:9000");
    ca_llm *llm = ca_llm_create(provider, url, provider[0] == 'a' ? "test-key" : NULL, "test-model");
    if (!llm) { printf("  FAIL create %s\n", provider); g_fail++; return; }
    ca_llm_message msgs[2] = {{"system", "plan"}, {"user", "创建 adapter.txt 写入内容为 hi"}};
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    int n = 0;
    int rc = ca_llm_stream(llm, &req, on_delta, &n);
    ca_llm_destroy(llm);
    printf("    stream(provider=%s) rc=%d deltas=%d\n", provider, rc, n);
    CHECK(rc == 0);
    CHECK(n >= 1);
}

int main(void) {
    printf("adapter tests (mock-llm-server :9000)\n");
    check_chat("openai");
    check_chat("anthropic");
    check_stream("openai");
    check_stream("anthropic");
    printf(g_fail == 0 ? "ADAPTER PASS\n" : "ADAPTER FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
