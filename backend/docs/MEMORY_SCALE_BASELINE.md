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

This harness does **not** measure Context MMU's effect on Agent success or
provider token use. `ctx_mmu_recall` is exercised in unit tests, but no call
from `reasoning.c` or `memory/service.c` currently invokes it in the task
path. A same-task MMU on/off trial would therefore compare identical task
paths and cannot establish an MMU benefit yet. Wire the MMU into the task
context path, then run fixed-task API A/B trials with usage and outcome capture
before closing issue #248.
