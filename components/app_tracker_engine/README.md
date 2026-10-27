# app_tracker_engine: portable S3M sequencer

The `tracker_engine.h` facade is the S3M audio-domain model for OP-Ay1. Its
component directory is `app_tracker_engine`; the existing C symbols remain
compatible. It is portable C with no
GSP, FreeRTOS, codec, or BSP dependencies. The public header is a stable facade
used by `app_tracker` and `app_audio`.

## Architecture

- `tracker_module.c` validates and decodes the caller-owned S3M blob into an
  immutable song model. It owns format bounds checks, pattern expansion, sample
  metadata, and channel mapping. Pattern cells are decoded into slot order
  (0..channel_count-1); the S3M source channel is mapped at decode time, so the
  engine and the mixer only ever index slots that exist.
- `tracker_engine.c` owns transport and event sequencing: order/row/tick
  progression, speed and tempo effects, portamento, slides, public lifecycle,
  and cross-task state ownership. It also implements the pattern-pad launcher:
  one pattern loops, and a pad tapped while playing is applied at the next
  pattern boundary (the order list resumes on stop/restart).
- `tracker_mixer.c` owns voices, linear-interpolated sample playback, loop
  wrapping, stereo panning, channel gain/mute/solo, clipping, and waveform or
  peak monitoring.
- `tracker_internal.h` defines the private composition and data contracts. Do
  not include it outside this component.

The dependency direction is `tracker_engine -> tracker_module/tracker_mixer`.
Module and mixer units do not call the public engine API or each other's
lifecycle interfaces. This keeps future DSP work independent of S3M parsing and
device transport.

## Threading

`tracker_engine_render` is the real-time audio entry point. The engine state is
guarded by one lock: a priority-inheriting FreeRTOS mutex on the device and a
single-threaded atomic flag on the host. The mutex matters because the audio
task (priority 7) and the GSP UI task share a core and the UI reads engine state
while rendering the pattern view; a spin lock let the audio task preempt the
holder and spin forever in priority inversion. The audio task renders 512-frame
chunks (about 10.7 ms at 48 kHz), so the lock bounds UI access without allowing
partial transport or mixer state to be observed.

Status publication and reads use the same source lock for a coherent short
copy; a plain-struct seqlock would still have a C data race. `playing` remains
an atomic fast-path flag on the device. The
source S3M blob and decoded pattern cells remain immutable after creation;
order-list entries are the only mutable song data and are accessed under the
same lock.

The engine keeps fixed 48 kHz stereo output. `app_audio`, not this component,
owns the DSP task, chunk ring, codec writer task, and output volume.

`tracker_engine_render_float` advances the same sequencer and voice state as
the compatible `int16_t` entry point. Its normalized stereo output can exceed
plus/minus one: no source clipping occurs before the audio bus applies source
gain, adds synth output and saturates the final signal. Call one render entry
point per block from the single audio owner; calling both advances time twice.
Monitoring remains source-local in both paths. Final mixed output meters and
scope belong to `app_audio_bus`. Either output path zero-fills a stopped or
invalid-tick tail instead of exposing uninitialized samples.
