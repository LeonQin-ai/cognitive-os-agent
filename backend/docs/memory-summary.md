# Viewable memory summaries

Open **Memory → Generate memory summary**. The Runtime snapshots the working
memory and long-term facts already visible on this page and submits a separate,
read-only model task. It does not summarize unrelated chat transcripts or expose
another reasoning lane's current summary. No tools are supplied to the model;
even an unexpected tool response is rejected without execution. Original facts,
working memory and chat history are unchanged.

The page shows the complete generated text, model, generation time, source
counts and reported input/output tokens, and offers full-text copy. Output tokens
include any reported reasoning tokens. Missing usage is labeled unavailable,
not zero. Generation uses the active configured model snapshot, has one model
request and no automatic retry. It can be cancelled without blocking other API
requests. Duplicate active submissions are rejected.

The last successful summary survives failed requests, incomplete model output,
unexpected tools, cancellation and restarts. Successful results are appended to
`<state>/memory/summaries.jsonl`; an incomplete tail is ignored and later writes
start on a fresh line. `<state>/memory/summary-job.json` identifies the latest
job. A completed result not yet viewed can be recovered from the task journal
after restart. Viewing or copying saved content does not call the model. This
is restart recovery, not a claim of fsync-based power-loss durability.

## API

- `POST /v1/memory/summary` returns `202 {task_id, started_ms}`. Task ID zero
  is valid. Empty memory returns 422; source snapshots over 128 KiB return 413,
  with no truncation or provider request. Configured-model and queue limits apply.
- `GET /v1/memory/summary` returns optional `job` and last successful `summary`.
  `job.status` follows the scheduler. A persistence failure is reported separately
  as `save_error`, retaining the prior saved result. Generated text is in
  `summary.text`, with scope, source hash/counts, model, timestamps and usage.
- `DELETE /v1/memory/summary` requests cancellation of this summary job only.
- All three use the Runtime's existing authentication. Generic task resume
  cannot replay memory data as an executable chat request; generate a fresh
  summary from Memory instead.

## Validation, 2026-10-28

`tests/test_memory_summary_e2e.py` exercises the real HTTP, scheduler and provider
adapter path with a deterministic provider: authentication, task zero, complete
long output, usage, immutable source/history, unexpected tool rejection,
HTTP/output/empty-response failures, cancellation, duplicate admission,
nonblocking health reads, restart including unviewed completion, incomplete-tail
recovery, and empty/oversized source rejection. The browser suite
`tests/test_memory_summary_ui.cjs` checks the visible result, full copy, metadata,
cancel/failure preservation, retry and stale-panel responses. Both are in CI.

Opt-in actual API:

```sh
python tests/test_live_memory_summary.py --config /private/cognitive-os-agent.json
```

One configured `deepseek-v4.1-flash` request through a private Runtime passed:
150 input + 278 output = **428 tokens**, no model failure, no tools, 7.425 seconds.
The actual generated summary retained random project/code facts and a directory
constraint. Restart/read produced identical saved content and zero new model
calls. Raw local report: `build/live-memory-summary.json`. No production task or
credential configuration was changed. This validates this read-only feature;
it does not resolve #224/#231's native-loop and repeated-compaction acceptance.
