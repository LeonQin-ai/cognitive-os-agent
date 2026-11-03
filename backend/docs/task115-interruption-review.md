# Task 115 interruption and durable progress

The installed Runtime recorded task 115 starting and later becoming INTERRUPTED
at restart, about 78 minutes apart. Its request concerned the background theme.
No durable pre-restart spans, tool counts or usage survived. Therefore this
evidence cannot establish an idle loop, a completed artifact or its token cost.
The installed Runtime was not restarted and the user's theme edits were preserved.

The observability defect is fixed with two alternating per-task progress files
under the private state journal directory. Each contains a validated task ID,
sequence and the existing sanitized, 128-event-bounded timeline. New snapshots
are written at model/tool boundaries and at most once per second otherwise.
A truncated write leaves the previous slot usable. Successful terminal journal
append retires both slots. Startup carries the newest valid trace into the
INTERRUPTED record before retiring its checkpoints.

Usage fields now survive in that timeline. They are explicitly **last known**;
an in-flight model request can still be billed without returning usage before
the process dies. Checkpoints do not automatically replay external actions or
mark a task complete. This protects process-crash diagnostics, not power-loss
transaction durability.

Validation includes actual child-process termination during a held API request,
restart, restored tool/usage evidence, snapshot rollback, explicit resume and a
following successful task. A deterministic unit case truncates the newest
checkpoint and verifies fallback to the preceding complete snapshot.
