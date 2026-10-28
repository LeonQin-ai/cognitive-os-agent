#pragma once
struct runtime_ctx;
struct task;
/* Uses a scheduler-owned, tool-free model request. Config is frozen at submit. */
int memory_summary_run(struct runtime_ctx *ctx, struct task *task, char **answer);
/* Serialized API helpers: caller authenticates. Returned JSON is caller-owned. */
char *memory_summary_get(struct runtime_ctx *ctx);
int memory_summary_start(struct runtime_ctx *ctx, char **json);
int memory_summary_cancel(struct runtime_ctx *ctx);
