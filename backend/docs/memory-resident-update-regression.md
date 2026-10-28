# Resident Context MMU records stay stale after updates

Reproduced on main 5992fcd through the C Memory OS interfaces. A record is
recalled into HOT, replaced under the same ID, then recalled again: the old body
is returned. Growing the body beyond the detail budget also leaves the old HOT
representation resident. A WARM overview can retain an old title/evidence after
a metadata-only update.

Cause: invalidation existed only in `materialize`, which HOT pages skip, and
its hash covered only body content. Cached L1 includes title, scope, type,
status, importance/confidence/utility and evidence as well.

Fix: fingerprint all representation inputs at page synchronization, release the
old resident token charge before clearing representations, and re-enter normal
budget-aware promotion. Ranking/access timestamps do not invalidate text.

Regression: `--memory-cache-updates` checks changed HOT bodies, HOT-to-WARM
growth, metadata-only overview edits, and exact resident token accounting.
Three update assertions failed before the fix. No user/answer keyword rules or
new output-token cap were added. The C API regression proves this component
behavior; the real model episode tests exercise a different retrieval path and
must not be described as proof of MMU update correctness.
