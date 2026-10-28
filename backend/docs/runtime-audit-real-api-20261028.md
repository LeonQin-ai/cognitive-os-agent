# Runtime finalization / Flow / IM audit, 2026-10-28

All paid runs used the configured `deepseek-v4.1-flash` through private Cognitive
OS Runtime instances. Production state/tasks and user edits were untouched.
Fixture inputs were provided by the harness; the Agent performed the requested
reads, copying and verification. No assistant-generated deliverable was inserted.

| Run | Outcome | Model requests | Input tokens | Output including reasoning | Total |
|---|---|---:|---:|---:|---:|
| Source read with mandatory EOF and optional fragment | DONE, both random proofs present | 6 | 45,906 | 2,661 | 48,567 |
| IM before context de-duplication, 5 rounds / 3 members | DONE, 15 replies | 15 | 130,905 | 7,426 | 138,331 |
| Flow budget reproduction | FAILED despite exact copied bytes | 6 | 27,664 | 1,244 | 28,908 |
| Flow intermediate reserve (last action unhandled) | FAILED, retained as failed cost | 7 | 28,086 | 1,068 | 29,154 |
| Flow after finalization reserve fix | DONE, 2 successful nodes and persisted record | 11 | 51,714 | 2,431 | 54,145 |
| IM after de-duplication, same round/member/task setup | DONE, 15 replies | 15 | 120,759 | 9,201 | 129,960 |
| **All runs, including failures** | **4 successful / 2 failed test runs** | **60** | **405,034** | **24,031** | **429,065** |

The two unsuccessful Flow runs cost 58,062 tokens. There were no provider
request failures in these runs; a Runtime task failure is a separate metric.
No rerun replaces earlier failure rows. The successful copy was independently
compared byte for byte, and its dependent node read the actual result.

## Code findings and regression boundaries

- #227: native Flow prose classification, stale completion review after new
  work, and the configured-round/final-review boundary. Nine positive and two
  negative native delivery cases check literal results, complete-only reserve,
  no over-budget tool execution, and blocked dependencies. Existing source,
  failure-recovery and cancellation fixtures remain required.
- #228: a rejected before-run hook reported success, leaked the temporary
  provider into the lane, and nested reasoning cleared the caller's HTTP
  cancellation binding. Fourteen direct interface assertions cover both exits.
- #229: IM replayed transcript-bearing synthetic prompts through recent chat
  history. Native and legacy discussion fixtures check reciprocal turns,
  consensus, failure, cancellation, superseding messages, deletion, persistence,
  and ordinary-chat history after lane reuse.

Model-written text and timings vary across real runs. The IM token delta is an
observation; the deterministic no-duplicate-history assertion is the regression
contract. Normal native completion uses tool calls and `complete_task`, not new
phrase lists. A dishonest completion declaration is not universally detectable;
actual file/failure criteria and bounded model delivery review remain necessary.

#224 remains independent and open: these runs do not prove the earlier costly
UNKNOWN request's unrecorded provider failure has been fixed. Nor do these tests
certify the still-missing 100-Agent authorization, crash consistency or 500-turn
memory acceptance matrix.
