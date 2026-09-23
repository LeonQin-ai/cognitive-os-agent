# Million-agent capacity: measured scope and next work (issue #63)

The runtime separates **registered virtual agents**, **queued tasks**, and
**physical executors**. A million agents must never mean a million threads.

Implemented now:

- The scheduler ready queue is a priority heap; lookup by task ID is direct.
- Executors expand from the configured initial worker count up to a bounded
  32-thread floor under concurrent load. Yielded coroutines resume on their
  original worker, which is required by macOS `ucontext` and Windows Fiber.
- The agent roster uses a hash index for average constant-time name lookup and
  registration. Deletion rebuilds the index because the public agent indexes
  retain their current compact-array semantics.
- Allocation failures reject submission without leaving gaps in task IDs or
  corrupting the ready queue.

Local Windows capacity checks (opt-in unit modes, run separately):

| Check | Result | Limit of evidence |
| --- | --- | --- |
| `COA_AGENT_STRESS_COUNT=1000000` | 1,000,000 agents registered, sampled lookups and removal passed; full unit suite 7,620/0 in 34.67 s. | Registry only; it does not start a model session for each agent. |
| `COA_SCHED_STRESS_COUNT=1000000` | 1,000,000 no-op virtual tasks submitted and completed; full unit suite 2,086/0 in 230.93 s, with about 312 MB observed peak working set. | Short-task throughput is still slow; this is not a million concurrent reasoning sessions. |

Remaining before the million-agent **system** target can be closed:

1. Reduce per-task coroutine creation/destruction and lock contention; repeat
   the million-task test with a throughput target and peak-memory budget.
2. Bound retention of completed `task` objects and traces while preserving
   durable lookup/history semantics, so a long-running server does not grow
   forever after repeated task waves.
3. Replace the current finite active chat-lane/session limits with an idle
   session eviction or paging policy, then load-test mixed active/idle agents.
4. Make worker scaling configurable and demand-based, including safe scale-down
   and admission/backpressure when model or tool capacity is exhausted.
5. Load-test multi-agent Flow, shared memory and API paging independently of
   the roster and task scheduler; those layers have smaller current limits.

These are capacity targets, not claims about current production throughput.
