/* test_stream.c — verify SSE streaming via the openai adapter. */
#include "cognitive-os-agent/llm/llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void on_delta(const char *d, void *ud) {
    int *n = (int *)ud;
    printf("  delta[%d]: %s\n", (*n)++, d);
}

int main(void) {
    coa_llm *llm = coa_llm_create("openai", "http://localhost:9000", NULL, "m");
    if (!llm) { printf("create failed\n"); return 1; }
    coa_llm_message msgs[2] = {{"system", "plan"}, {"user", "创建 stream.txt 写入内容为 hello-stream"}};
    coa_llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    int n = 0;
    int rc = coa_llm_stream(llm, &req, on_delta, &n);
    printf("stream rc=%d deltas=%d\n", rc, n);
    coa_llm_destroy(llm);
    return rc == 0 && n >= 1 ? 0 : 1;
}
