# OP-Ay1

A handheld tracker / music workstation prototype for the **ESP-Mosaico** board
(ESP32-S31, 480×480 CO5300 display, ES8311 audio, CST9220 touch).

It plays the built-in `aryx.s3m` module and a native four-voice synthesizer
through a shared 48 kHz stereo bus. Six GSP pages provide waveform scopes,
pattern view, playback, a mixer, a 4×4 section-pad launcher and a touch synth.

![OP-Ay1 pattern view](docs/images/pattern.png)

## Features

- **Portable S3M engine** – decoder, sequencer and mixer in plain C; no ESP-IDF
  dependency except a priority-inheriting task lock. Handles speed/tempo effects,
  portamento, volume slides, sample loops, panning and per-channel gain.
- **Live monitoring** – per-channel peak meters, 2048-sample waveform scopes and
  a final tracker+synth master meter with clip detection.
- **Native synth** – four voices; anti-aliased saw/square, triangle, sine and
  generated wavetable morphing; per-voice low-pass, ADSR and LFO. INIT, BASS,
  LEAD and PAD presets support runtime editing and one-pointer glissando.
- **Shared native UI** – Playback, Mixer and Pads use the first two pages'
  bitmap font and grid. The native GSP simulator runs the same C controller,
  renderers, synth and source bus as the device.
- **Mixer page** – dBFS-calibrated meters (green ≤ −12 dB, yellow ≤ −3 dB), a
  scrollable channel list, dB faders with readout, and mute/solo with the usual
  cascade behaviour.
- **Section pads (clip launcher)** – 4×4 pads: a tap loops that pattern, a second
  tap queues the next pad (blinking in tempo) and it takes over on the next
  pattern boundary; tapping the playing pad queues a stop that closes after the
  current pattern. During song playback the current and upcoming patterns are
  shown.
- **Robustness** – priority-inheriting engine mutex, UI stall watchdog (panic +
  self-recovering reboot), task watchdog, larger ISR stack and a retained
  pre-crash log ring readable through ESP-Iris.

| PATTERN | MIX |
|---|---|
| ![Pattern view](docs/images/pattern.png) | ![Mixer](docs/images/mix.png) |

| SECTION PADS | PLAYBACK |
|---|---|
| ![Section pads](docs/images/pads.png) | ![Playback](docs/images/playback.png) |

| WAVES | |
|---|---|
| ![Waveforms](docs/images/waves.png) | |

| SYNTH | |
|---|---|
| ![Synth](docs/images/synth.png) | |

Playback, Mixer, Pads and Synth have also been exercised on the real board;
the first two pages retain their original appearance. The validation record
distinguishes native screenshots, device evidence and outstanding listening checks.

## Controls

- **Section pads** – tap to play a pattern in a loop; tap another to queue it;
  tap the playing pad to queue a stop (tap again to cancel).
- **Mix** – drag vertically in the list to scroll (start in the right-hand gutter
  to avoid the faders). Fader law: 100 = 0 dB, 120 = +6 dB, 50 = −15 dB, 0 = mute.
- **Playback** – play/pause are immediate; restart rewinds the song and leaves pad
  mode; speed presets are 0.5× / 1× / 1.5× / 2×.
- **Hardware AI button** – short press play/pause, long press rewind.
- **Synth** – hold a key to play, slide to another key, release to enter the
  envelope tail. Choose a preset, OSC/FILTER/ENV/LFO tab, waveform or octave;
  PANIC immediately clears voices at the next audio render. Parameter edits
  stay in RAM. The song can remain paused while the synth plays.
- **Navigation** – swipe outside captured controls or tap a bottom page dot.

## Roadmap

### Software

- [x] **Synthesizer** – native subtractive / built-in wavetable voices alongside
      S3M, with filter, envelope and LFO per voice; shared native validation.
- [ ] **Sampler** – record/import samples, slice and map them to the section pads,
      with per-pad gain and pitch.
- [ ] **Sequencer** – step/clip sequencing, pattern chaining and a proper song mode
      on top of the current order-list player.
- [ ] Expand the module/track limits and add MIDI-over-USB input.

### Hardware

- [ ] **External DAC** – I2S DAC output for higher-quality audio than the on-board
      codec path.
- [ ] **Faders** – physical channel faders wired to the mixer page.
- [ ] **Keyboard** – a key/pad matrix for note entry and performance.

## Build and install

Requirements: the workspace's exact ESP-IDF revision
`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`, target `esp32s31`, GSP 1.5.1,
`gspc` 0.6.1 and a device running retained Vibe Mode firmware.

```sh
# build + install from source
python mosaico.py iris system-update --project projects/OP-Ay1

# or install a released ESP-Iris bundle
python mosaico.py iris system-update --bundle OP-Ay1-1.0.0.irisfw
```

Prebuilt bundles are attached to [GitHub Releases](https://github.com/AyalaKaguya/OP-Ay1/releases).

`app-update` can be used for code-only iterations while the partition table is
unchanged. The release artifact is the **ESP-Iris system-update bundle**
(`.irisfw`), not a full-flash image: the retained Recovery image owns the
bootloader and partition layout.

## Structure

- `main` – composition root: Recovery support, engine, audio, display and GSP.
- `components/app_tracker_engine` – portable S3M decoder / sequencer / mixer;
  the public `tracker_engine.h` C API remains compatible.
- `components/app_synth` – portable four-voice DSP, command queue and presets.
- `components/app_audio` – portable float source bus, DSP task, chunk ring and
  I2S writer; final gain, saturation, clip and scope ownership.
- `components/app_tracker` – shared input model and six-page Canvas renderer;
  `ui/main.json` is authored input, `host/` runs the native backend. Bundles
  and generated headers are rebuilt inside the build directory.
- `components/app_display` – BSP-to-GSP display setup.
- `main/assets/aryx.s3m` – built-in song (12 patterns, 12 channels).

## Known limitations

- The bundled module has 12 patterns and 12 channels, so pads 0C–0F are dim and
  mixer rows 0C–0F are inert.
- Synth input supports one pointer; DSP polyphony does not imply multitouch
  chord input. USB MIDI and preset persistence remain future work.
- Native tests do not establish device touch latency, focus cancellation,
  maximum-load DSP timing, speaker sound or DMA stability.
- The old order-list editor has been replaced by the section-pad launcher.

## Native checks

```sh
python tools/test_synth.py
python tools/test_audio.py
python tools/test_ui_renderer.py
python tools/test_ui_interaction.py
```

From the workspace root, use `python mosaico.py project sim --project
projects/OP-Ay1 --interactive` with a native C compiler. See the
[UI component contract](components/app_tracker/README.md) and
[implementation verification](docs/implementation-validation.zh-CN.md).

## License

Application code is Apache-2.0, see [LICENSE](LICENSE). Third-party dependencies
(ESP-IDF, ESP-GSP, BSP, ESP-Iris, …) keep their own licenses and are not vendored
here.

**Bundled music:** `main/assets/aryx.s3m` is © 1995 Karsten Koch (all rights
reserved). ModArchive only licenses it for unmodified, unbundled redistribution;
embedding it in a firmware image distributed to others requires the author's
written permission. See [docs/store/licensing-aryx.md](docs/store/licensing-aryx.md)
for the evidence and the pending permission request.
