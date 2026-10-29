# ASAN gate: wait for the committed history record

CI run 40837553832 failed in `test_embedded_tool_completion_e2e.py`, at the
immediate history assertion for `native/stuck`. It was an AssertionError, not an
AddressSanitizer memory report. All earlier completion-review cases and the first
six native markup-recovery cases passed.

`finalize_task` publishes the terminal task status, then the worker invokes the
completion callback outside the scheduler lock. That callback writes the final
run to tasklog. A failed run is deliberately absent from successful conversation
turns, so its result becomes visible in `/v1/chat/history.runs` only after this
callback commits it. Reading history once immediately after seeing FAILED races
that commit. The scheduler's existing idle barrier includes completion callbacks;
seeing a terminal status alone is not that barrier.

The test now waits for the exact run ID, terminal status and complete output in
history before asserting contents. It uses the original 15-second task deadline,
without retrying the task/model or extending the deadline. It also explicitly
checks that the failed case did not create a successful conversation turn. A
missing or incorrect final record still fails with task/history diagnostics.

The controlled scheduler callback test now asserts the intermediate state:
terminal status is visible while the completion callback is held and `completed`
is still false. The idle wait remains blocked until the callback is released.
This validates the lifecycle ordering without adding delays to production code.

Local checks: the complete 20-case markup suite passes with ASAN, native/stuck
passes 20 additional ASAN repetitions, and the targeted scheduler suite passes
337 checks. The original full ASAN regression group also passed locally before
the synchronization change, consistent with a timing-dependent assertion.

The ASAN regression group now has individually named steps using the existing
log/annotation wrapper. Failures preserve script output and fixture log artifacts,
so the public gate identifies the exact script and traceback. Assertions,
provider/iteration budgets, sanitizer checks and exit codes remain enforced.
No runtime terminal-state semantics were changed for this CI fix.
