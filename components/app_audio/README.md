# app_audio: source bus and device output

`app_audio_bus.c` is portable C. It borrows a tracker, a synth, or both, with an optional sampler and mixes
their normalized float stereo output at 48 kHz. `app_audio.c` is the ESP-IDF
adapter: one DSP task renders the bus, a four-slot PCM ring transfers blocks to
one ES8311 writer, and the writer applies codec volume. Public dependencies are
`app_tracker_engine` (the `tracker_engine.h` facade) `app_synth` and `app_sampler`; BSP, codec, timer and FreeRTOS belong to the
device adapter only.

## Ownership and lifetime

Create sources first, create a bus with `app_audio_bus_create(tracker, synth)`,
then call `app_audio_init_bus(bus)` once from the startup task. A bus requires at
least one source. Its sources are borrowed and must outlive it. The device
adapter borrows the bus until reboot. Destroying a bus is legal only after all
render/control callers stop. There is no runtime device teardown API.

The legacy `app_audio_init(tracker)` creates and retains a tracker-only bus.
Repeating initialization of the same active bus/tracker succeeds; replacing an
active bus returns `ESP_ERR_INVALID_STATE`. Failed initialization closes the
opened codec and releases semaphores, tasks and any compatibility-owned bus so
the caller can retry. BSP retains ownership of its cached codec/I2S interfaces.
New tasks wait for startup notifications until both tasks and all resources
exist; failure rollback cannot interrupt a renderer holding a source lock.

## Rendering and controls

Exactly one audio caller owns `app_audio_bus_render`. It renders source blocks
into instance-owned scratch buffers, samples each source gain once
per public render call, adds the sources, then saturates once into interleaved
`int16_t` stereo. There is no render-time allocation. Source gains are finite
values from 0 to 2, initialized to 1. Invalid gain/source requests return false.
A zero source gain still advances the source's transport/envelopes. Nonfinite
source output becomes silence for that stereo frame and latches the clip flag.
Tracker renders in 512-frame device blocks so its source-local channel meters
retain transients from the whole block. Synth rendering and output/history
copies use 128-frame slices for prompt command consumption and short monitoring
locks. Larger host render requests use several 512-frame source blocks; tracker
meters then describe the final source block, while bus meters cover the complete
request. All scratch memory belongs to the bus instance, never the task stack.

Tracker play/pause and `app_audio_play/pause` control the tracker only. The audio
tasks continue to render so synth notes remain playable with the tracker paused.
The codec hardware volume, 0..100, is separate from source gain and digital bus
meters. Changing hardware volume does not change the recorded digital waveform.

The DSP task produces 512-frame blocks (10.67 ms at 48 kHz). Four ring slots
hold at most 42.67 ms of generated audio, plus any downstream codec/DMA buffering.
The existing ring therefore also bounds note/parameter response latency. Device
performance acceptance requires maximum voice count plus tracker playback and UI
interaction without output underruns; host tests do not establish that.

## Thread safety and monitoring

UI/control callers may set source gains and read monitoring concurrently with
the single renderer. Gains use atomic fixed-point storage (1/65536 resolution).
A priority-inheriting FreeRTOS mutex protects short output/history updates and
snapshots on the device; host builds use a C11 atomic boolean. Source rendering is
outside that mutex, so acquiring a monitoring snapshot does not nest source
locks. Host code must use a C11 compiler with atomics support.

`get_status` returns the latest completed render call's peak before saturation,
the corresponding 0..255 meter, a clip latch and the lifetime rendered frame
count. Values above 1 in `master_peak` expose digital overload. `consume_clip`
atomically clears the latch. The 2048-sample mono scope contains final saturated
output, oldest first, with zero history before the first samples. A short copy
returns the most recent requested samples. Status and waveform calls are each
coherent snapshots, not a single combined transaction.

Timing information travels with each PCM ring slot. After semaphore transfer,
only the writer reads that slot's DSP timing and owns the period totals/maxima;
there are no shared unsynchronized counter resets.
The five-second log also reports maximum writer wait for a ready PCM block and
the count of blocks whose wall time exceeded 512/48000 seconds. These times
include scheduling, source locks and interrupts; they are not isolated DSP CPU
measurements. A ready wait is also not direct observation of a DMA underrun.

## Host verification

Run `python tools/test_audio.py` for both native test executables below.

Build `tests/audio_bus_test.c`, `app_audio_bus.c`, the three `tracker_engine`
implementation files and the `app_synth` implementation files with their public
include directories, C11 atomics, and the platform math library where needed.
The test generates its own S3M fixture and verifies unclipped source output,
legacy output equivalence, gain before final clipping, transient source meters,
master clip consumption,
scope ordering, chunk equivalence, and synth playback while the tracker pauses.

Build `tests/audio_init_test.c` and `app_audio.c` separately, placing
`components/app_audio/tests/host_stubs` first in the include search path, along
with the three public component include directories. This fault-injection test
uses no device: it fails each of the eight codec/semaphore/task setup stages,
checks both initialization entry points release their partial resources, retries,
then verifies that successful initialization notifies tasks only after every
resource exists. Compile these as separate test executables; the init test's fake
source bus is intentionally not linked with `app_audio_bus.c` or real engines.
MSVC builds use `/std:c11 /experimental:c11atomics`.

The ESP-IDF adapter is validated separately by the pinned firmware build. The
host shims cover resource ownership and call order, not driver/DMA behavior.

## Sampler source and project edit gate

`APP_AUDIO_SOURCE_SAMPLER` is a third gain-controlled source. Bind a borrowed
sampler with `app_audio_bus_set_sampler` before the first render. The source uses
its own 128-frame float scratch buffer and participates in the same sum and
single final saturation as tracker/synth, including master scope and meters.
It handles audition and explicitly enabled input monitoring; the bus never owns
microphone I/O. Source gains retain their 0..2 range and project persistence must
include all three sources.

The bus owns a render transaction mutex (C11 atomic try-lock on native hosts).
`app_audio_bus_begin_edit(bus, timeout_ms)` is a worker-side bounded gate;
`end_edit` releases it. An audio block encountering a held gate produces silence
without advancing any source. A project worker stages allocations first, then
holds this gate only while committing prepared tracker/bank state and coherent
mixer/synth controls. Rendering owns the gate for one public render call. No
source or UI call should recursively acquire it. Startup binding also takes the
gate and rejects changes after rendering starts. Destroy requires all render
and project callers to have stopped.

No tests, build, simulator or device validation were run for these additions,
as requested by the user.
