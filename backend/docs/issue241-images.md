# Image readback and chat delivery (#241)

`file_read` previously treated image bytes as failed text extraction. A generated
image could therefore never satisfy write/readback acceptance, and a plain local
path in the model's answer could not become a browser image URL.

PNG/JPEG/WebP files up to 8 MiB now produce a format-aware read receipt. The full
image is copied to session-scoped attachment storage; only an opaque preview ID
and metadata enter progress/history. A bounded cache prevents repeated reads from
creating fresh preview IDs or fake progress. Image receipts are excluded from the
text-page retrieval cache. Actual image observations are sent to the next model
request using the existing vision adapter; base64 is not copied into the answer
or persisted tool trace.

The chat renders backend-validated preview descriptors next to the assistant's
answer, even when the model only returns a local filename. Preview IDs remain
scoped to their session and survive redraw, step rollover, and history replay.
Arbitrary JSON in text-file output cannot create a validated preview descriptor.

Tests cover existing PNG/JPEG, cached variants, script-generated images, invalid
images, raw payloads in OpenAI native/legacy and Anthropic requests, ownership
denial, direct chat rendering, zoom, mobile layout and redraw.

One configured native DeepSeek v4.1 Flash run generated a 422-byte PNG through an
Agent-authored Python script and a real shell command, then used file_read. An
independent decoder verified 240x120 dimensions, left red and right blue pixels,
and byte equality between the artifact and its scoped preview. It completed in
19.626 seconds, 7 requests, with 44,749 prompt + 1,822 visible completion + 570
reasoning = 47,141 tokens. Logs/report/image are retained under ignored
build/issue241-live-image.*. No original production task or application was
restarted for this validation.
