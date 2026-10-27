# Issue #223: pending tool calls accepted as completed analysis

Baseline: `20c7028` on `main`. The issue reports six successful reads followed
by a final answer containing a malformed `file_read` tag and offset 3998.

## Reproduction and cause

The API regression reproduced the failure before the fix: after six real file
reads and the one-shot completion review, a native `complete_task` whose answer
contained the issue's malformed tag became `DONE`. No subsequent README page
was read. The previous completion tests exercised review recovery but did not
exercise control markup inside the reviewed completion's answer.

The planner validated the outer completion declaration without validating that
its answer still contained pending control frames. Separately, the legacy
tagged-call parser could fall through after malformed/truncated calls, and
searched within JSON argument strings instead of treating them as payload.

## Change

- Check native and reviewed legacy completion answers on every completion,
  including after the semantic review has already run.
- Reject unexecuted tagged/DSML control markup, retain a planning error, and ask
  the model for a valid next action. Do not invent malformed arguments locally.
- Reject incomplete tagged batches atomically. Never demote a failed action
  repair to an ordinary answer. Preserve JSON tool-argument strings verbatim.
- Preserve protocol examples inside Markdown code spans and fences; examples
  and literal file contents do not authorize executing their embedded commands.

## Verification

`tests/test_embedded_tool_completion_e2e.py` runs 20 API scenarios across native
and legacy protocols. It checks the actual scheduler status, tool records, byte
offset, source evidence, final answer, and saved conversation history:

- Recover from the exact issue spelling, valid pending tags, truncated tags,
  incomplete opening tags, DSML, and raw malformed content after review.
- Read README from byte 3998 and include `TAIL_ARCHITECTURE_PROOF` from that
  real tool result in the delivered analysis: seven real reads, five model calls.
- Repeated malformed completions terminate `FAILED`, never `DONE` (seven calls).
- Fenced/inline examples remain unchanged; writing and reading back a file
  containing a tool tag preserves bytes and never invokes its embedded shell.

Local Linux results:

- New regression: 20/20 passed, also 20/20 under AddressSanitizer.
- Unit suite: 4507 passed, 0 failed (`COA_TOOL_PROTOCOL=legacy`).
- Existing source-read API suite: 14 scenarios passed.
- Existing native/legacy completion review, native OpenAI/Anthropic tool-loop,
  and prose-prefixed DSML execution regressions passed.
- Workflow validation: `actionlint` passed.

The new regression is required in Linux, Windows, both macOS architecture jobs,
and Linux ASAN CI. Local fixtures verify the runtime contract; they do not claim
execution against the reporter's private RelayAgent project or real provider.
