# Tool-call normalization and batch validation (#232)

This change is parser/executor-boundary work. It does not change planner prompts,
request/answer phrase lists, provider decoding settings or recovery budgets.

## Reproduced code defects

A fixed HTTP fixture returns the same response sequence regardless of request
text. With the prior binary, the valid comparison sequence fails: a JSON
`tool_calls` envelope and a standalone `function` envelope are not fully
recognized, and `{tool, arguments}` loses its arguments because that fallback
branch is unreachable for a valid `tool` field. The observed attempted read has
`args: {}` and a missing `path` error despite the response containing its path.

A second fixed fixture returns a valid file-write action followed by an invalid
entry in the same JSON batch. The prior parser skips the invalid item and
actually writes the prefix file repeatedly. These are reproduced implementation
defects, not claims about the unavailable GLM response that produced the user's
most recent zero-tool task.

## Code behavior

- Normalize explicit `tool_calls` arrays and single `{function:{name,arguments}}`
  or `{name,arguments}` envelopes into the existing action representation.
- Honor the `arguments` field when a record uses `{tool,arguments}`.
- Accept argument objects and fully parsed JSON-object strings. Reject arrays,
  other explicit types and JSON strings with trailing garbage instead of
  substituting `{}`.
- Validate all records and registered-tool schemas before executing any part of
  the batch. Invalid structure or arguments cause zero tool actions, including
  no file-write prefix. Actual tool-runtime failures still use the existing
  failure/transaction handling; this is not a new side-effect rollback system.
- Interpret a sole valid `complete_task` as a control declaration even before a
  legacy completion-review round. It is not a filesystem tool or a reason to
  generate a plugin. Completion markup and delivery checks remain enforced.
- Retain the known target of schema-rejected work for recovery. An unrelated
  read cannot resolve it. An invalid invocation without an identifiable path is
  a planning error, not a fabricated file failure. Diagnostics retain only tool
  name/path, never command text or file mutation contents.

## Validation

`test_legacy_tool_envelopes_e2e.py` never examines system/user prompt text or a
completion-review phrase. Its nine fixed-wire scenarios validate:

1. Both actual source reads, HTML write/readback, final result and saved history
   from the valid envelope sequence (DONE; five requests/four tools).
2. A schema-rejected batch followed by a corrected sequence (DONE; six requests,
   four tools, one planning failure; rejected prefix file never exists).
3. An unrelated successful read after rejection of a known required search
   target (FAILED, with the original target unresolved).
4. A direct completion control (one request/zero tool actions, no generated tool).
5. Missing name, invalid argument type, trailing argument bytes, and missing
   schema field all fail before any tool executes or prefix file is created.
6. Pure commentary without calls stays FAILED with zero tools; the runtime does
   not invent calls from “我将…让我…” sentences.

The same fixed-wire scenarios pass under ASAN (`detect_leaks=0`). The existing
schema-error mock now actually sends corrected calls before claiming recovery;
its repeated-invalid control expects five rejected planning rounds/zero tools,
rather than three invalid tool executions in the first round.

Local validation also covers the full C suite, six artifact-recovery scenarios,
20 completion-review scenarios, both resumed-review protocols, legacy recovery,
20 embedded-markup scenarios, native OpenAI/Anthropic loops, and Flow regression.
The fixed-wire suite is included in every platform/ASAN CI path.

#232 remains open: the actual GLM response must be checked to establish whether
it contains one of these explicit envelopes, malformed calls, or no calls at all.
A response with no calls cannot be made into an executed read from prose alone.
No original Windows source or GLM API run is claimed by this fixture.
