# OP-Ay1

[简体中文](README.zh-CN.md) · [Documentation](docs/README.md)

A handheld music workstation for **ESP-Mosaico**: S3M playback, a four-voice
synthesizer, project storage and a microphone/WAV sampler on a 480×480 touch display.

![OP-Ay1 HOME, sampler, mixer and settings](docs/images/workstation/overview.png)

*Real device captures · 2026-10-27. HOME, sampler, mixer and settings share one active project.*

## Make and keep music

- **Projects on NAND** — create a blank project or import S3M, save the complete
  session and open it later. Save As creates a separate copy. Projects include
  the source module, sample PCM, synth patch, mixer and instrument mappings.
- **Sampler** — import WAV or record the onboard microphone. Browse large,
  vertically scrolling sample cards with real waveforms at the right. Preview,
  trim, loop, adjust gain/C2 rate, Apply, Cancel or Undo; explicitly assign a
  sample to a tracker instrument slot. Recording monitoring defaults OFF.
- **Four-voice synth** — subtractive and built-in wavetable synthesis with a
  low-pass filter, ADSR and LFO. Edit INIT, BASS, LEAD and PAD presets, then hold
  or slide across the keyboard, independently of song playback.
- **S3M playback** — a portable C decoder, sequencer and mixer at 48 kHz stereo,
  with tempo/speed effects, loops, panning and per-channel gain.
- **Mixer and Pads** — scrolling dB faders, meters, mute/solo and a separate
  codec master volume. The 4×4 Section Pads launch Patterns, with queued
  switches and stops at Pattern boundaries.
- **One shared UI** — firmware and the native GSP simulator execute the same
  C controls and renderers. Device settings include brightness, rotation and
  independent idle-dim/display-off timers.

Hardware: **ESP32-S31**, **CO5300 480×480 display**, **ES8311 audio** and
**CST9220 touch** on the ESP-Mosaico board.

## Get around

HOME has three 2×2 launcher pages: Projects, Settings, Playback and Mixer;
Pads, Synth, Sampler and Pattern; then Waves. Empty tiles are inactive.

- Swipe horizontally on **HOME** to change launcher pages. The page indicator
  appears briefly and has no tap action.
- Swipe up from the **bottom edge** to return HOME; the module moves upward.
  Horizontal drags inside modules operate controls without switching pages.
- The project name, unsaved marker and Save entry appear **only on HOME**.
  Navigation keeps the active session and does not restart the song.
- Swipe down from the top edge for Quick Settings. The AI button toggles
  playback on a short press and pauses/rewinds on a long press.

The sample library uses rows 1.6× taller than the previous layout and 2× bitmap
text. Drag to scroll; tap to select. Its per-row waveforms replace the lower
preview panel and paging buttons. EDIT retains its full waveform editor.

## Run in an ESP-Mosaico workspace

Place this application at `projects/OP-Ay1` in the workspace, with its pinned
BSP and integrated utility/ESP-Iris submodules initialized.

- Target: `esp32s31`.
- ESP-IDF: exactly `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`.
- ESP-GSP `1.5.1`, GSPC `0.6.1`; a native C compiler is required for preview.

From the workspace root, preview the shared C application:

```sh
uv run mosaico.py project sim --project projects/OP-Ay1 --interactive
```

Build and install through retained **Vibe Mode**, using the workspace's pinned
build environment and device workflow:

```sh
uv run mosaico.py iris system-update --project projects/OP-Ay1
```

Vendor USB provides the ESP-Iris control/data links. Keep Vendor selected for
firmware delivery. MIDI Host is experimental; its device acceptance failed and
further MIDI work is paused. See the [settings record](docs/global-settings-plan.zh-CN.md).

## Verification and limits

As of **2026-10-27**, the latest UI passed five native suites, 27 simulator
checks, the pinned SDK build and a healthy update on the same device. NAND
mounting, project save/reopen and a microphone Take surviving save and later
firmware updates were observed on hardware. See the [validation record](docs/swipe-navigation.zh-CN.md)
and [component contracts](components/app_tracker/README.md).

- Touch supports one pointer; four DSP voices do not imply multitouch chords.
- Physical power-loss durability, subjective recording quality, long-duration
  maximum load and browser focus-loss delivery remain unverified.
- Section Pads launch Patterns; a step sequencer remains future work.
- Startup is paused. Master volume belongs to the project and starts at 100%
  before loaded controls are applied; the 5% used during debugging is not a
  permanent device preference.

The existing [v1.0.0 release](https://github.com/AyalaKaguya/OP-Ay1/releases/tag/v1.0.0)
is the earlier five-page player. It does not contain this workstation update.

## License

Application code is **Apache-2.0**; see [LICENSE](LICENSE). Dependencies retain
their own licenses. **Aryx** (`main/assets/aryx.s3m`) is © 1995 Karsten Koch,
all rights reserved. Permission to embed it in distributed firmware has not
been established; see the [music licensing record](docs/store/licensing-aryx.md).
