# OP-Ay1

An ESP-Mosaico tracker player. The 20 KB built-in `main/assets/aryx.s3m` module is
decoded by a portable C engine and rendered into 48 kHz stereo by the board audio
path; the whole UI is one generated GSP scene driven by `app_tracker`.

## Pages

| Page | Contents |
|---|---|
| WAVES | Per-channel oscilloscope canvas |
| PATTERN | Pattern grid canvas with the playback cursor |
| PLAYBACK | Transport, speed multiplier (0.5x / 1x / 1.5x / 2x), position |
| MIX | Master fader/meter on top, scrollable 4-row channel list with M/S, dB fader and level meter |
| SECTION PADS | 4x4 pattern launcher |

## Controls

- **Section pads** – tap a pad to play that pattern in a loop. Tapping another
  pad queues it (cyan, blinking in tempo) and it takes over at the end of the
  current pattern. Tapping the playing pad queues a stop (orange blinking) that
  also lands on the pattern boundary; tap again to cancel. During normal song
  playback the current pattern is orange and the upcoming pattern blinks cyan.
- **MIX** – the list scrolls with a vertical drag (start in the right-hand gutter
  to avoid the faders). The fader law is dB based: 100 = 0 dB, 120 = +6 dB,
  50 = -15 dB, 0 = silent; the value is shown next to the meter. M/S follow the
  usual solo cascade; channels the module does not have are dimmed.
- **Playback** – PLAY/PAUSE are immediate; RESTART rewinds the song and leaves
  pad mode.
- **Hardware AI button** – short press play/pause, long press rewind.

## Structure

- `main`: composition root only (Recovery support, engine, audio, display, GSP).
- `components/tracker_engine`: portable S3M decoder, sequencer and mixer.
- `components/app_audio`: DSP task, chunk ring and I2S writer.
- `components/app_tracker`: GSP scene controller, meters, mix page and pads.
- `components/app_display`: BSP-to-GSP display setup.
- `components/app_tracker/ui`: authored scene (`tracker.json`) and packed bundle.
- `main/assets/aryx.s3m`: built-in song (12 patterns, 12 channels).

## Build and install

From the workspace root, with `IDF_PATH` and `IDF_TOOLS_PATH` set:

```sh
python mosaico.py iris system-update --project projects/OP-Ay1
```

`system-update` builds the project, packs the GSP scene into the application and
installs the ESP-Iris system-update bundle through Recovery. Code-only iterations
can use `iris app-update` while the partition table is unchanged.

## Runtime notes

- The UI watchdog panics (and reboots into a healthy image) if the render task
  stops ticking for 7 s; the task watchdog covers blocked tasks. Core dumps and
  the retained pre-crash log ring stay readable through
  `python mosaico.py iris crash` / `iris logs`.
- Touch-panel and WiFi-PHY warnings are limited to errors so the retained log
  ring keeps real evidence.
