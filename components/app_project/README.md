# app_project

Owns one global musical project and its background file jobs. Launcher, tracker,
synth and sampler borrow the same stable engine/source handles. Opening the
already active saved ID is a no-op: no reload, rewind, note reset or loss of
unsaved edits. Navigation and transport are not edits. `main` supplies sources,
an optional note router, the borrowed built-in demo bytes and an optional host
filesystem root; no UI component or private source header is included.

## Public contract and ownership

`app_project_create` starts one IO worker. Device stack is 20 KiB so filesystem
paths and NAND initialization do not consume UI/audio stack space; larger
snapshots, blobs, codecs, manifests, LittleFS probe objects and PCM remain heap
owned. Windows uses an owned worker/event/critical section; POSIX uses pthread
mutex/condition/thread. Mounting and first sampler-bank extraction happen on
that worker, never a callback. Boot stages the real ARYX demo, or an independent
blank S3M if no demo is supplied. It is an unsaved session, never an automatic
filesystem save or silent reopen of an old project. Mount failure still permits
the in-memory session and reports storage unavailable.

UI owns a heap/context `app_project_snapshot_t`, then polls `app_project_read`.
It is a zero-wait coherent copy; BUSY leaves the previous snapshot intact.
Lists contain actual catalog/inbox files, bounded at 32 with explicit truncation.
`storage_usage_valid` is false if capacity is unknown. Host disk-wide capacity
is never presented as onboard NAND free space. No file IO runs while holding
the snapshot/admission mutex.

Before changing source state, UI calls `app_project_begin_edit`, makes only
short public source mutations, then always calls `app_project_end_edit(changed)`.
Job admission rejects an active edit and publishes busy synchronously before
the worker can capture or replace state. The begin/end pair is not a long-held
mutex: it is explicit session admission. `app_project_set_controls` inside it
updates saved master volume and Pad references; device brightness/orientation
remain separate device preferences. `note_edit` is a convenience for completed
metadata-only edits, not a substitute for admission around source mutation.
Sampler revision changes are also observed, including applied/kept recordings.

Requests are copied into one bounded in-flight slot; no hidden queue or UI wait.
`operation_generation` advances on admission and `session_generation` only on
committed replacement/new sessions. Source reload resets queued Pad launches,
audition/edit state and old note-router ownership only after all staging succeeds.
Incoming failures retain the complete current session and unsaved status.

`NEW_BLANK`, `IMPORT_ARYX`, `OPEN` and `IMPORT_MODULE` reject dirty replacement
unless the caller sets `discard_dirty` after its own user confirmation. Save
creates an ID for an unsaved session; Save As always creates a new ID/name and
does not overwrite the previous ID. UI freezes musical editing while busy;
large sampler Apply/Undo/Assign and recording-start/Keep are worker actions.
Recording/takes must be stopped and kept/discarded before replacing or saving.

Stop request/read/edit callers and drain any Iris inbox streams before
`app_project_shutdown`. A timeout retains the service, thread/platform resources
and all source borrows for a later retry. Pending IO completes before exit; NAND
teardown errors also retain its owner until retry succeeds. No caller may destroy
the tracker/synth/bus/sampler/router/demo while shutdown still retains the handle.

## Actual project storage

On device the only owned namespace is `/nand/apps/op-ay1`, with `projects/<id>`
and `inbox`. `app_nand` mounts the existing LittleFS without format or expansion.
Vibe Mode's Ideas, system update bundles and unrelated files are never modified.
Host adapters must supply an actual local root, e.g. `build-sim/project-data`.
`app_project_inbox_ready` is the small admission query for a deferred Iris
inbox-only volume. Never expose writable `projects/` or all of `/nand` through it.

Each immutable generation contains:

- `module.s3m`: owned source bytes with edited order-list overlay.
- `samples.bank`: actual signed-PCM sample library, original names/rates,
  trim/loop/gain metadata and explicit tracker-slot assignments.
- `state.bin`: versioned explicit little-endian scalar synth patch, mixer
  gain/mute/solo, source gains, speed/loop, master volume and Pad references.
- `manifest.bin`: ID/name/revision/generation, lengths and CRC32 for all files,
  plus its own CRC. No absolute host/device source path is serialized.

Blank projects have one genuinely empty 64-row pattern and 16 empty assignable
sample slots, with none of ARYX's music/title/sample data. ARYX import copies
the built-in module and extracts its actual samples into the new session.
S3M/WAV inbox imports accept basenames only. Imported WAV PCM becomes owned bank
data; future playback never depends on the inbox file or a computer path.

Save creates a fresh generation, writes/checks every IO result, flushes/fsyncs
and closes files, then independently rereads their CRC with a reusable 4 KiB
buffer. Finally it atomically replaces the small `CURRENT` pointer. The prior
pointer is retained as `PREVIOUS` before commit. Load verifies manifests, lengths,
CRCs and binary bounds; a damaged/missing current generation may fall back to
the prior generation. Source decode and sampler assignment allocations complete
before the audio-bus transaction gate. Synth patch submission is the final
fallible step; stable engine/bank commits are no-failure operations under that
gate. Audio gets silence during the short commit rather than a half-replaced bank.

An error or no-space result never deletes the active generation. Incomplete
generations remain unreferenced; this initial implementation deliberately does
not prune old generations or delete user files. Catalog checks manifest integrity;
complete asset CRC/decode occurs on open. No multi-file rename is represented as
a filesystem-wide transaction. CRC detects corruption and is not authentication.

Large loads need old and staged PCM in memory at once; allocation failure is
explicit and preserves the old project. Encoded bank bytes are released after
decode; save verification does not allocate a second full bank. Storage IO and
capture are not claimed to satisfy device real-time, power-loss or physical
microphone acceptance merely from source inspection.

This change adds no test code and performs no compilation, simulator run,
filesystem/device operation, firmware update or format. Public API/source review
is the validation boundary for this prototype implementation request.
