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
no blocked declaration, truncation, or exhausted round budget. The failed
round's detail was not expanded in the supplied file, so its actual error is
still unknown.

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

This fixes the reproduced protocol recovery gap. Confirming that it addresses
the deployed #96 failure still requires that failed round-2 diagnostic and a
real GLM run. Issue #232 remains unresolved pending that verification.

Validation: 4585 unit checks; 21 fixed-wire scenarios with normal and ASAN
builds; 6 artifact-recovery scenarios; 20 completion-review scenarios; 20
embedded-tool scenarios passed. The new fixed-wire scenario fails before the
patch on the required recovery-state assertion.
