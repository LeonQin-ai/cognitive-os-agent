# IM context replay duplicates an already supplied transcript

Every IM role turn receives the canonical group transcript and original human
request. The same role also had prior synthetic requests replayed as recent
chat history. Those requests each contained another entire group transcript,
so later rounds repeatedly loaded overlapping conversation context.

The reciprocal discussion fixture now rejects a separate `Conversation history`
section in model-facing IM requests. Before the fix it failed at round two;
after the fix all reciprocal, consensus, member failure, cancellation,
superseding human message, room deletion, limits and restart cases passed.
The same lifecycle suite now runs against both explicit legacy fixtures and
native `complete_task` responses, including checks for a native tool catalog.

An explicit `reasoning_set_external_history` flag suppresses only this redundant
recent-history injection while the IM caller supplies its transcript. The flag
is cleared before returning the lane to ordinary chats. Persisted history,
summaries, tool notes and long-term memory/retrieval are unchanged; this does
not assert that every form of repeated retrieval is eliminated.

The opt-in real API five-round, three-member discussion is used to validate
fifteen reciprocal replies and exact usage accounting. Token comparisons across
real runs are observations, not a controlled claim about model reasoning quality
or a guarantee of a particular savings percentage.

Real configured `deepseek-v4.1-flash` runs both completed all 5 rounds / 15
messages in 15 requests without model failures. Before: 130,905 input + 7,426
output/reasoning = 138,331 tokens (69.7s). After: 120,759 input + 9,201
output/reasoning = 129,960 tokens (82.086s). Input decreased by 10,146 tokens,
while generated content and latency varied. The deterministic fixture additionally
verifies ordinary chat still receives its own history after an IM turn reuses
the lane. These facts do not establish fixed cost/latency savings for all rooms.
