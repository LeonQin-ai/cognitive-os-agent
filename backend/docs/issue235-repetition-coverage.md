# Issue #235: fragmented repetition bypassed completion validation

The issue exports task 109 as DONE after a source read and many repeated
announcements. Its final planner preview is truncated; it shows repeated
`让我继续读取文件并搜索关键代码。` blocks interrupted by `......`. The follow-up
task 110 instead fails after several prose-only turns. These exports do not
contain the full original requests, complete final model response, or original
private source trees. Neither is evidence of successful analysis or delivery.

## Reproduced runtime defect

The response-wide repetition detector previously required a *single* periodic
region to occupy 40% of the answer. Several long repeated regions separated by
punctuation or short labels can occupy almost all of an answer while each
individual region stays below 40%.

A controlled native response with three such regions was accepted by the old
runtime: exit 0, one model request, and the repeated answer delivered verbatim.
The legacy regression also missed the response-level check and consumed five
requests instead of stopping on the degenerate response. This reproduces a
general validation gap, not the unavailable complete original GLM response.

The detector now accumulates coverage from qualifying periodic regions without
counting overlapping bytes twice. The existing per-region minimum (six cycles,
768 bytes) and answer-wide 40% threshold remain. Ordinary short quotations,
multiple sparse quotations, and distinct analysis still pass. No task types,
provider names, new narration keywords, prompts or retry budgets were added.

## Validation and limits

`tests/test_answer_repetition.py` covers both legacy and native paths, including
fragmented cycles and non-dominant disjoint quotations. Four HTTP scenarios
verify that a real source read followed by degenerate output results in FAILED,
uses exactly two requests, preserves the tool step, and adds no successful chat
turn. CLI checks verify that the repeated content is not delivered. Existing CI
registration runs the expanded suite.

Local verification: 4597 unit checks, 32 tool-envelope/recovery scenarios,
24 repetition scenarios (including four HTTP cases), and the repetition suite
under AddressSanitizer. Completion-review scenarios are also checked.

## Task 110: stop by executable decisions, not prose equality

The `strcmp(txt,last_narration)` termination branch and its stored text are
removed. The compatibility correction allowance is one retry, independent of
wording; it no longer grants four intent nudges to varying announcements.

The planner reports `no_action_response` when the configured completion protocol
requires an executable decision but the response contains no actions or valid
completion declaration. Both native and reviewed legacy paths use this typed
signal. Two such rejections without new observations stop the run with an
explicit diagnosis. Malformed argument/schema, transport and truncation failures
keep their existing separate recovery paths and budgets.

Successful new observations reopen the window; cached reads do not. A cached
legacy action that clears parser recovery therefore rearms it if a no-action
rejection is still pending. This rule contains no task-type or response-word
classification. Trace exposes the typed signal for diagnosis. This changes no
prompt wording and does not attempt semantic similarity matching.

The fixed-response task-110 scenario uses the reported variations, actual
directory/source tools, and no HTML requirement. The previous runtime reaches
9 requests in this controlled replay; the changed runtime stops at request 6
after the two rejected decisions. The original export contained 8 requests;
this is not a claim of an exact replay of its unavailable request context.
Additional cases cover recovery after one rejected response, real-progress
reset, cached-read non-reset, ordinary conversation, and legitimate blocking.
The earlier delayed-operation sequences are retained as negative regressions:
they now hit the stricter bound before their third unproductive response or
later artifact write, rather than being removed from coverage.

Both the 32 envelope scenarios and 24 repetition scenarios also pass ASAN;
20 completion-review, 20 embedded-tool, 12 Flow, six artifact-recovery and
resumed-review cases pass. The complete legacy completion-recovery suite passes.

These changes fix false completion of fragmented repetition and excessive
prose-only recovery. They do not make a provider that keeps returning commentary
produce a valid action or report. Actual GLM task completion remains unverified;
keep issue #235 open pending integration verification of that remaining claim.
