# Oct 29, 17:31 GLM continuation failure: unresolved

The report shows three planning responses executing nine displayed reads/listings,
then two answer responses, followed by failure because no HTML was delivered.
It does not contain the raw responses, completion declarations, configured round
limit, or running commit. The final displayed narration is not sufficient to
infer the response envelope or the branch that stopped execution.

`test_legacy_tool_envelopes_e2e.py` now reconstructs those tool targets using
temporary fixture projects, followed by two synthetic copies of the reported
narration. The fixture never inspects prompts to decide its next response.

* `trace_resume`: tools emitted after the two prose responses execute; HTML is
  written and read back, then the task completes on request 10.
* `trace_stuck`: a provider that keeps returning that prose stops on request 9;
  the missing report is correctly rejected. The repeated README read uses the
  existing cache, leaving eight executed discovery tools.

All 20 wire scenarios pass with the normal and AddressSanitizer builds of
`d254dba`. These are synthetic protocol tests, not a GLM integration result.
Neither reconstructed case reproduces the reported stop on request 5. No
production behavior or prompts have been changed in response to this report.

The next required evidence is the raw provider responses for calls 4 and 5
(message content, tool_calls, finish_reason and complete_task arguments), the
running commit/build and reasoning.max_rounds. Check the preserved execution
logs before changing completion, blocked-state or budget handling. Keep #232
open; physical artifact acceptance prevents false success but does not make a
provider generate the missing report.

## Follow-up: exported trace has no planner decisions

The additional task #94 trace confirms five `llm.plan` spans all marked
successful. It still contains no response bodies. These span statuses mean
successful planning, not completed work, and cannot establish whether a
response was a completion declaration or prose. This is a reproducible
observability gap: previously only tool spans had expandable decision details.

Each `llm.plan` span now stores a bounded `planner decision` detail containing
round/limit, protocol, review requirement, parsed completion declaration,
planned-action count, provider stop reason, truncation flag, parsing error (if
any), and the parsed response (if available). This is not the full raw provider
response or hidden reasoning. Request context and authentication headers are
not recorded. Existing trace redaction and 4 KiB truncation apply. Details use
the existing trace API, expandable UI and durable task journal.

The two reconstructed cases verify both live and journaled planner details,
including the rejected fifth prose response and final structured completion.
This fixes diagnostic visibility; the original GLM stop on request 5 remains
unreproduced and unresolved. No execution, completion or prompt policy changes
are included.

## Task #96: retain the recovery protocol after rejected plans

The next export contains a failed planner request on round 2, followed by three
plain narration responses on rounds 3–5, each showing
`completion_review_required:false`. The last two texts are identical. There is
no blocked declaration, truncation, or exhausted round budget. The subsequent
expanded round-2 detail confirms `invalid or incomplete tagged tool-call
response; no actions executed; use the configured tool protocol with a valid
identifier and complete JSON object`. This is the protocol-error branch covered
by the fix, rather than a transport or tool-execution failure.

A fixed-wire test now covers one concrete variant: directory discovery,
malformed tagged tool call, three narration responses, then actual source
reads, HTML write/readback, and structured completion. Before this change the
test fails because the first prose response after the malformed call is still
accepted with the completion protocol disabled. This does not reproduce the
entire deployed failure: the existing local review subsequently recovers.

On a legacy protocol parsing/schema failure, the runtime now enables the
existing structural completion protocol for subsequent recovery calls. Prose
alone is rejected by the planner; it cannot fall into narration equality
checks. Tool actions and valid complete/blocked declarations remain accepted.
No prompt wording, keyword lists, retry limits, physical artifact checks,
transport error handling or native behavior changed. In-process test providers retain
their existing behavior. The new test requires the recovery protocol on round
3, checks all three rejected prose responses in live and journaled traces,
then requires real HTML delivery and completion on request 10.

This fixes the reproduced protocol recovery gap, and the supplied round-2
diagnostic confirms that #96 entered the affected branch. Successful completion
with the actual GLM endpoint still requires a real run. Issue #232 remains
unresolved pending that verification.

Validation: 4585 unit checks; 21 fixed-wire scenarios with normal and ASAN
builds; 6 artifact-recovery scenarios; 20 completion-review scenarios; 20
embedded-tool scenarios passed. The new fixed-wire scenario fails before the
patch on the required recovery-state assertion.

## Task #97: recovery flag remains false in the deployed trace

The copied diagnostic confirms a tagged-call parser failure on round 4, two
successful read rounds, then identical prose on rounds 7 and 8. All planner
decisions still show `completion_review_required:false`, including after the
protocol failure. There is no structured blocked declaration or budget limit.

`trace_after_error_progress` reproduces that order with temporary source
fixtures. On the current runtime the flag becomes true on round 5 and stays
true after both reads; prose rounds 7 and 8 are rejected structurally, and later
write/readback actions complete the report on request 12. Live and journaled
decisions are both checked. The scenario also passes ASAN. This is a controlled
response replay, not a successful integration run with GLM.

The user confirmed the current source/executable and `openai` provider. Treat
those as established; do not attribute this report to an old process. The
exported false flag conflicts with the local transition, and the complete
deployed stop is still not reproduced. The generic recovery invariant below
is now enforced at the producer instead of a late outer-loop handler.

## Generic protocol recovery, without task or provider classification

The runtime now latches structural recovery in `h_reason`, immediately when
the planner rejects a legacy response at the protocol stage. No task text,
HTML requirement, provider name or model name participates in that decision.
The provider whitelist and the outer-loop transition were removed. There is
no new task-type branch or prompt text. The proposed HTML-specific startup
change was discarded before commit at the user's request.

The state remains latched across successful intermediate tools and clears at
the start of a new run. Native completion, HTTP/transport failure handling,
ordinary plain-text conversation, existing failure budgets and acceptance
checks keep their existing paths. This enforces an error-recovery contract;
it does not manufacture tool actions or force a stalled model to succeed.

Four additional fixed-response scenarios have no HTML requirement: successful
source verification after a malformed response and three prose responses;
bounded failure when prose persists; a legitimate blocked declaration; and a
plain greeting that still completes on its first call. Existing source and
artifact scenarios continue to verify actual tool evidence.

In-process tests use the non-vendor `fixture` provider and inspect actual trace
decisions to require structural recovery on the second call after schema
rejection. Their final responses now use valid completion declarations, while
existing assertions still reject unrelated evidence and unresolved failures.
The offline mock's corrected-arguments fixture also emits a valid declaration
after its successful retry. These fixture updates conform to the generic
protocol; no live-provider behavior is keyed to those scenarios.

Validation: 4594 unit checks, 26 fixed-response scenarios with normal/ASAN
builds, 20 completion-review scenarios, 20 embedded-tool scenarios and the
legacy completion-recovery suite passed. Successful GLM execution remains an
integration check; issue #232 stays open.
