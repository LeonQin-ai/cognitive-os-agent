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
