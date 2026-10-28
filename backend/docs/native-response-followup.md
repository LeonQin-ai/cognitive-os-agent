# Native response and rolling retention follow-up

## Confirmed observations

Isolated configured `deepseek-v4.1-flash` Runtime tests captured a non-truncated
HTTP 200 `tool_calls` response whose arguments mixed DSML markup into JSON:
`{"status">complete</｜DSML｜: ...}`. Rejecting the entire batch is correct; no
partial actions or guessed repairs are permitted. This is additional evidence
for #224, not proof that every earlier length-limit incident had the same cause.

The adapter now reports argument byte length, parse offset, string/object types
and truncation state without logging argument contents. Temporary raw capture
instrumentation was removed. The native regression checks atomic rejection,
recovery, safe diagnostics, and that ordinary text cannot declare completion.

OpenAI-compatible requests now use `tool_choice=auto` instead of forcing every
provider response into a tool call. Runtime still requires `complete_task` to
declare completion; plain text remains commentary and incomplete arguments are
never executed. This is a compatibility mitigation, not demonstrated elimination
of provider corruption. Existing OpenAI/Anthropic loop regressions passed.

## Actual API retention evidence

`build/live-rolling-retention-auto.json` used 15 synthetic **user-only** inputs
and 10 real native chat tasks. No assistant answers or model summaries were
fabricated. The log records two actual eight-turn compactions, producing 1,667
and 2,001-character summaries. The final exact random protected-directory and
acceptance-command recall passed. This supports the specific #231 fix carrying
the prior summary forward; it is not 500 real turns or a semantic benchmark.

The batch is still marked failed: final recall initially returned plain text,
then recovered via structured completion. Nine progress tasks had no model
failure; final recall had one. Usage: 67,438 input + 2,578 visible output +
1,304 reasoning = **71,320 tokens**, 13 ledger requests including compaction.
Efficiency acceptance is not claimed. The original report remains unchanged.

The harness now records retention and efficiency independently even if the
efficiency assertion ends the batch, and preserves the original strict ACK mode.
`--focus-retention` instead requests brief progress acknowledgements; final
random-fact recall remains exact. A prior strict-ACK auto-choice run failed
formatting (6,800 tokens), not a proven memory failure.

Additional failed required-choice probes retained: 27,965 tokens (one recovered
argument failure and one compression), and 20,129 tokens (two tasks, one recovered
argument failure, one compression). No failed report was overwritten. Previous
comparison costs and cancelled-request usage limitations are documented in
`resumed-completion-review.md`.

Keep #224 open pending evidence that the intermittent provider-response/high-output
problem is resolved. Passing retention or a successful HTML task does not prove
that result.
