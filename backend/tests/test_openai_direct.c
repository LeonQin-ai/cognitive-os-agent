/* test_openai_direct.c — debug the openai adapter against a running mock-llm-server. */
#include "cognitive-os-agent/llm/llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    coa_llm *llm = coa_llm_create("openai", "http://localhost:9000", NULL, "m");
    if (!llm) { printf("create failed\n"); return 1; }
    coa_llm_message msgs[2] = {{"system", "plan"}, {"user", "创建 note.txt 写入内容为 hello-e2e"}};
    coa_llm_request req;
    memset(&req, 0, sizeof(req));
    req.messages = msgs;
    req.num_messages = 2;
    req.temperature = 0.2;
    req.max_tokens = 1024;
    coa_llm_response resp;
    memset(&resp, 0, sizeof(resp));
    int rc = coa_llm_chat(llm, &req, &resp);
    printf("rc=%d\n", rc);
    if (resp.error) printf("error=%s\n", resp.error);
    if (resp.content) printf("content=%s\n", resp.content);
    free(resp.error);
    free(resp.content);
    coa_llm_destroy(llm);
    return 0;
}
