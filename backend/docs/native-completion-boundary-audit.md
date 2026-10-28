# Native completion boundaries: false failures and stale reviews

Code review found native Flow completions still routed through legacy prose
classifiers (`I will`, `Let me`, future-tense Chinese phrases). Legitimate
translation/examples/advice could therefore be marked unfinished. Native
completion now uses tool calls / `complete_task`, actual tool/file checks and
bounded delivery review. Prose classifiers remain only in explicit legacy mode.

The review flag also remained set after review resumed actual work. New
observations now invalidate that old review, without reopening it on cache hits.

## Real API reproduction

A configured `deepseek-v4.1-flash` automatic two-node Flow copied a fixture via
`file_write(copy_from=...)`. The independent harness verified exact bytes and
the persisted Flow record. Nevertheless the first node hit its five-round test
budget during verification/final review and failed, blocking the second node.
Two diagnostic executions are retained, not replaced with a successful score:

| Attempt | Status | Requests | Input | Output including reasoning | Total |
|---|---|---:|---:|---:|---:|
| Original budget failure | FAILED | 6 | 27,664 | 1,244 | 28,908 |
| Candidate-only reserve, last tool round still unhandled | FAILED | 7 | 28,086 | 1,068 | 29,154 |
| Completed fix | DONE, both nodes ok | 11 | 51,714 | 2,431 | 54,145 |

Root cause: a final native declaration required another model review even at
the last configured round; a successful last tool round instead fell through
to a partial summary. Reserve exactly one finalization request in either case.
It may return a completion/blocked declaration but cannot execute tools; no
additional synthesis request follows an unsuccessful reserve. The work-round
limit and all failure/file checks remain enforced. This is not an unlimited
budget increase or a relabel of partial output as success.

## Regression

`test_native_flow_literal_results.py` covers nine successful literal/read/budget
boundaries, malformed completion rejection and a requested over-budget write
that must not execute; failed dependents remain blocked. Completion-review
fixtures also check that missing work requested during the reserve stays FAILED.
The existing Flow suite keeps native/legacy source evidence, full upstream
result retrieval, independent branches, failure and cancellation checks.

Raw local evidence: `build/runtime-audit-flow-before-{report,task}.json`,
`build/runtime-audit-flow-reserve-before-{report,task}.json`, and
`build/flow-progress-live-{report,task}.json`. No production task was modified.
