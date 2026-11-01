# Native turn completion and CI migration

Reference inspected locally: claude-code-src-main/src/query.ts, especially
needsFollowUp initialization and tool_use accumulation (554-558, 826-836),
the no-tool branch (1062), output-limit recovery, and Stop Hook handling
(1258-1305). Transport errors must not be mistaken for delivered results.

Native complete, nontruncated tool-free text now becomes a completion candidate
instead of mandatory commentary recovery. The candidate is passed through the
same structured acceptance checks as complete_task, and existing delivery review.
Tools still control continuation. Tool markup in a purported final answer,
truncation, failed tools and missing declared receipts still prevent completion.
Legacy wire mode continues to require explicit decisions. The 128-round proposal
was withdrawn; negative/default settings retain their original semantics.

CI repairs preserve assertions and actual evidence:
- Flow stuck replies mark requires_tools=true in both protocols.
- Vision fixtures declare completion only for their successful ready replies;
  silent/trickling responses retain their original timeout stimulus.
- Legacy DSML replies survive the fixture final-answer helper. The parser accepts
  complete DSML after visible commentary, excludes Markdown examples and leaves
  markup inside JSON file content untouched. Whole batches are still validated.
- The pre-existing plan-cycle fixture migration was reviewed and tested, retaining
  repeated-plan stopping, actual recovery, and successful-action checks.

Focused local results: Flow 12 scenarios, vision timeout/recovery, DSML actual
write and source evidence, structured-contract scenarios, 20 completion-review
scenarios, plan-cycle suite and a native zero-tool plain final answer passed.
Full C validation remains pending: an initial run crashed after plugin setup;
the original log is retained in build/nov1-final-unit.log. Do not treat focused
results as a successful full gate or close the outstanding issues on this basis.
