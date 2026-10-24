# tracker_engine: portable S3M sequencer

`tracker_engine` is the audio-domain model for ARYX. It is portable C with no
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
  and cross-task state ownership.
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

The status snapshot uses a seqlock for lock-free UI polling. Publishing is
serialized with `state_lock`; `playing` remains an atomic fast-path flag. The
source S3M blob and decoded pattern cells remain immutable after creation;
order-list entries are the only mutable song data and are accessed under the
same lock.

The engine keeps fixed 48 kHz stereo output. `app_audio`, not this component,
owns the DSP task, chunk ring, codec writer task, and output volume.
