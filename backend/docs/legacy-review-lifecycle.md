# Legacy protocol recovery lifecycle (#232)

The issue analysis correctly identified that `completion_review_required` was
latched after a malformed response and survived later valid tool plans. Recovery
now clears that stale protocol state after a valid legacy action batch. Trace
records the flag actually supplied to the current model request, not the updated
state for the next request.

This does not remove completion review. A later delivery candidate is reviewed
against the current evidence. Invalid complete_task declarations, failed tools,
missing required artifacts and prose-only stalled work remain rejected.
Experimentally accepting arbitrary prose in the planner made the existing
`legacy/commentary` regression falsely finish before its required source read;
that proposed change was discarded, not shipped.

The fixed-wire task-97 sequence now requires recovery on the first corrective
call, clearing after a valid plan, normal candidate review after subsequent
reads, and actual HTML write/readback. It completes in the same 12 requests with
two protocol failures instead of three. Live and persisted decisions are checked.
The 26 wire scenarios and 20 completion-review scenarios pass; negative cases
still fail. Review-count assertions now allow a cleared recovery stage instead
of requiring the recovery prompt on every remaining request.

Actual configured DeepSeek/legacy paged-source delivery on the changed Runtime
passed: 11 requests, 14 tools, zero model failures, 42.323 seconds, 105,702 input
+ 6,085 output = 111,787 tokens. Source markers, model-created HTML, readback
and final answer were independently checked. Raw report/artifact/log:
`build/live-review-lifecycle-fixed.*`. This is a same-class task, not execution
on the unavailable original maas-glm-5.1-zhipu endpoint.
