# Directory recovery and task completion

The supplied task 73 export contains a complete Context report, but its final
failure explicitly identifies an unresolved `list_dir`. Tool arguments are not
included in that export, so the exact failed Windows path is not established.
Do not infer that every later successful tool repairs that particular failure.
The architect result is also a progress announcement, not an architecture report;
this change does not claim to have semantically validated that private run.

A controlled reproduction shows a missing guessed `src/context` directory,
followed by successful listing of `nested/src/context` and a real source read,
still causes FAILED despite the delivered analysis. Missing file recovery already
recognizes the same nested workspace relocation, but directory recovery did not.

The fix applies the existing full relative suffix rule to a successful
`list_dir`: the failed directory must still be absent, its error must identify a
missing directory, and both paths must be under the active workspace. A matching
basename, unrelated source reads, or an existing path that failed cannot clear it.
No tool is replayed automatically and the execution history retains the original
failed call. Remaining failures now include the affected path and original error.
External-project relocations without this workspace relationship remain failures
pending actual retry/evidence; this is deliberately not a blanket success override.

Validation:

- Old binary reproduced FAILED after successful listing, source read and report.
- Completion review tests cover legacy/native recovery, unrelated directory,
  existing non-directory path, and source reads without the replacement listing.
- Three-node Flow regression verifies architect/coder/reviewer reach 3/3 and DONE
  after the reviewer recovers the directory, reads the real source and upstream
  report, and submits its actual review. Existing stuck and cancellation cases
  retain their expected terminal states.
- These tests run in the existing platform and ASAN CI steps; no private source,
  provider credentials or uploaded report are committed.

Already-terminal task 73 is not rewritten by this fix. Re-execute with the updated
binary to validate the actual Windows workspace; inspect the specific failed
path and error if another unresolved operation remains.

Local results: 4504 unit checks passed; 20 completion review scenarios and 10
three-node Flow scenarios passed. The same 20 completion and 10 Flow scenarios
also passed with AddressSanitizer (`detect_leaks=0`). The existing legacy
completion-recovery suite passed with its required `COA_TOOL_PROTOCOL=legacy`.
Windows/macOS execution is delegated to CI and is not claimed as locally tested.
