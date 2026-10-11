# Code review fixes and verification — 2026-10-11

The fixes described here are included in v0.8.0.
Pre-existing execution progress UI changes have been preserved.

## Fixes

| Issues | Implementation | Verification |
|---|---|---|
| #73, #74, #77 | At most 16 concurrent connection readers, serialized application handlers, 30-second total request deadline, interruptible I/O, bounded headers and body-sized allocations; strict case-insensitive framing | Real slow/normal simultaneous TCP connections, lowercase length, malformed/duplicate length, unsupported chunked transfer, oversized length, native stop check |
| #75 | Wait for worker exit notifications against one deadline before joining; preserve resources when shutdown times out | Blocked runner, timed out shutdown, safe release and retry |
| #76 | Reference-based task lookup, compact sorted ID index, configurable retained task count/bytes; release large attachment snapshots after completion | 100 completion cycles with bounded history; live API eviction and durable journal query |
| #78 | Immutable session-validated attachment snapshots passed into planner messages and both image-capable adapters | Capture actual OpenAI image_url and Anthropic base64 messages and compare original image bytes |
| #79 | PDF and Office text extraction through a bounded direct Python subprocess; ready/failed/unsupported status; unusable attachments rejected at chat submission | PDF/DOCX/XLSX/PPTX extraction and actual provider payload, corrupt document and unsupported binary |
| #80–#85 | Unique file IDs and manifests, immediate index deletion, replacement cleanup, bounded UTF-8 chunks without filename truncation, persisted extracted text and consistent restart handling; pre-ID file migration | Native retrieval checks, immutable same-name uploads, long names, size limit, restart and legacy migration |
| #86, #87 | Session-specific drafts; pending uploads awaited before submission; submission snapshots retain their original session | Playwright switching before/after upload and upload-before-send |
| #88 | Persistent bottom-follow state, scroll after layout, preserve manual history reading | Desktop 1280×800 and mobile 390×844 Playwright checks |
| #89 | Windows socket handles are not inherited by child processes | Repeated full document upload/provider/browser runs without the observed connection reset |

## Runtime limits and behavior

Subsequent multimedia expansion is documented in [attachment-formats.md](attachment-formats.md); the earlier 42-check result below records the initial review checkpoint. Current limits are updated below.

- Attachments: at most 64 MiB each and four per chat submission. IDs identify immutable stored bytes; duplicate names do not replace old files. Delete using the ID. Unique legacy names remain accepted for deletion.
- Supported images: PNG/JPEG/WebP/GIF plus normalized BMP/TIFF/HEIC/HEIF/AVIF/SVG; animated images may be sampled. Actual visual understanding requires a model that supports the relevant image format; capture tests verify transport, not model recognition quality.
- Office text extraction uses ZIP/XML support; Word/PPT page rendering and legacy formats use installed Office/LibreOffice. PDF rendering uses PyMuPDF; pypdf is a text-only fallback. `COA_DOCUMENT_PYTHON` can name a Python executable. Missing dependencies, corrupt files and PDFs requiring OCR produce a visible failed status. Scanned pages are supplied to the visual model; no local OCR text index is generated.
- Extraction: 60-second process limit, 64 MB total Office archive expansion limit, one million extracted characters, and bounded captured output. Text shown to the planner is a preview of up to 32,000 bytes per document; the extracted file path is provided for reading additional content.
- `scheduler.history.count` defaults to 1024 and `scheduler.history.bytes` to 16777216; zero disables that limit. `/v1/scheduler` reports retained count/bytes and limits. Active tasks are never evicted. Temporary acquired references defer eviction. The compatibility `scheduler_get` API pins borrowed tasks until free; production code uses acquire/release instead.
- Completed-task eviction affects `/v1/tasks/<id>`; `/v1/tasks/journal` retains durable history. Already accepted attachment content remains in the running task's immutable snapshot even if the stored attachment is subsequently deleted.
- Application handlers default to serialization; attachment upload processing explicitly runs concurrently, with at most two processors, so it does not block task queries.

## Reproduce on Windows

```powershell
./backend/build.ps1 -Target all
./backend/build/test-review.exe
python backend/tests/test_upload_e2e.py
```

The browser tests use Playwright from `backend/webarena/node_modules` and Edge.
The end-to-end runner starts its own local capture provider and runtime in a
temporary state directory. No paid model requests or real API keys are used.
Legacy adapter/e2e tests additionally use `mock-llm-server.exe 9000`.

## Results

- Full Windows build succeeded.
- Changed HTTP/attachment/planner/scheduler and POSIX socket/process modules compiled to Linux objects with Zig; Linux runtime tests were not executed.
- Unit suite: 2750 passed, 0 failed.
- Scenario suite: 91 passed, 0 failed.
- Native review regression: 236 assertions passed, 0 failed.
- New HTTP/provider/persistence/browser end-to-end suite: 42 checks passed.
- Existing adapter suite: ADAPTER PASS (both providers, chat/images/SSE).
- Existing agent end-to-end suite: E2E PASS (both providers).

Logs are generated under `backend/build/review-*.log`, with machine-readable
end-to-end results in `backend/build/review-e2e.json`. Browser screenshots are
in `backend/build/ui-progress-qa/`, including `thinking-follow-1280.png` and
`thinking-follow-390.png`. These build artifacts are not tracked in Git.

Windows is the platform on which the runtime and browser tests were executed.
