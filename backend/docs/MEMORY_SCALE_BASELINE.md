# Local vector memory scale baseline

`tests/bench_memory_scale.c` measures the in-process `vectorstore` used by the
retrieval layer. It inserts deterministic records, runs 21 identical top-five
queries, reports nearest-rank P50/P95, process peak RSS, and whether the
intended record appears in the top five. It makes no LLM or network calls.

Build with `make build/bench-memory-scale` on Linux, or `./build.sh memory-bench`
on Windows. Run `./build/bench-memory-scale 10000` and
`./build/bench-memory-scale 100000` (append `.exe` on Windows). The 100K build
is deliberately excluded from CI because the current insertion path scans all
existing IDs and takes minutes; CI uses 1K as a functional smoke check.

## Initial measurement

Windows, Zig 0.14.1, `-O2`, 2026-11-03. These are single-process observations,
not performance targets or cross-platform estimates.

| Records | Build | Retrieval P50 | Retrieval P95 | Peak RSS | Exact query top-five | Ambiguous query top-five |
| ---: | ---: | ---: | ---: | ---: | :---: | :---: |
| 10,000 | 557 ms | 4.11 ms | 4.62 ms | 23.5 MB | yes | no |
| 100,000 | 149,733 ms | 38.57 ms | 40.73 ms | 151.9 MB | yes | no |

The broad query `Project storage engine SQLite` did not find the unique SQLite
record in the top five at either scale. Including the exact project identifier
did. This shows a retrieval quality limit as well as the insertion cost.

## Agent-path wiring and live A/B

The production Runtime now owns a thread-safe `memory_runtime` bridge over the
canonical Markdown records and rebuildable vector mirror. Completed tasks with
successful tool receipts pass through the promotion gate. `reasoning_run_ex`,
Flow nodes and orchestrator fallback recall through Context MMU when
`context.mmu.enabled=1` (the default); `0` retains the legacy retrieval path
for comparison. Private records use exact `session/<id>` scope matching;
`global` and `user/global` remain explicitly shareable.

`tests/test_live_memory_ab.py` is an opt-in paid regression. It seeds legacy
and V3 stores once using a successful real Agent task, removes conversation
history, clones identical state per trial, and interleaves MMU off/on queries.
It records status, actual provider usage, calls, failures and elapsed time
without writing credentials to the report.

Windows, `deepseek-v4.1-flash`, 2026-11-03, three trials per arm:

| Path | Success | Input tokens | Output tokens | Total tokens | Model failures |
| --- | ---: | ---: | ---: | ---: | ---: |
| Legacy retrieval | 3/3 | 17,046 | 1,425 | 18,471 | 0 |
| Context MMU | 3/3 | 17,169 | 879 | 18,048 | 0 |

Both paths succeeded in one model call per trial. MMU used 423 fewer total
tokens (2.29%) in this small fact-recall sample. This is evidence for this
fixed task only, not a general task-success claim; larger multi-task samples
and confidence intervals remain future benchmark work.

The first live run also exposed a double-truncation defect: MMU produced a
budgeted L1/L2 item, then the legacy renderer cut every item to 300 bytes,
discarding the verified result after a long task description. MMU items now
render directly under the cold-tier total budget. The anti-pollution prompt
also distinguishes factual recall from claims that an operation was performed:
facts may use scoped memory, while file changes and other delivery claims still
require tool evidence from the current run.
