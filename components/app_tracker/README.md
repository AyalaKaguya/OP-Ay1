# app_tracker: shared workstation UI

This component owns the six pages, their RGB565 rendering and interaction
state. Device and native simulator use the same `tracker_ui.c`,
`tracker_model.c`, `tracker_synth.c`, graphics and page renderers. GSP supplies
one opaque 480x480 Canvas, mapped pointer samples and 50 ms timers. The authored
entry is `ui/main.json`; generated bundles and bindings belong in the build
directory, not beside the source.

## Responsibilities and dependencies

- `tracker_ui.c` adapts the public GSP Canvas/input/timer API and owns two
  reusable 480x480 frames plus scope scratch. It publishes only complete frames.
- `tracker_model.c` owns transport, mixer mute/solo, pad selection, input
  capture, navigation, parameter retry and the render snapshots.
- `tracker_graphics.c` contains the shared palette, exact 5x7 glyph drawing,
  status/order bars and clipped primitives. `tracker_render.c` retains the
  original pattern/waveform pages; `tracker_pages.c` draws Playback/Mixer/Pads.
- `tracker_synth.c` owns the synth page geometry, parameter mapping and keyboard
  hit testing. It is independent of FreeRTOS, BSP and codec output.
- `tracker_device.c` is the device adapter: PSRAM allocation, clock, codec
  volume, hardware-button request posting, queued Iris pointer input and the
  independent stall watchdog.
- `host/` is the native sim_bridge adapter. It advances the real source bus,
  optionally writes a WAV/plays Windows audio and records read-only snapshots
  for input-based acceptance tests.

Only public APIs from `app_tracker_engine` (`tracker_engine.h`), `app_synth` and `app_audio_bus` are used.
No component private structures or mutable globals cross their boundaries.

## Ownership, threads and lifetime

`app_tracker_create(ui, &config, &owner)` borrows its sources and copies its
hooks. The hooks' context and every source must remain valid until successful
stop. Create is called once by the composition task, before user input; frames
and model are fully initialized before callbacks are published. A ready flag
keeps an early callback idle during registration. After publication, one GSP UI
thread owns model changes and frame production/drawing. The two frames do not
make arbitrary concurrent rendering safe.

`app_tracker_read` is a UI-thread snapshot. Physical button tasks call only
`app_tracker_request_transport`, which atomically posts a request for the UI
timer. Engine/source/bus public methods own their separate audio concurrency.
Pointer callbacks trigger notes immediately; they do not wait for the 50 ms
timer. Controls redraw at up to 10 Hz when no input is active, scopes at 20 Hz.

Host stop is serialized with callbacks. Device stop must run from an external
control task, never a GSP render callback: it disables input/timers, stops the
Canvas and successfully fences selected callbacks before releasing memory.
If detach or the 2-second fence fails, `app_tracker_stop(&owner)` leaves `owner`
non-NULL; keep its borrowed resources alive and retry later. The same retention
rule applies to the unusual create-failure rollback that cannot be fenced.
Routine argument/allocation failures return a NULL output.

The device entry `app_tracker_start_bus` is a once-per-boot adapter. The button
driver has no callback-completion fence, so its handles and a small inert
service context stay alive until reboot after UI stop. Buttons post only into
that retained context, never into a freed UI. Its watchdog is disabled and
stopped with the pinned SDK's blocking stop before deletion. The normal product
UI/audio lifetime is until reboot; there is no runtime restart API.

The native backend also has a single process lifecycle. It tears down the app
before closing the bridge on normal duration exit. After transport loss it
preserves a context whose detach could not be confirmed until process exit.

## Interaction contracts

Page order: WAVES, PATTERN, PLAYBACK, MIXER, SECTION PADS, SYNTH. Navigation dots
have wider invisible touch zones. Horizontal drags outside captured controls
switch pages. The mixer decides between vertical scrolling and horizontal
fader movement; its right gutter is reserved for scrolling. Rows snap at
66-pixel boundaries after momentum. Blank tracker channels cannot change gain,
mute or solo.

Playback controls the song, with restart pausing/rewinding and play leaving pad
mode. The AI button keeps its original semantics: short press toggles the
current transport, long press pauses/rewinds. Synth performance is independent
of song pause. Mixer gain remains 100=0dB, 120=+6dB, 50=-15dB and 0=silent;
MASTER remains 0..100% codec volume in steps of 5. The digital master meter is
the final tracker+synth bus before codec volume.

Pads are a 4x4 Pattern launcher. First launch starts playback. A different pad
queues a pattern-boundary switch; tapping the running pad queues a stop,
tapping again cancels it. Empty slots stay inert. Pending flashes follow the
engine frame clock, effective tempo and playback multiplier.

The synth keyboard supports one pointer and glissando, not independent
multitouch chords. Key-up, leaving the keyboard, changing page and stop release
notes; PANIC clears voices at the next audio render. The upstream input adapter
must map cancellation/focus loss to release. A 30-second missing-sample guard
is a last fallback, not a guarantee about browser/device focus behavior.
Failed patch submissions retain the latest edit and retry once per UI tick;
BUSY clears when the bounded source queue accepts it.

## Device diagnostics

The device adapter registers Iris's standard pointer/v1 profile `0x1001/1`.
Its 12-byte little-endian payload is `<BBhhHI>`: phase (0 down, 1 move, 2 up),
reserved byte, x/y pixels, reserved word and sequence. Coordinates are 0..479.
RPC tasks enqueue into a fixed 32-event queue; the UI thread drains it through
the same controller input path as mapped touch. Any queue admission failure
also requests UI cancellation, so an aborted remote gesture cannot leave a
held note behind. An accepted response acknowledges enqueue, not display or
sound completion; observe the subsequent state.

Read-only service `0x2101/1` takes an empty payload and returns UTF-8 JSON:
page, volume, scroll, held note, tab/preset, song and pad state, active voices,
synth source peak, rendered frames, four visible channel gains/M/S and the last
GSP error. The snapshot is copied under a short priority-inheriting mutex,
published at 50 ms UI cadence and unavailable before the first valid snapshot
or after shutdown. Hardware-button state may appear one tick later. Pair this
snapshot with live Iris Device ID/Boot ID and firmware SHA for acceptance;
read-only fields alone do not establish identity or acoustic output.

Use only the product entry for these services:

```sh
python mosaico.py iris rpc 8449 1 --project projects/OP-Ay1 --json
python mosaico.py iris rpc 4097 1 --payload-hex HEX --project projects/OP-Ay1 --json
```

These are inspection/input interfaces, not a second firmware writer. All
installations still use Vibe Mode and the project's System Update bundle.

Diagnostic method `0x2101/2` auditions four notes for 100..10000 ms, then
automatically releases them. Its eight-byte little-endian payload is
`<BBBBHH>`: four MIDI notes (0..127), velocity (1..127), duration in ms.
It posts a one-entry request to the UI task, uses the normal synth command
channel and fails explicitly if busy. It exists to measure target polyphony
alongside song playback; it does not add multitouch support or a sequencer.

## Native verification

Resolve ESP-GSP 1.5.1, then activate a native C compiler and run from the
workspace root:

```sh
python mosaico.py project sim --project projects/OP-Ay1 --headless --duration 3
python mosaico.py project sim --project projects/OP-Ay1 --interactive
```

On Windows use a Visual Studio x64 development environment with CMake/Ninja.
`host/CMakeLists.txt` uses the official `gsp_add_backend`; it compiles the shared
code rather than a second UI implementation. `OP_AY1_SIM_AUDIO=1` enables
Windows speaker output, `OP_AY1_SIM_WAV=PATH` captures bus PCM, and
`OP_AY1_SIM_STATE_LOG=PATH` enables read-only NDJSON snapshots. These are host
diagnostics, never product controls. The default sink is silent.
`OP_AY1_SIM_STOP_FILE=PATH` permits an external host test to request orderly
application cleanup by creating that file before quitting the simulator.

From the application directory:

```sh
python tools/test_synth.py
python tools/test_ui_renderer.py
python tools/test_ui_interaction.py
python tools/test_ui_sim.py --sim /path/to/pinned/1.5.1/sim
```

The simulator test requires an already-built `build-sim/` backend. It uses
press/move/release RPC input, screenshot captures and independent source-state
and PCM observations. Renderer tests protect bounds, all 793 scroll offsets and
the original first two pages. Input tests advance the actual audio bus through
pad boundaries and synth envelopes. Native output does not establish device
DMA timing, touch latency, focus cancellation or real-time headroom.
