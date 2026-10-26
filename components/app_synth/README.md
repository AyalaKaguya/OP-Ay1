# app_synth

Portable four-voice subtractive synthesizer for OP-Ay1. The component has no
ESP-IDF dependency. `main` owns the instance; the audio DSP task alone calls
`app_synth_render`; UI/control tasks use the public command API. Destroy the
instance only after those tasks have stopped using it. It is not a singleton.

The product rate is 48 kHz. Creation accepts 8–192 kHz, allocates the instance
and its fixed buffers once, and returns NULL for invalid rates or allocation
failure. Rendering never allocates, waits, logs, or calls board APIs.

The engine provides polyBLEP saw and square, triangle and sine, a generated
four-position wavetable with phase/position interpolation, a topology-preserving
two-pole low-pass, ADSR, and per-voice sine LFO modulation of pitch, cutoff or
amplitude. The tables are generated from harmonics at creation; no sound asset
or external table is required. Continuous patch parameters and waveform weights
smooth over 10 ms. Filter/pitch coefficients update every 32 samples on a
persistent sample clock, so splitting a render call does not change the audio.

`app_synth_patch_for_preset` returns INIT, BASS, LEAD or PAD. The public header
lists all control units and ranges. `set_patch` clamps out-of-range values and
replaces nonfinite values with the lower bound. It copies the input. A note uses
MIDI pitch 0–127 and linear velocity 0–1; velocity zero releases that note.
Retriggering an active pitch reuses its voice. When all four voices are busy,
the engine first steals the quietest released voice, otherwise the oldest held
voice. Note-off releases every voice for that pitch. All-notes-off uses the
release envelope; panic hard-resets all voices.

Controls append to a 64-command ring protected by a bounded atomic try-lock.
The render task tries the ring once per call and consumes at most 64 commands.
A busy or full ring rejects note-on/patch changes and reports false; callers can
retry a patch on a later UI update. `note_off` always retains a valid release by
using an atomic per-note mask when the ring is unavailable. All-notes-off has
its own emergency flag, and panic always uses an emergency flag. Render applies
emergencies after earlier queued commands, preventing queued note-ons from
resurrecting released notes. No producer spins or mutates voice DSP state.

`get_patch` and `get_status` use atomic words and bounded sequence checks. They
return true only after copying one coherent snapshot to the caller. Invalid
arguments or four unsuccessful read attempts return false without changing the
caller's output; keep the last successful snapshot and retry on a later update.
Patch snapshots describe the last accepted control patch; DSP/meter snapshots
describe the last render call. Independently sampled queue counters are live
command diagnostics. Ring drops include commands retained by the emergency path.
No C11 64-bit atomic is required on the device.

Output is interleaved stereo float, in PCM full-scale units. It overwrites the
caller buffer. Finite source headroom above +/-1 is retained for the shared audio
bus to mix and saturate once at the final int16 boundary. `peak`/`clipped` report
that source headroom and must not replace the final bus meter. NULL synth renders
silence; NULL output is ignored.

Minimal use:

```c
app_synth_t *synth = app_synth_create(APP_SYNTH_SAMPLE_RATE);
if (synth != NULL) {
    app_synth_set_patch_id(synth, APP_SYNTH_PATCH_LEAD);
    app_synth_note_on(synth, 69, 0.8f);
    /* Audio task: app_synth_render(synth, caller_stereo_buffer, frame_count); */
    app_synth_note_off(synth, 69);
    /* Stop all callers before app_synth_destroy(synth). */
}
```

Run `python tools/test_synth.py` from the application root for native pitch,
ADSR, waveform/extreme, source headroom, render block equivalence, queue safety,
voice stealing and concurrent-producer checks. The runner finds native GCC/Clang
or MSVC and writes an independently reread 8-second preset WAV under ignored
`build-synth/`. These host checks do not establish codec sound, device latency,
CPU headroom or hardware acceptance.
