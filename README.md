# OP-Ay1

A handheld tracker / music workstation prototype for the **ESP-Mosaico** board
(ESP32-S31, 480×480 CO5300 display, ES8311 audio, CST9220 touch).

It plays the built-in `aryx.s3m` module through a portable C S3M engine (48 kHz
stereo), and drives a five-page GSP interface: waveform scope, pattern view,
transport, a mixer, and a 4×4 section-pad launcher.

![OP-Ay1 pattern view](docs/images/pattern.png)

## Features

- **Portable S3M engine** – decoder, sequencer and mixer in plain C; no ESP-IDF
  dependency except a priority-inheriting task lock. Handles speed/tempo effects,
  portamento, volume slides, sample loops, panning and per-channel gain.
- **Live monitoring** – per-channel peak meters, 2048-sample waveform scopes and
  a master meter with clip detection.
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

## Controls

- **Section pads** – tap to play a pattern in a loop; tap another to queue it;
  tap the playing pad to queue a stop (tap again to cancel).
- **Mix** – drag vertically in the list to scroll (start in the right-hand gutter
  to avoid the faders). Fader law: 100 = 0 dB, 120 = +6 dB, 50 = −15 dB, 0 = mute.
- **Playback** – play/pause are immediate; restart rewinds the song and leaves pad
  mode; speed presets are 0.5× / 1× / 1.5× / 2×.
- **Hardware AI button** – short press play/pause, long press rewind.

## Roadmap

### Software

- [ ] **Synthesizer** – add a native voice engine (subtractive / wavetable) next to
      S3M playback, with filter, envelope and LFO per voice.
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

Requirements: the workspace's ESP-Mosaico toolchain (ESP-IDF 6.2 dev, GSP 1.5.1,
`gspc` 0.6.1) and a device running the retained Recovery image.

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
- `components/tracker_engine` – portable S3M decoder / sequencer / mixer.
- `components/app_audio` – DSP task, chunk ring and I2S writer.
- `components/app_tracker` – GSP scene controller, canvas renderers, mix page and
  section pads; `ui/` holds the authored scene and packed bundle.
- `components/app_display` – BSP-to-GSP display setup.
- `main/assets/aryx.s3m` – built-in song (12 patterns, 12 channels).

## Known limitations

- The bundled module has 12 patterns and 12 channels, so pads 0C–0F are dim and
  mixer rows 0C–0F are inert.
- A horizontal swipe inside the mixer list pages the scene; vertical drags scroll.
- The old order-list editor has been replaced by the section-pad launcher.

## License

Application code is Apache-2.0, see [LICENSE](LICENSE). Third-party dependencies
(ESP-IDF, ESP-GSP, BSP, ESP-Iris, …) keep their own licenses and are not vendored
here.

**Bundled music:** `main/assets/aryx.s3m` is © 1995 Karsten Koch (all rights
reserved). ModArchive only licenses it for unmodified, unbundled redistribution;
embedding it in a firmware image distributed to others requires the author's
written permission. See [docs/store/licensing-aryx.md](docs/store/licensing-aryx.md)
for the evidence and the pending permission request.
