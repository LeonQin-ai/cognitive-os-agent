# Tasks 106/107 and structured-flow CI

The supplied exports show task 106 stopping after two roughly 303-second
WinHTTP receive timeouts, and task 107 reaching request 201 before HTTP 429
AccountQuotaExceeded. Both used a recorded work budget of 1,000,000 rounds.
The latter executed repeated diagnostic/test commands, not just prose turns.
An executable tool call alone does not establish useful task progress.

The proposed 128-round default was withdrawn at the user's request. Negative
settings retain unlimited compatibility behavior. Completion instead follows
actual native tool blocks, complete transport responses, structured acceptance
receipts and stop review; legacy responses retain their explicit contract.

CI run 41268354560 exposed fixtures still relying on the removed prose
classifiers: scoped source-reading cases now supply explicit full_reads
acceptance through HTTP. A missing source EOF remains FAILED. Legacy greeting
fixtures now supply complete_task, and review/call accounting follows the
structured protocol. No successful source/tool assertions were removed.

Local HTTP validation: 14 source-reading scenarios and 20 native/legacy
completion-review scenarios passed. Full C validation and final CI are still
required before treating issues 235/236 as resolved. Existing uncommitted
test_plan_cycle.c changes were present before this work and are preserved.

No paid model call was made during this initial validation. The reported
quota-exhausted request is not retried to obtain a green result. The exports do
not contain a complete usage ledger, so their total token cost is unknown.
