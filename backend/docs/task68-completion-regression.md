# Task 68: absent exploratory read and historical answer truncation

The original installed Agent task is preserved. It failed after 571259 ms,
63 model requests and 102 tool executions: 2284139 input + 174901 output =
2459040 reported tokens. The guessed `src/query/transitions.ts` did not exist.
Later directory and search observations established absence, but an unresolved
`file_read` gate still demanded successful reading. This forced unnecessary
recovery instead of accepting a source analysis based on files actually read.

The live task API retained 9936 answer characters. The journal saved only the
first 4095 bytes, sometimes cutting a UTF-8 character. That is a separate
runtime persistence defect, not evidence of provider output-token truncation.

## Changes and boundaries

- `complete_task.unavailable_optional_reads` is a structured model decision:
  path, reason and a later successful exact-parent `list_dir` step ID.
- The runtime checks actual absence, exact scope and evidence ordering. It
  accepts no prose completion keywords. Existing files, failed mutations,
  unrelated directories and fabricated step IDs cannot clear the read failure.
- The model must not waive required sources; it must report those as blocked.
  Analysis omissions must be explained and grounded in sources actually read.
- Journal output is retained in full independently of model context packing.
  Older malformed UTF-8 journals are sanitized when read; missing old answer
  tails cannot be reconstructed from those truncated records alone.

## Validation

Local provider + actual CLI: 7 scenarios (verified optional absence, no decision,
wrong parent, evidence before failure, bad step ID, failed mutation, existing
file). Full multilingual output is compared after actual restart. All pass.
The journal unit checks include a long Chinese answer and a legacy torn UTF-8
record. Existing 14 OpenAI/Anthropic finalization/recovery scenarios also pass.

Real configured API, through an isolated Cognitive OS Agent process:
`build/live-api-suite-20261023-072649/`. The Agent encountered the missing
optional path, listed its parent, read the real source containing a randomized
proof value, and finished DONE. Observed cost: 9 model requests, 6 tool
executions, 26653 ms, 56817 input + 4709 output = **61526 reported tokens**.
Its 5470-byte answer contains the exact proof value and complete conclusion.
The runtime logged structured resolution against directory evidence step 2.

The first harness incorrectly applied the diagram-OCR assertion to this new
case after the Agent had completed. That test failure is retained in report.json;
the case dispatch was corrected. Independent verification reuses the saved task
and source, documented in independent-validation.json; no additional paid calls
were made to repair the harness. Real-test journal persistence was not separately
verified because its temporary state was already removed; the restart assertion
is covered by the local integration test. These are different tasks/scopes, so
the cost numbers do not establish a percentage improvement for the full original
repository analysis. Cached duplicate reads remain visible in the saved trace;
this does not claim zero redundant model work.
