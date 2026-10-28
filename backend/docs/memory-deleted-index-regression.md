# Deleted records survive derived vector index rebuild

Reproduced on main 5992fcd: put a canonical record, rebuild its vector mirror,
remove the canonical record, rebuild again, then query the vector store. The
deleted `r:<id>` entry and text remain queryable.

Cause: `mem_records_rebuild_index` removed only IDs still present in the
canonical store. Deleted IDs are absent from that list, so their derived entries
were never removed.

Fix: remove the dedicated `r:` namespace before rebuilding live records. Other
namespaces are retained. This preserves working/episode/document vectors while
making the canonical store authoritative for record mirrors.

Regression: `test_memory_deleted_index` checks the real put/rebuild/remove/
rebuild/query sequence and verifies an independent `w:keep` vector survives.
The deletion assertion failed before the fix. It is in the full unit suite and
the focused `--memory-cache-updates` / `--memory-lifecycle` suites. This does not
claim that arbitrary concurrent memory transactions or crash recovery passed.
