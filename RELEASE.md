# OP-Ay1 1.0.0

ESP-Mosaico tracker player with the built-in `main/assets/aryx.s3m` module
(12 patterns, 12 channels). Target `esp32s31`, 480x480 CO5300 display, ES8311
audio, CST9220 touch.

## Release artifact

The release is the **ESP-Iris system-update binary**, not a flash image with
bootloader/partition table:

- `OP-Ay1-1.0.0.irisfw` - ESP-Iris system-update bundle (application, UI bundle
  and declared partition-table changes); this is the file to distribute.
- `OP-Ay1-1.0.0.elf`, `OP-Ay1-1.0.0.map` - symbols and linker map for crash
  decoding/support only (not installable images).

No merged full-flash `.bin` is produced or shipped; Recovery owns the bootloader
and partition layout.

## Firmware identity

| Item | Value |
|---|---|
| project name on device | `OP-Ay1` |
| ELF SHA-256 | `367b525a2818824aae306df8877c88268b30f182fa3bee7cf64709e3b52b8bf2` |
| install result | `system update: succeeded`, validation healthy |
| first boot ID after install | `14823207951374323298` |

## Checksums

See the `SHA256SUMS` file next to the artifacts:

- `OP-Ay1-1.0.0.irisfw` `b97a8604183fcfc43e806e755ed9f0d31d7ae138891591780399d5fabf732de5`
- `OP-Ay1-1.0.0.elf` `367b525a2818824aae306df8877c88268b30f182fa3bee7cf64709e3b52b8bf2`
- `OP-Ay1-1.0.0.map` `4266aa4bc6e82d2470a822e0e14f45148c6c5a4dfbba0c5f5e664c72e8d047fa`

## Install

From the workspace root, either build from source or install the released
bundle:

```sh
python mosaico.py iris system-update --project projects/OP-Ay1
python mosaico.py iris system-update --project projects/OP-Ay1 --bundle OP-Ay1-1.0.0.irisfw
```

## Highlights

- Portable C S3M engine (decoder / sequencer / mixer) with a priority-inheriting
  task lock and 48 kHz stereo output.
- Five pages: WAVES, PATTERN, PLAYBACK (0.5x-2x speed), MIX, SECTION PADS.
- MIX: headroom-calibrated dBFS meters, dB faders with readout, M/S solo cascade,
  in-place scrolling channel list.
- SECTION PADS: 4x4 clip launcher; a queued pad blinks in tempo and takes over on
  the pattern boundary, a queued stop closes after the current pattern; during
  song playback the current and upcoming patterns are shown.
- Robustness: priority-inheriting engine mutex, UI stall watchdog (panic + reboot
  after 7 s without a UI tick), task watchdog, larger ISR stack and retained
  pre-crash log ring.

## Build toolchain

- ESP-IDF checkout `383dda6c182e` (local `master`; the workspace pin
  `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` differs on this machine).
- Workspace launcher `python mosaico.py`; scene `tracker.json` packed with gspc
  0.6.1; GSPC_EXECUTABLE is used when the fetched cache is available.

## Verification performed

- Host tests of the portable engine (scratch harnesses): full-song render,
  pattern pad launch/queue/stop, gain law and meter scaling - all pass.
- Device: project name `OP-Ay1`, firmware hash matches the ELF, `crash_count 0`,
  healthy install, stable heap (`free_internal` ~157 KiB, SPIRAM ~12 MiB free).
- Touch interactions were validated on the same UI code in earlier builds on this
  device; the release build removes development-only input-injection RPCs.

## Known limitations

- The built-in module has 12 patterns and 12 channels, so SECTION PADS 0C-0F are
  dim and MIX rows 0C-0F are inert.
- The old order-list editor was replaced by the pad page.
- A horizontal swipe inside the MIX list pages the scene; use the right-hand
  gutter for vertical list scrolling.
