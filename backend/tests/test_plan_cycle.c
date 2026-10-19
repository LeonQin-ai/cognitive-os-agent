/* Regression: formatting-only JSON changes must not let a repeated write
 * plan consume the full planning budget. */
#include "action/tools.h"
#include "cognition/reasoning.h"
#include "llm/llm.h"
#include "os/os_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cycle_fixture {
    int calls;
} cycle_fixture;

static char *copy_text(const char *text) {
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}

static int cycle_chat(llm *self, const llm_request *req, llm_response *resp) {
    (void)req;
    cycle_fixture *fixture = self ? self->impl : NULL;
    if (!fixture) return -1;
    fixture->calls++;
    if (fixture->calls <= 3) {
        const char *plan = fixture->calls % 2
            ? "[{\"tool\":\"file_write\",\"args\":{\"path\":\"repeat.txt\",\"content\":\"same\"}}]"
            : "[{\"tool\":\"file_write\",\"args\":{\"content\":\"same\",\"path\":\"repeat.txt\"}}]";
        resp->content = copy_text(plan);
    } else {
        resp->content = copy_text("检测到重复写入后已停止，任务未完成。");
    }
    return resp->content ? 0 : -1;
}

typedef struct recovery_fixture {
    int calls;
    int failed_actions;
} recovery_fixture;

typedef struct repeated_read_fixture {
    int calls;
} repeated_read_fixture;

static int repeated_read_chat(llm *self, const llm_request *req, llm_response *resp) {
    (void)req;
    repeated_read_fixture *fixture = self ? self->impl : NULL;
    if (!fixture) return -1;
    fixture->calls++;
    const char *answer = fixture->calls <= 6
        ? "[{\"tool\":\"file_read\",\"args\":{\"path\":\"same.txt\"}}]"
        : fixture->calls == 7
            ? "[{\"tool\":\"file_read\",\"args\":{\"path\":\"new.txt\"}}]"
            : "复核完成：已继续读取另一份证据。";
    resp->content = copy_text(answer);
    return resp->content ? 0 : -1;
}

static int recovery_chat(llm *self, const llm_request *req, llm_response *resp) {
    (void)req;
    recovery_fixture *fixture = self ? self->impl : NULL;
    if (!fixture) return -1;
    fixture->calls++;
    const char *answer = NULL;
    switch (fixture->calls) {
    case 1: answer = "[{\"tool\":\"file_read\",\"args\":{\"path\":\"same.txt\"}}]"; break;
    case 2: answer = "[{\"tool\":\"fixture_failure\",\"args\":{}}]"; break;
    case 3: answer = "I'll investigate the failed check now."; break;
    case 4: answer = "[{\"tool\":\"fixture_failure\",\"args\":{}}]"; break;
    default: answer = "失败原因已核对并继续执行成功"; break;
    }
    resp->content = copy_text(answer);
    return resp->content ? 0 : -1;
}

static tool_result *fixture_failure(const tool *self, const tool_ctx *ctx, const char *args) {
    (void)ctx;
    (void)args;
    recovery_fixture *fixture = self ? self->ud : NULL;
    if (fixture && fixture->failed_actions++ > 0)
        return tool_result_new(1, "fixture command succeeded on retry");
    return tool_result_new(0, "fixture failure: command exited with status 1");
}

static int check(int condition, const char *label) {
    if (condition) {
        printf("PASS %s\n", label);
        return 0;
    }
    printf("FAIL %s\n", label);
    return 1;
}

int main(void) {
    static const llm_vtable vtable = {.chat = cycle_chat};
    cycle_fixture fixture = {0};
    llm provider = {.vt = &vtable, .provider = "fixture", .model = "fixture", .impl = &fixture};
    const char *workspace = "build/test-plan-cycle-work";
    char file_path[256];
    snprintf(file_path, sizeof(file_path), "%s/repeat.txt", workspace);
    fs_mkdirs(workspace);
    fs_remove(file_path);

    tool_registry *registry = tool_registry_new();
    if (!registry || tool_register(registry, tool_file_write()) != 0) {
        tool_registry_free(registry);
        return 2;
    }

    reasoning_config cfg = {0};
    cfg.llm = &provider;
    cfg.tools = registry;
    cfg.workspace = workspace;
    cfg.max_rounds = -1;
    reasoning *engine = reasoning_new(&cfg);
    if (!engine) {
        tool_registry_free(registry);
        return 2;
    }

    char *answer = NULL;
    int rc = reasoning_run(engine, "创建 repeat.txt 并写入 same", &answer);
    char *written = fs_read_file(file_path);
    int failures = 0;
    failures += check(rc != 0, "stalled run is marked incomplete");
    failures += check(answer && strstr(answer, "任务未完成") != NULL,
                      "user receives an incomplete result");
    failures += check(fixture.calls == 4,
                      "three repeated plans stop before the unlimited round budget");
    failures += check(written && strcmp(written, "same") == 0,
                      "the first successful write remains available");

    free(written);
    free(answer);
    reasoning_free(engine);
    tool_registry_free(registry);
    fs_remove(file_path);
    fs_remove(workspace);

    /* Re-reading the same source may be a legitimate recovery step in a
     * multi-step review. Allow the planner's duplicate-read recovery window
     * to run instead of aborting after only two skipped read rounds. */
    const char *read_workspace = "build/test-plan-repeat-read";
    char same_read_path[256], new_read_path[256];
    snprintf(same_read_path, sizeof(same_read_path), "%s/same.txt", read_workspace);
    snprintf(new_read_path, sizeof(new_read_path), "%s/new.txt", read_workspace);
    fs_mkdirs(read_workspace);
    fs_write_file(same_read_path, "same evidence", 13);
    fs_write_file(new_read_path, "additional evidence", 19);
    repeated_read_fixture repeated = {0};
    llm repeated_provider = {
        .vt = &(const llm_vtable){.chat = repeated_read_chat},
        .provider = "fixture", .model = "fixture", .impl = &repeated
    };
    tool_registry *read_registry = tool_registry_new();
    if (!read_registry || tool_register(read_registry, tool_file_read()) != 0) {
        tool_registry_free(read_registry);
        fs_remove(same_read_path); fs_remove(new_read_path); fs_remove(read_workspace);
        return 2;
    }
    reasoning_config read_cfg = {0};
    read_cfg.llm = &repeated_provider;
    read_cfg.tools = read_registry;
    read_cfg.workspace = read_workspace;
    read_cfg.max_rounds = -1;
    reasoning *read_engine = reasoning_new(&read_cfg);
    if (!read_engine) {
        tool_registry_free(read_registry);
        fs_remove(same_read_path); fs_remove(new_read_path); fs_remove(read_workspace);
        return 2;
    }
    char *read_answer = NULL;
    int read_rc = reasoning_run(read_engine, "复核源码并根据证据汇报", &read_answer);
    failures += check(read_rc == 0 && read_answer && strstr(read_answer, "复核完成"),
                      "duplicate-read recovery can continue to a later valid evidence step");
    failures += check(repeated.calls == 8,
                      "planner does not abort after two repeated-read rounds");
    free(read_answer);
    reasoning_free(read_engine);
    tool_registry_free(read_registry);
    fs_remove(same_read_path); fs_remove(new_read_path); fs_remove(read_workspace);

    /* A prior successful read must not make a later failed action's
     * work-in-progress narration look like a completed answer. */
    const char *recovery_workspace = "build/test-plan-recovery";
    char same_path[256], new_path[256];
    snprintf(same_path, sizeof(same_path), "%s/same.txt", recovery_workspace);
    snprintf(new_path, sizeof(new_path), "%s/new.txt", recovery_workspace);
    fs_mkdirs(recovery_workspace);
    fs_write_file(same_path, "old evidence", 12);
    fs_write_file(new_path, "new evidence", 12);
    recovery_fixture recovery = {0};
    llm recovery_provider = {
        .vt = &(const llm_vtable){.chat = recovery_chat},
        .provider = "fixture", .model = "fixture", .impl = &recovery
    };
    tool_registry *recovery_registry = tool_registry_new();
    tool failure_tool = {
        .name = "fixture_failure", .description = "deterministic failure fixture",
        .execute = fixture_failure, .ud = &recovery
    };
    if (!recovery_registry || tool_register(recovery_registry, tool_file_read()) != 0 ||
        tool_register(recovery_registry, &failure_tool) != 0) {
        tool_registry_free(recovery_registry);
        fs_remove(same_path); fs_remove(new_path); fs_remove(recovery_workspace);
        return 2;
    }
    reasoning_config recovery_cfg = {0};
    recovery_cfg.llm = &recovery_provider;
    recovery_cfg.tools = recovery_registry;
    recovery_cfg.workspace = recovery_workspace;
    recovery_cfg.max_rounds = -1;
    reasoning *recovery_engine = reasoning_new(&recovery_cfg);
    if (!recovery_engine) {
        tool_registry_free(recovery_registry);
        fs_remove(same_path); fs_remove(new_path); fs_remove(recovery_workspace);
        return 2;
    }
    char *recovery_answer = NULL;
    int recovery_rc = reasoning_run(recovery_engine, "检查两份状态并汇报结论", &recovery_answer);
    failures += check(recovery_rc == 0 && recovery_answer &&
                      strstr(recovery_answer, "失败原因已核对并继续执行成功"),
                      "planner resumes after narration following a failed action");
    failures += check(recovery.failed_actions == 2 && recovery.calls == 5,
                      "recovery retries the failed action and continues after it succeeds");
    free(recovery_answer);
    reasoning_free(recovery_engine);
    tool_registry_free(recovery_registry);
    fs_remove(same_path); fs_remove(new_path); fs_remove(recovery_workspace);
    printf("plan cycle regression: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
