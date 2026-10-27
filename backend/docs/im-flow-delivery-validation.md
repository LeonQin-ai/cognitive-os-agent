# IM capacity waits and Flow delivery regression

## Observed failures

The reported three-node Flow ended with two nodes marked `ok`, followed by a
reviewer failure after nine model requests and seven successful directory
listings. Its final text still announced reads that had never happened. The
pasted trace also contains an architecture node whose result is only a promise
to read and write a document, and a downstream report cut off at 4096 bytes.
A node's `ok` label alone cannot establish that it delivered the requested work.

A separate real API reproduction filled all four chat lanes, submitted an IM
turn, and released lane 1 while lane 0 stayed busy. The old IM turn remained
RUNNING: it had blocked specifically on lane 0 instead of taking the free lane.
Lock waits also ignored cancellation, and 32 historical sessions exhausted a
fixed session-lock table whose fallback serialized unrelated sessions.

## Changes

- Wait for any available lane; keep lane, same-session and IM-room waits
  cancellable and report resource waiting explicitly. Reclaim per-session locks
  when their last owner/waiter exits, without truncating session identifiers.
- Bind the parent task's HTTP cancellation callback inside Flow worker threads.
  Binding only the scheduler thread did not interrupt silent node requests.
- Put Flow candidates through the structured delivery review before legacy
  missing-read/narration checks can prematurely terminate the node. Apply the
  existing progress-only announcement guard to native Flow completions too.
  No additional language phrase list or unlimited retry budget is introduced.
- Save each full node result with its original/resolved instruction and execution
  workspace under `<state>/flow-results/<task-id>/node-<ordinal>.md`. Long inline
  previews point to that file for real paginated reads. Preview cuts respect
  UTF-8 boundaries. Expanded node instructions are dynamically assembled and
  bounded at 512 KiB rather than silently clipping off the final instruction.
- Automated repair PRs now use `Fixes #...`; merging the verified repair closes
  the linked issue. The automation still does not merge PRs itself.

## Verification

`tests/test_flow_review_delivery_e2e.py` exercises three real scheduled nodes
and filesystem tools in native and legacy protocols:

- The old runtime reproduces `2/3`, FAILED, nine reviewer requests and seven
  directory calls using the reported narration. The updated recovery reads the
  saved report tail, discovers the omitted critical files, reads both source
  files and returns the evidence-backed review: `3/3`, DONE, thirteen reviewer
  requests. Architecture/source reads and a written/read-back document are
  checked; completed upstream nodes are not restarted during recovery.
- A second case expands two long references beyond the old 8192-byte buffer and
  verifies that the actual reviewer instruction survives.
- A consistently uncooperative model remains FAILED; directory success is never
  relabeled as a completed review.
- A silent reviewer request can be cancelled within three seconds, while a
  concurrent IM discussion finishes normally.

`tests/test_im_capacity_e2e.py` verifies the free-lane case, cancellation without
releasing occupied lanes, same-session waiter cancellation without affecting its
owner, and concurrent chats after more than 32 historical sessions.
`tests/test_im_capacity_ui.cjs` verifies resource-wait text, stopping, and rejection
of delayed queued events after cancellation.

Local Linux results: eight Flow cases and four capacity cases passed, also under
AddressSanitizer. The browser regression, 4507 unit checks, existing reciprocal IM
and restart suite, Flow workspace/restart suite, session-isolation suite, twenty
issue-223 completion cases, issue-repair plumbing tests and workflow validation
passed. The new checks are required in platform CI and the Linux ASAN job.

These are local controlled-provider reproductions. They do not claim a rerun of
the user's private Windows task #71 or verification of the target project's
actual architecture and implementation. Historical terminal task records are
preserved; updating the runtime does not rewrite an old FAILED record to DONE.

## Windows gate timing follow-up

The cancellation case formerly required all upstream execution and reviewer
setup to finish within a separate five-second event wait. Windows CI exhausted
that preparation wait before issuing cancellation. Preparation now uses the same
25-second Flow budget as other cases and reports terminal node failures
immediately. The fixture deliberately delays the first upstream response by
5.5 seconds so restoring the old five-second wait fails on every platform.
Actual cancellation remains bounded at three seconds, measured from before the
DELETE request through the terminal state. Provider response helpers also handle
peer aborts during headers and body; unexpected provider errors still propagate.
