# Rolling compaction follow-up, 2026-10-28

Rechecking unfinished memory acceptance found a reproducible Runtime defect
(#231). Each compaction sent only the next eight discarded turns to the model,
then replaced the old summary. Earlier constraints and unfinished work therefore
vanished from compressor input after the first compaction. The new prompt
supplies the previous summary plus the new turns and explicitly preserves still
valid constraints, decisions and pending work. An allocated temporary summary
on the successful path is now released.

## Deterministic evidence

`cognitive-os-agent-test --rolling-compaction` runs 500 actual reasoning turns
and at least 60 compactions. Its test compressor copies only the fixed synthetic
constraint/todo markers present in its input; it cannot reconstruct missing
markers. Both must remain in the final summary and planner input.

- Before: 4 passed / 4 failed. The second compaction no longer received earlier
  constraints; neither the final summary nor final planner input retained them.
- After: 8 passed / 0 failed. Both markers survive all compactions.
- Full local C suite: 4,582 passed / 0 failed. The focused test is also part of
  the full suite and `--memory-lifecycle`, so all CI platforms exercise it.

This establishes Runtime input continuity, not 500 real-model turns, semantic
summary correctness, tool-side-effect constraint compliance or crash recovery.

## Real API evidence: not a passing acceptance result

Both attempts used the configured `deepseek-v4.1-flash` through isolated Cognitive
OS Runtime processes. Cross-session memory was disabled to prevent episode
retrieval from hiding the missing-summary defect. No production task was touched.

| Attempt | Result | Reported input | Reported output including reasoning | Reported total |
|---|---|---:|---:|---:|
| 25-real-chat plan, stopped before first compaction | 11 DONE tasks, one cancelled; inefficient task retained | 63,238 | 34,566 | 97,804 |
| Targeted compaction: 15 synthetic user-only inputs, planned ten real tasks | First task returned ACK and performed first compaction, then efficiency gate stopped the batch | 11,041 | 33,548 | 44,589 |
| **Combined observed usage** | **Neither acceptance run passed** | **74,279** | **68,114** | **142,393** |

The first attempt had one native request reach 32,768 output tokens with HTTP
200, `stop_reason=length`, no visible content and incomplete native tool
arguments. Its Runtime retry produced ACK: two planner requests, 10,903 input
and 32,871 output tokens, 96.557 seconds. The initial version of the new harness
checked answer accuracy only, so this row incorrectly passed its local answer
check. The batch was stopped externally, retaining the row and cancellation.
The harness now has an immediate model-failure/output-efficiency gate, separate
from answer accuracy, rather than treating correct answers as efficient runs.

The targeted attempt stopped at its first task under this gate. The same native
length failure occurred, followed by a successful Runtime retry and the first
actual compressor call (three usage-ledger calls total). It took 111.155 seconds
and produced a 1,711-byte summary. The second compression and final recall were
not reached. No further paid retry was attempted.

The first attempt has twelve usage-ledger calls plus a cancelled request whose
provider usage was unavailable. The second has three recorded calls. Thus the
combined 142,393 tokens are **observed usage**, not a complete claim about cost
including the unreported cancelled request. The two anomalous task rows alone
report 88,363 tokens; their per-failed-request token splits are not available.
Reports remain at `build/live-rolling-compaction.json`,
`build/live-rolling-compaction-focused.json`, with separate logs and the captured
native failure at `build/rolling-compaction-live-failure-task.json`.

#224 now has repeated same-class length/incomplete-native-call evidence in
different contexts. This narrows the observed failure mode, but does not prove
the underlying generation cause or provide a Runtime fix. #231's code regression
is fixed; its real-model acceptance remains incomplete. Both issues must remain
open pending their remaining verification.

Other unfinished memory contracts remain in `memory-validation.md`: labeled
semantic recall, authenticated 100-Agent isolation, acknowledged-write crash
consistency, 500 real turns with independent constraint checks, and code-repair
memory ablation. Default lexical hashing and MMU scope ranking cannot stand in
for semantic retrieval and authorization tests.
