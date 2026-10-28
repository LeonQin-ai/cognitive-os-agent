# Memory lifecycle validation

Run `python tests/run_memory_baseline.py` after building the test target. The
runner uses a fresh temporary working directory, writes an assertion report and
the complete log into `build/memory-baseline.{json,log}`, and returns nonzero on
failure. It invokes the actual C interfaces rather than asking a model to grade
itself. The constituent tests also remain in the full unit suite.

## Interpret the architecture correctly

Working/episodic/semantic/procedural are Memory Service types. L0/L1/L2 are
abstract/overview/detail representations in Context MMU, not three durability
classes. HOT/WARM/COLD describe residency. A scope ranking factor is not an
authorization boundary. Lossy summaries cannot establish 99% constraint retention.

## Current evidence and missing acceptance tests

| Scenario | Deterministic coverage | Remaining evidence |
|---|---|---|
| M01/M04/M05 | Memory Service, KV, episode duplicate/update tests | Natural-language extraction, latest valid fact use |
| M02/M03 | Vector and retrieval regression checks | 1K/10K/100K labeled corpus, semantic Recall@5/Precision@5 |
| M06 | Record metadata and promotion rules | Explicit source-authority/conflict benchmark; no universal last-write-wins claim |
| M07 | Memory persistence and session tests | Independent process restart plus new session using remembered facts |
| M08 | Forget/lifecycle/record checks | End-to-end deletion across every derived cache and post-restart retrieval |
| M09/M10 | Context budgets, promotion, MMU residency tests | Constraint survival through repeated materialization and eviction |
| M11 | Not certified by this baseline | 100 concurrent identities, private/shared authorization and acknowledged-write ledger |
| M12 | Not certified by graceful reopen | Kill writer at persistence boundaries; compare acknowledged operations to recovery |
| M13/M14 | Incomplete-run memory gate | Missing-fact abstention and malicious-memory tool-side-effect checks |
| M15 | Context shaping/caps/MMU checks | 500–1000 turns, repeated compaction, source reloading and independent constraint checker |

Assertion counts are not task success rates. This baseline does not measure
retrieval latency percentiles, semantic quality, throughput or real-model benefit.

## Next benchmark contracts

Keep exact-key and semantic-query samples separate, as well as cold/reopened and
warm-cache samples. Label relevant record IDs before running queries. Report
Recall@5 with the number of answerable queries, Precision@5 with its actual
returned-result denominator, missing results and individual failures. Report
P50/P95/P99 only from per-operation samples, not total suite elapsed time.

Real Agent runs must use isolated Runtime state and the configured API, new
session IDs, independently checked facts/artifacts and measured usage. Compare
memory disabled, working-only, and supported persistent-memory configurations
against identical seeded tasks; do not relabel those as L0/L1/L2 ablations. Include
failure/retry tokens and report the number of repetitions. Until these runs exist,
no claim of improved task success or reduced tokens is justified.

## Opt-in real API regression

Run `python tests/test_live_memory_lifecycle.py --config <private-config-path>`.
It starts private Runtime processes using the configured provider, generates
unguessable fictional facts, and requires exact answers from new session IDs.
After a completed seed, it stops the process and copies the same persisted state
into independent memory-on and memory-off branches before either query executes.
The report includes task status, measured usage, model failures, latency and
answers; credentials are never included. `--extended` additionally tests an
explicit superseding fact across another restart and unknown-fact abstention.

This tests episode persistence and actual model use, not a structured fact
extraction API. The sharing switch is currently global: branches must use
separate Runtime states, never concurrent toggle requests against production.
These results must not be described as multi-Agent authorization isolation,
500-turn retention, mid-write crash consistency or code-repair benefit.

The real API script fails its exit status for either accuracy or efficiency
failures by default. `--allow-inefficient` explicitly permits accuracy-only exit
status while retaining failed efficiency rows. Timeout/transport exceptions
retain the last observed task usage and diagnostic rather than dropping the
failed case. `--abstention-only --repetitions 1` performs one bounded diagnostic
without repeating successful memory cases.

## Confirmed lifecycle regressions found on 2026-10-28

The initial 276 assertions did not exercise resident record edits or deletion
followed by index rebuild. New tests reproduced stale HOT bodies, stale WARM
metadata, and retained vector mirrors of deleted records. See
`memory-resident-update-regression.md` (#225) and
`memory-deleted-index-regression.md` (#226). They are actual defects, not claims
of broad memory correctness based on a passing baseline.

## Reopened unfinished work: rolling compaction (#231)

The 500-turn deterministic Runtime test exposed another defect: each new
compaction replaced the previous summary without supplying it to the compressor.
It now receives both the previous summary and newly dropped turns. The test
compressor copies only constraint/todo markers actually present in its input;
after at least 60 compactions, both markers must still reach the final planner
input. This tests input continuity, not a model's semantic constraint compliance.
The success-path temporary summary is also freed.

Run the focused C test with `cognitive-os-agent-test --rolling-compaction`.
It is included in the full suite and memory baseline. The opt-in
`python tests/test_live_rolling_compaction.py --config <private-config-path>`
performs 25 real native chat tasks in a private Runtime with episode sharing
disabled. It checks exact random facts after two actual compactions and records
the global usage ledger, including compaction requests. There is no automatic
retry after failure or after reaching the test's aggregate token budget.
`--preload-user-inputs` instead seeds 15 synthetic user-only inputs (empty
assistant fields) and targets the same boundaries with ten real chat tasks.
Model failures and excessive output fail immediately even if the answer is
correct. Unreported usage on cancellation must not be counted as zero cost.

Outstanding acceptance work remains explicit: a labeled semantic retrieval
corpus, authenticated 100-Agent private/shared authorization, acknowledged-write
crash consistency, 500 real-model turns with independent tool-side-effect
constraint checks, and matched code-repair tasks with/without memory. Default
local embeddings use hashing/lexical overlap; MMU scope ranking is not an ACL.
No result above certifies those separate contracts, or fixes #224's unconfirmed
high-output provider incident.
