# Legacy narration recovery after cached observations (#232)

Latest report: task 107 alternates malformed tagged output, valid tools and repeated prose announcements. This is not evidence that plain text should be accepted as completion, nor that a model alone caused the failure.

## Confirmed code defect

A valid legacy action plan clears protocol recovery. Cached reads do not invalidate the already-reviewed work stage. If the model then emits prose while the required HTML is absent, the artifact gate previously consumed three artifact rejections without restoring the structured-decision requirement. Repeated commentary could therefore exhaust artifact acceptance before a subsequent valid write. Separately, the legacy intent nudge allowed final plain text even when reviewed recovery required actions or complete_task.

The missing-artifact recovery now restores the explicit legacy decision contract. Narration is handled as rejected protocol output, not a new artifact delivery claim. Valid tools clear that recovery state as before. The intent nudge supplies the action and complete_task envelopes and no longer requests final plain text. Native dispatch, actual artifact receipts, failure bounds, total round budget and legacy keyword lists are unchanged.

## Deterministic regression

`tests/test_legacy_tool_envelopes_e2e.py` adds two fixed response sequences, independent of prompt contents:

- `trace_narration_recovery`: successful source reads, candidate narration, cached reads, three further narrations, actual HTML write/readback and complete_task. The previous main binary fails at request 6 with no HTML. The fixed binary reaches DONE at request 10, recording two protocol failures, the actual artifact and final answer. Live and persisted planner Trace verify the restored review flag. Cached steps are retained in Trace while unique execution metrics count four operations.
- `trace_107`: malformed tagged replies twice, valid read, three narrations, another real read, narration and actual HTML delivery. DONE at request 13, six tool operations and four recorded protocol failures. This is a structural replay of the report, not an exact replay of unavailable original model responses.

Existing always-narrating, invalid batch, missing/stale artifact and native completion negatives remain in the same suite; none may report false DONE. The new tests inherit that suite's existing CI jobs.

## Configured real API

A single isolated DeepSeek legacy paged-source comparison run delivered and read back a 7,037-byte HTML report with random source markers, leaving both source fixtures intact: DONE, 8 requests, 14 tools, 0 model failures, 36.310 seconds. Runtime accounted 79,920 input + 5,896 output (including reasoning) = 85,816 tokens. Provider breakdown: 4,893 visible completion + 1,003 reasoning. No monetary price is configured or inferred. Reports: ignored `build/live-narration-recovery.json`, `.html`, `.log`.

This real run did not encounter a protocol failure; the deterministic before/after regression proves the recovery branch. The reporter's original GLM endpoint and original source directories are unavailable locally, so this does not guarantee all providers or claim exact reproduction of task 107.
