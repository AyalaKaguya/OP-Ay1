# app_sampler

`app_sampler` owns the project sample bank, non-destructive edit drafts, one
audition voice and an optional real microphone capture worker. It depends on the
portable tracker facade. Only the device capture adapter depends on the BSP,
codec and FreeRTOS. It does not own the project filesystem, project UI, tracker,
audio bus or USB/MIDI configuration.

## Bank and formats

The bank holds up to 64 indexed samples and 16 MiB of canonical signed 16-bit
mono PCM. Each sample has its original source and name, frame count, source
rate, editable C2 reference playback rate, trim and forward-loop bounds,
linear gain, original instrument volume, original S3M slot and optional explicitly assigned S3M slot. Bounds
are frames with an exclusive end. An empty S3M instrument remains a visible
zero-frame entry; ARYX therefore has nine entries rather than eight.

S3M extraction retains the original slot order, names, C2 rate, default volume
and loop metadata. Unsigned 8-bit PCM converts with `(value - 128) * 256`, signed
8-bit uses `value * 256`, and 16-bit PCM preserves its full precision. Stereo
S3M input is planar and averages channels in int32. Packed/ADPCM and FM
instruments are rejected. S3M has no independent recording-rate field: its
`sample_rate` records the original C2 playback rate, which may be below 8 kHz.
The project keeps its original S3M file separately, including order/pattern
references, raw effect commands and parameters.

WAV import accepts little-endian RIFF PCM format 1, 8/16-bit mono or stereo,
8–192 kHz. Stereo interleaved channels average in int32. Unsupported encodings,
invalid chunk ranges/alignment, duplicate fmt/data chunks, excessive lengths
and truncated reads fail without appending. The file importer reads headers and
converts 256-frame chunks; it never loads a second complete WAV into memory.
One sample is limited to 2,880,000 frames and 60 source seconds (whichever is
smaller). Allocation failure leaves the current bank unchanged.

`AYSB` version 1 bank serialization uses explicit little-endian fields, a
16-byte header and 120-byte metadata records followed by signed PCM16. It never
serializes C structure padding or pointers. The project service owns generation
checksums and durable file commits. Decoding checks all metadata and sample
ranges, maximum bank bytes and unique assigned slots before returning a staged
bank. Snapshots retain immutable PCM with reference counts; encoding does not
hold the audio lock. Returned encoded bytes use the normal `free()` contract.

## Ownership and editing

Create the sampler and bind its borrowed stable engine before rendering. Attach
it to the audio bus with `app_audio_bus_set_sampler` before the first render.
The bus calls `app_sampler_render` as the only audition/monitor audio owner at
48 kHz. The sampler never calls an audio bus API or writes the speaker codec.

One UI controller owns an edit draft. Begin/get/set/cancel operate only on
metadata. Audition during editing uses the draft while the committed bank stays
unchanged. Apply clones metadata, updates an explicitly assigned tracker slot
if present, and commits only after all fallible work succeeds. Cancel discards
the draft and audition. Undo restores the last applied edit, including its
assigned tracker PCM, and consumes the undo entry. Import, assignment and
project replacement clear undo rather than undoing an unrelated operation.

Original nonempty S3M entries begin assigned to their matching 1-based slot,
so Apply updates that original instrument. Mapping a new imported/recorded
sample or moving any existing mapping requires explicit assignment. Assignment
uses 1-based S3M slots. It deep-copies trimmed PCM
into the stable engine with adjusted loop bounds, preserving pattern cells,
volume commands and effects. A sample has one assigned slot; moving that
assignment atomically restores its old slot from the original S3M. Replacing a
slot removes the previous sample's assignment metadata. The tracker owns its
deep copy so audition/bank lifetime cannot invalidate a playing voice.

The project worker must serialize heavy operations and block competing UI
mutations. WAV import, assignment, Apply/Undo and record start are worker calls;
they can allocate or copy megabytes. Hold the audio bus edit gate during engine
replacement/combined project commits so the renderer emits silence without
waiting for those copies. Info, bounded waveform copies (maximum 2048 points),
draft controls, audition controls, status and Stop are UI-safe short calls.
KEEP transfers a retained immutable recording buffer into the bank; it does not
copy its PCM. Revision advances only for committed persistent bank changes.

Project load stages `tracker_engine_prepare`, `app_sampler_bank_prepare` and
`app_sampler_bank_apply_slots` before taking the bus gate. After staging, the
engine and bank commits consume their staged objects without allocation or
failure. Their handles remain stable for all borrowed UI/audio users. A
recording or unsaved take must be kept/discarded before switching projects.

## Recording

On device REC allocates at most 60 seconds of PCM in PSRAM, starts a worker and
opens the real BSP ES8311 microphone at 48 kHz, 16-bit stereo. It averages the
two I2S slots to mono in 512-frame chunks. Input gain is 0–42 dB in 6 dB steps.
The fixed format matches the active output clock; the BSP retains its cached
codec/I2S handles. Codec close releases the microphone's input reference while
the ES8311 shared hardware reference count retains the active output.

Monitoring defaults OFF. It can be armed explicitly and uses a bounded
4096-frame overwrite ring; it never changes recorded PCM. Stop requests finish
the current codec read, closes capture, shrinks the buffer when possible and
publishes READY. The worker also stops at the 60-second limit. READY supports
audition and KEEP/DISCARD; KEEP appends a recording sample, while DISCARD frees
it. An I/O failure publishes ERROR and retains its data until DISCARD. No fake
waveform or silent take is generated on a host: REC returns UNSUPPORTED and the
host still supports file import, editing and audition.

`app_sampler_shutdown(&sampler, timeout_ms)` requests stop and frees only after
the worker announces exit. A timed-out/blocked codec read returns false and
retains the entire instance for a later retry. Audio/UI callers must already be
stopped before shutdown. `destroy` is a convenience for that stopped-lifetime
case; callers needing a completion result should use `shutdown`.

No build, test, simulator or device validation was run for this implementation,
as requested. Runtime audio quality, capture wiring, shared-clock behavior,
PSRAM headroom and device performance remain unverified.
