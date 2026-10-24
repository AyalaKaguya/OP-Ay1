# ARYX 1.0.0

ESP-Mosaico tracker player for `main/assets/aryx.s3m` (12 patterns, 12 channels).
Target `esp32s31`, 480x480 CO5300 display, ES8311 audio, CST9220 touch.

## Highlights

- Portable C S3M engine (decoder / sequencer / mixer) with a priority-inheriting
  task lock, 48 kHz stereo output.
- Five pages: WAVES, PATTERN, PLAYBACK (with 0.5x-2x speed), MIX and SECTION PADS.
- MIX: headroom-calibrated dBFS meters, dB faders with readout, M/S solo cascade,
  in-place scrolling channel list.
- SECTION PADS: 4x4 clip launcher; a queued pad blinks in tempo and takes over on
  the pattern boundary, a queued stop closes after the current pattern; during
  song playback the current and upcoming patterns are shown.
- Robustness: priority-inheriting engine mutex, UI stall watchdog (panic + reboot
  after 7 s without a UI tick), task watchdog, larger ISR stack, retained
  pre-crash log ring.

## Firmware identity

| Item | Value |
|---|---|
| ELF SHA-256 | `f211274950cc319ba089e8596c9485699945c4d5b499f32bcf8a0c056ee50204` |
| application BIN SHA-256 | `37387da11a6926dcd81b2df722c842c10caf6d5e30600b8b66ba5ad4de324b2e` |
| first boot ID after install | `1239560398540252981` |
| install result | `system update: succeeded`, validation healthy |

## Artifacts

Local release artifacts (`.codex-runs` is not versioned):

- `.codex-runs/release-v1.0.0/aryx.bin` - application image
- `.codex-runs/release-v1.0.0/aryx.elf` - ELF with symbols
- `.codex-runs/release-v1.0.0/aryx.map` - linker map
- `.codex-runs/release-v1.0.0/aryx-system-update.irisfw` - system-update bundle
- `.codex-runs/release-v1.0.0/SHA256SUMS`

## Build toolchain

- ESP-IDF checkout `383dda6c182e` (local `master`; the workspace pin
  `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` differs on this machine).
- Workspace launcher: `python mosaico.py` (project-local `.venv`).
- Scene: `components/app_tracker/ui/tracker.json` -> `tracker.gspb` (gspc 0.6.1).

## Install

```sh
python mosaico.py iris system-update --project projects/aryx
```

## Verification performed

- Host tests of the portable engine (scratch harnesses in `.codex-runs`):
  full-song render, pattern pad launch/queue/stop, gain law and meter scaling.
- Device: firmware identity matches the ELF, `crash_count 0`, stable heap
  (`free_internal` ~157 KiB, SPIRAM ~12 MiB free), boot screen renders, healthy
  install.
- UI interactions (pads, mixer scrolling, solo cascade, meters, faders) were
  validated on the same UI code in earlier builds on this device; the release
  build only removes development-only RPC/debug hooks and log heartbeats.

## Known limitations

- The bundled module has 12 patterns and 12 channels, so SECTION PADS 0C-0F are
  dim and MIX rows 0C-0F are inert.
- The old order-list/sequencer editor was intentionally replaced by the pad page.
- A horizontal swipe inside the MIX list pages the scene; use the right-hand
  gutter for vertical list scrolling.
