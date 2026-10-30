# Planner rejection must not be reported as a tool failure

`reasoning_run_ex` previously sent every failed stage to the same tool-recovery
instruction: diagnose a failed path/command, edit and rerun it. Planner rejection
has no dispatched action. The instruction incorrectly invented a tool failure
and conflicted with the latched structured-protocol recovery contract.

After existing timeout/rate-limit/truncation handling and the consecutive-failure
breaker, failed rounds with no executable plan now receive protocol recovery:
submit the next valid action, or a valid complete/blocked declaration. They do
not enter the actual-tool-error instruction. Actual failed actions keep their
existing recovery path. No keyword heuristics, guessed tool actions, relaxed
argument parsing or larger retry budgets were introduced.

`test_legacy_tool_envelopes_e2e.py` now checks that the first malformed generic
response receives a no-actions-executed protocol instruction, not a fictitious
tool-error instruction. Its response sequence remains fixed, independent of the
prompt. The 26 existing envelope scenarios passed, including task-97 ordering,
actual write/readback, no-progress failure, invalid batches and native cases.
Both protocols' 20 completion-review cases and the task recovery suite passed.
The latter now explicitly selects legacy mode, matching its wire fixtures.

A configured actual DeepSeek/legacy paged-source task on the changed Runtime
passed all source-marker, actual HTML write, readback and final-answer checks:
10 requests, 14 tools, zero model failures, 44.386 seconds; 103,415 input + 6,293
output = 109,708 tokens. Raw report and model-created HTML are retained under
`build/live-legacy-protocol-recovery-fix.*`. This successful run had no protocol
failure, so it does not independently prove recovery causality; the deterministic
scenario exercises that branch. Original GLM deployment acceptance remains
unverified and #232 stays open until matching evidence is obtained.
