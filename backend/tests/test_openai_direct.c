/* test_openai_direct.c — debug the openai adapter against a running mock-llm-server. */
#include "cagent/llm/llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    ca_llm *llm = ca_llm_create("openai", "http://localhost:9000", NULL, "m");
    if (!llm) { printf("create failed\n"); return 1; }
    ca_llm_message msgs[2] = {{"system", "plan"}, {"user", "创建 note.txt 写入内容为 hello-e2e"}};
    ca_llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    ca_llm_response resp;
    memset(&resp, 0, sizeof(resp));
    int rc = ca_llm_chat(llm, &req, &resp);
    printf("rc=%d\n", rc);
    if (resp.error) printf("error=%s\n", resp.error);
    if (resp.content) printf("content=%s\n", resp.content);
    free(resp.error);
    free(resp.content);
    ca_llm_destroy(llm);
    return 0;
}
