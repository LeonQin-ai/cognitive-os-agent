# Nested run cleanup: blocked status, provider restoration and cancellation

Code inspection found `agent.before_run` rejection returned zero, so the
scheduler/Flow caller could count an unexecuted run as successful. That early
return also skipped restoring/freeing the selected task-local model. Normal
and blocked reasoning exits cleared the thread-local HTTP cancellation binding
instead of restoring the caller's binding. In IM this caller owns the entire
discussion and subsequent channel sends, not just one member's reasoning turn.

Reject a blocked run with nonzero status, restore/destroy the temporary selected
provider, and save/restore the caller's HTTP cancellation callback/context on
both exit paths. Existing watcher joins still protect borrowed callback and
transport lifetimes. No global provider/cancellation state is mutated.

`test_nested_run_cleanup` directly checks blocked and successful return values,
provider reuse after a blocked routed task, and the original callback/context
remaining callable after each nested run. All 14 assertions passed. Existing
HTTP cancellation, lane capacity and reciprocal discussion tests additionally
exercise actual scheduled tasks, failures, cancellation and room transitions.

This does not claim a real external messaging service outage was reproduced;
the broken callback scope and blocked-return contract were established in code
and direct Runtime interface regression.
