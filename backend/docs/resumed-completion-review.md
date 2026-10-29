# Completion review after resumed work (#232)

Code review reproduced an early-DONE path in explicit legacy compatibility mode:
read a source, offer a candidate, let completion review resume another real read,
then offer a multi-paragraph progress announcement via `complete_task`. The
old `completion_reviewed` flag remained set after the resumed read, so the new
candidate was accepted without a fresh review. Existing prose heuristics did not
cover this paragraph shape and must not become the fix.

Any new actual observation now invalidates the prior review in both protocols.
Cached/repeated unchanged observations do not. The model still chooses the next
tools or structured completion; no new answer-keyword rules were added. The
review instruction now correctly describes one review per candidate work stage,
including a new delivery after resumed operations, rather than claiming only
one review per entire task. Task progress also includes `tool_protocol` and
`completion_declaration` for deployment diagnostics. These safe metadata fields
also survive in the task journal's trace; model prose/credentials are not added
to trace events.

`test_resumed_completion_review_e2e.py` reproduces the state-machine scenario:
before the fix, legacy returned DONE after four model calls/two file reads with
only a progress announcement. After the fix, legacy and native each perform
three actual source reads and seven calls, retain the required final source
proof, and save the delivered answer to chat history. The native path already
invalidated old reviews; this closes the compatibility gap. Existing completion
and Flow tests retain source, failure, cancellation, budget and upstream-artifact
checks. Their legacy expected call count changes by the one new delivery review.

This is a proven compatible failure class, not proof that the unavailable
reporter's `maas-glm-5.1-zhipu` instance used legacy mode. Their exact source,
build and final response structure remain unavailable. A separate opt-in
`test_live_comparison_delivery.py` asks the configured model to read two synthetic
CPU-frequency projects, generate an actual HTML comparison and read it back.
No assistant-authored report is supplied.

The first actual configured `deepseek-v4.1-flash` legacy attempt did **not pass**:
the Runtime rejected an invalid/incomplete DSML response before executing any
tool. The harness stopped on that failure, cancelling the in-flight recovery
attempt rather than retrying the whole benchmark. Observed usage was 3,074 input
+ 65 output = **3,139 tokens**, one recorded request. The cancelled recovery
request did not report usage; it cannot be assigned zero cost. No HTML was
delivered. The terminal task diagnostic reflects cancellation transport error
12017; the original DSML failure is retained in
`build/live-comparison-delivery.log`. The harness now also retains its
pre-cancellation task observation on future runs; this lost detail is not
fabricated retroactively. Raw report: `build/live-comparison-delivery.json`.

Keep #232 open pending the reporter's deployment evidence and matching actual
model verification. Passing deterministic tests or another configured model's
answer is not grounds to claim their original task fixed. #224's length-limit
incident and #231's repeated real-model compaction acceptance remain independent.

## Follow-up: contradictory recovery instructions

### Native paged-source cursor correction

Native planner input incorrectly labelled every pending read as optional and
explicitly said EOF was unnecessary. It now presents observed incomplete byte
coverage neutrally and requires the planner to compare it with the original
scope before structured completion. This does not classify answer keywords or
force exploratory excerpts to EOF. The resumed-review fixture uses a long file
and rejects the old misleading exemption in native input.

Actual configured API validation: `test_live_comparison_delivery.py
--protocol native --paged-sources` passed. Both source markers were placed after
18 KB of comments. Actual reads covered offsets 0/4000/8000/12000/16000 for both
files, followed by HTML write and paged readback. Final marker and artifact
checks passed: 11 requests, 14 tools, zero model failures, 130,804 input + 6,489
output = 137,293 tokens, 40.374 seconds. Report and generated HTML are retained
under `build/live-paged-comparison-232.*`. This is synthetic task validation with
the configured DeepSeek API, not a claim of testing the unavailable GLM endpoint.

Code review found that cached-read and stall recovery told native planners to
finish with plain text, contradicting the mandatory `complete_task` contract.
These recovery messages now follow the active protocol; cached native reads
explicitly require structured completion or the next necessary operation. No
answer-keyword completion heuristics or malformed-argument repairs were added.
The resumed-review fixture now inserts a cached read and checks both protocols
still finish with actual source evidence and a fresh delivery review.

The live comparison harness now selects its protocol explicitly, allows the
Runtime's configured 16 work rounds plus bounded finalization (the old external
12-call cutoff could cancel legitimate readback), and preserves generated HTML
even on failure. The original failed reports are unchanged.

One native execution passed with independently checked source markers, HTML
write/readback, final answer and unchanged sources: 9 requests, 7 tool executions,
0 model failures, 67,550 input + 6,906 output = 74,456 reported tokens, 43.49 s.
Report: `build/live-comparison-native-capture.json` (the temporary diagnostic
capture was not triggered and has been removed from source).

Earlier native follow-ups remain failed: external budget cancellation consumed
98,294 reported tokens; two argument-error runs consumed 11,423 and 41,739.
Their cancelled in-flight usage is unknown. The latter did create and read back
a valid artifact, but failed before final delivery. A subsequent isolated probe
captured DSML markup corrupting an otherwise native JSON argument key; this
provider-response problem remains tracked separately under #224. The successful
comparison does not prove that intermittent response corruption is eliminated.

## Ordinary chat recovery before legacy stop guards (October 29)

The latest #232 report again contains README reads and repeated work
announcements without the requested HTML. Code inspection found an independent
control-flow gap: Flow candidates receive explicit delivery review before legacy
read/narration guards, but ordinary tool-backed chat candidates reached those
guards first. The repeated announcement could therefore end FAILED without ever
receiving the existing `COMPLETION REVIEW PROTOCOL` next-action/completion
contract. This does not establish that the unavailable GLM run used legacy mode.

All tool-backed legacy candidate answers now enter that same bounded review
before prose guards. A legacy request with an already-detected required mutation
also receives it when no tools have executed yet. Existing mutation detection is
unchanged; no new request/answer keywords, synthesized actions, malformed-call
repairs, protocol switching, or expanded iteration budgets were added. Native
completion and Flow behavior remain structural. The model must still return
actual executable calls; a promise alone cannot cause a file read or report write.

`test_artifact_delivery_recovery_e2e.py` covers the comparison request wording
without adding a `generate` verb to it. Two README reads are followed by the
reported progress shape, a real two-file source batch, HTML creation, and actual
readback. The test independently checks both source markers, the saved artifact,
post-write reading, final answer and chat history. Native batch replies also
carry progress text, verifying that commentary does not hide real tool calls.
A zero-tool explicit artifact request recovers separately. A provider that keeps
returning commentary without a valid decision remains FAILED without creating a
report or saving a successful conversation turn.

The pre-fix deterministic reproduction failed after observations and a repeated
progress announcement, with the same generic unfinished/narration diagnostic.
After the fix the legacy comparison takes seven requests/six real tools; the
zero-tool recovery takes six requests/four tools. The stuck control stops FAILED
in seven requests. The native comparison takes seven requests/six tools. These
four cases are added to Linux, ASAN, Windows, and both macOS CI jobs.

Existing compatibility counts reflect moving the review: cached directory
recovery uses four requests instead of five, while the no-write stalled control
receives one explicit review and stops within its unchanged six-round budget.
Their actual-work and FAILED assertions remain enforced.

Local validation: 4,583 unit checks passed, 20 completion-review scenarios,
12 Flow scenarios, both resumed-work protocols, the legacy completion-recovery
suite and all four new cases passed. The four new cases also passed with ASAN
(`detect_leaks=0`). No original private source or configured GLM API execution is
claimed. Keep #232 open for CI and verification against the reported deployment;
this patch fixes a reproduced recovery gap, not every possible model refusal to
return tool calls.

## Zero-tool recovery with the reported wording and first-request failure

The next report repeats the HTML comparison request, with zero executed tools,
three model calls and one model failure, ending after “我将开始探索这两个项目的结构，
以便进行详细的对比。让我先列出这两个目录。” The prior zero-tool test used the
stronger “生成 comparison.html 文件” wording, so it did not exercise the original
request against the mutation detector. That was a real coverage gap.

A pre-fix binary reproduces the same three-call/one-protocol-failure/zero-tool
FAILED sequence with the reported request wording (private paths replaced by
fixture project names), a first malformed tagged response, then the exact quoted
announcement. This demonstrates a Runtime recovery path; the real first GLM
response/error has not been captured and is not claimed to be that malformed tag.

The early review previously required either already-successful tools or a
mutation detected by existing request heuristics. Reading/observation/intent
checks were evaluated only later, where repeated zero-tool narration could stop
the run before receiving the explicit next-tool/completion contract. Compute the
existing unmet-work checks first and use them for early review as well. No new
phrases, automatic tool inference, protocol switching or retry budget is added.

All zero-tool comparison cases now use the original “输出一个html来对比说明”
wording. New cases cover the first protocol error followed by the exact reported
announcement, and a zero-tool provider that continues refusing valid decisions.
The former completes seven requests/four actual tools (two source reads, an HTML
write, and readback). The latter remains FAILED in six requests/zero tools, with
no report or successful history. The regular zero-tool recovery completes six
requests/four tools. Existing tool-backed legacy, no-progress and native parallel
batch/commentary controls remain covered by the same CI test file.

Local results: 4,577 unit checks, all six artifact-recovery cases, 20 completion
review scenarios, both resumed-review protocols, the compatibility recovery
suite and 12 Flow scenarios passed. The six artifact cases also pass under ASAN
(`detect_leaks=0`). The private Windows source and GLM endpoint have not been run
in this workspace; deployment matching remains tracked by open #232.
