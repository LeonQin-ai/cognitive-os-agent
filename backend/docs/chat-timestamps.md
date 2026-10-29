# Chat dates and times (#234)

User and assistant messages display a local date and time, including seconds.
The `<time>` element stores an ISO timestamp and its tooltip identifies the
browser time zone. New messages capture their creation time; finished assistant
answers capture completion time. Progress updates do not keep changing it.

History uses the persisted task-journal timestamp for the assistant and the
recorded execution start for the user when available. Browser session caches
also retain timestamps across redraw and restoration. Older history without a
matching dated task explicitly displays “时间未记录”; rendering never invents a
current timestamp for an old message.

The source UI and generated embedded header are synchronized. Browser regression
checks a fixed instant in Asia/Shanghai, different start/finish instants, cache
restore, historical missing dates, new messages, completed answer redraw, mobile
width, and retaining the answer copy action. The existing full-answer clipboard
and IM-capacity cancellation UI regressions also pass. The new browser test is
included in the Windows browser CI step. Workflow lint and diff checks pass.

This is a UI addition using existing journal data; it does not backfill dates
that were never saved, nor guarantee timestamps after the corresponding journal
and browser cache have both been removed.
