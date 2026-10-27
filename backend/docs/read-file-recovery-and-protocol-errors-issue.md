# Plugin read_file failure remains unresolved after a claimed file_read fallback

Status: reported; exact plugin invocation and replacement-read evidence pending.
This document is an issue draft, not confirmation that a GitHub issue exists.

## Observed report

The user reports a FAILED task with an unresolved `read_file`, followed by a
substantial model analysis. The model claims a generated plugin required
`fs.write` while granted `fs.read` and `shell`, and that a built-in `file_read`
subsequently read the target. The pasted result does not include actual tool
arguments, permission error, generated plugin definition, or replacement output.
Those claims therefore require execution-trace verification.

The same model answer attributes repeated `invalid or incomplete tagged tool-call
response` errors to a plain-language clarification question. This is not an
established root cause: the parser accepts plain text and rejects malformed tool
markup separately. Obtain the raw provider response, configured protocol, and
parser diagnostic before changing that behavior.

## Code findings

- Failure scopes currently recognize built-in file tools and discovery tools;
  arbitrary generated tool names such as `read_file` have no equivalent path
  scope. A later `file_read` does not prove that an arbitrary plugin obligation
  has been completed.
- Missing tool names can trigger plugin generation; generated bindings survive
  restart. Inspect the actual binding and skill before treating it as an alias.
- Never clear arbitrary plugin failures just because another file was read, and
  never grant extra filesystem write permission to make a nominal read succeed.
- The reported failure format lists only `read_file`. Since b34403d, diagnostics
  include each failure's path and error. The run may predate that build or use an
  older process; verify the executed binary and task creation time.

## Expected behavior and acceptance checks

1. Reproduce the denied generated tool with its real arguments and capabilities.
2. If equivalent built-in reading is supported, associate recovery with explicit
   validated read-only semantics, the same canonical file, and actual successful
   read evidence. Keep unrelated reads and write obligations unresolved.
3. Cover the real task lifecycle and saved history with both configured tool
   protocols; recovered work must deliver the requested result, not a promise.
4. Verify a plain clarification question does not produce a tagged-markup parse
   error; retain rejection of incomplete/malformed executable markup. Preserve
   literal examples in fenced documentation.
5. Verify missing permissions remain enforced and no arbitrary generated or MCP
   tool can evade its obligations through a read-like name.

## Related pushed fixes and validation

- 5e1189a: pending executable markup in completion answers is rejected (#223).
- b34403d: verified nested-directory listing can repair a missing directory guess;
  unresolved failures include concrete path and original error.
- 410fa43: actual new tool evidence clears stale narration repetition state so a
  progressing Flow can continue to final delivery. Local checks: 4507 unit tests,
  12 Flow scenarios normally and under ASAN, 20 completion-review scenarios, and
  the legacy completion-recovery suite passed.

These fixes are on main. They do not establish that this newly reported generated
plugin failure is solved. Do not close this issue until the real invocation is
reproduced and the acceptance checks pass. No private project paths, source,
credentials, or uploaded full execution report are included here.
