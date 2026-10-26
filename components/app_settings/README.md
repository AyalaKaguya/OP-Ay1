# app_settings

Owns OP-Ay1 display settings, gravity orientation policy, battery snapshots and
its `opay_settings` NVS namespace. It does not render UI, own audio, change the
partition layout or run device update operations. `app_settings.h` is portable;
`app_settings_device.h` is the ESP-IDF/GSP device boundary. `main` owns the opaque
instance and borrows the application-owned presenter from `app_display`.

## Use and lifetime

After NVS and GSP start, call from the composition task:

```c
app_settings_t *settings = NULL;
esp_err_t result = app_settings_start(ui, display.presenter, &settings);
```

Start validates pointers, claims the singleton BSP policy owner, creates a
snapshot mutex/completion semaphore and starts one priority-2 worker with a
6144-byte stack. Allocation/task creation failures release application resources
and leave `*out_settings == NULL`. BSP hardware belongs to BSP singleton services;
no component-private headers or foreign mutable state are accessed.

Only the worker calls IMU/fuel-gauge APIs, owns NVS, applies panel commands and
pauses/resumes the display. UI callbacks send nonblocking atomic requests and use
`app_settings_read_snapshot()`. Snapshot reads use a zero-wait mutex and leave
the caller's previous snapshot untouched on `ESP_ERR_TIMEOUT`. A snapshot's
settings are applied values; `busy` means a hardware change is pending/in flight.
Sensor/storage failure does not stop brightness or manual rotation, and remains
visible in `last_error` and validity flags.

Before a pointer gesture or held-note interaction, the UI sets
`app_settings_set_interaction_busy(settings, true)`; clear it after release or
input cancellation. The worker rechecks that flag after the display pause fence
and defers rotation while it is true. Do not call GSP pause, stop or settings
stop from a GSP event/timer/draw callback: the worker runs those waits outside
the UI task. The UI must keep its callback short and never wait for `busy` to
clear on the render task.

Stop all request/read callers, then call `app_settings_stop(&settings, timeout_ms)`
from a non-UI task **before** stopping GSP or deleting its presenter. Shutdown
resumes any retained pause token, releases only sensors initialized by this
service, commits pending preferences and closes NVS. Hardware cleanup or display
resume failure keeps the instance alive for retry; a stop timeout never frees a
borrowed/in-flight object. The worker's completed task stays inert until its
exclusive owner deletes it. NULL `*settings` is an idempotent successful stop.

## Display policy

- Brightness is 0..100 percent, applied with
  `bsp_display_brightness_set()`. The CO5300 driver writes register `0x51`; no
  separate backlight/PWM driver is introduced.
- Latest brightness requests replace older pending values. Hardware updates
  run at most once per 40 ms, with a 1000 ms retry backoff after failure.
- Manual rotation accepts clockwise 0/90/180/270. GSP is paused/quiesced first,
  then `esp_display_presenter_set_rotation()` updates transforms and storage,
  and `esp_gsp_esp_lcd_resume_paused()` forces a full redraw. Panel and touch
  remain at their BSP 0-degree orientation; presenter input mapping performs
  the inverse transform. The square 480x480 scene needs no layout transpose.
- Rotation lock freezes automatic changes at the current applied angle.
  Explicit manual rotation is still allowed while locked.
- Initial preferences are brightness 70%, angle 0 and locked. NVS stores only
  version, brightness, rotation and lock in the application-owned namespace.
  Commits coalesce after 600 ms; errors retry after five seconds. Unknown
  future schema versions are reported and are not overwritten or erased.

## Automatic orientation

The portable classifier receives acceleration in g at 25 Hz. It accepts a
direction after 400 ms of stable dominant-axis gravity, rejects magnitude
outside 0.75..1.25g, near-flat `|z| > 0.85g`, diagonal orientations, nonfinite
samples, changes over 0.30g/sample, sample gaps over 200 ms and backward time.
Lock and active input reset the candidate dwell. Rotation failures restore the
classifier's last applied angle, not the attempted angle.

The board-axis mapping is centralized in `app_settings.c`: negative Y is 0°,
positive X is 90°, positive Y is 180° and negative X is 270°. This mounting
convention **requires four-direction physical validation on the product board**;
host gravity tests establish the policy, not sensor mounting or physical touch.
Changing mounting convention must update the portable cardinal test with it.
No magnetometer, gyro integration or sensor-fusion service is required.

If an existing initialized/running BMI270 is present, its state is borrowed and
is not reconfigured or stopped. If this service initializes it, it also owns
stop/deinit, including partial initialization cleanup. Initialization failure
disables automatic orientation for this instance while preserving manual rotation.

## Battery evidence

The worker calls BSP BQ27220 status at 1 Hz. Percentage is the gauge's SOC,
voltage is mV, and signed current is mA (positive charging, negative discharge).
Charge state uses reported discharge/full flags and signed current; it does not
claim USB cable/charger presence from those values. Impossible voltage/SOC is
rejected, and failed reads do not invent zero-percent measurements.

`battery_valid` is false until a complete accepted reading and after read
failure. The last sample/time remains available, and `battery_stale` becomes
true after five seconds without a valid update. UI should show unavailable or
stale feedback, rather than treating the retained numerical values as fresh.
The BSP init uses its own characterized fuel-gauge profile; this component does
not alter calibration/profile parameters. Existing gauge state is borrowed;
only a gauge observed uninitialized and started here is deinitialized on stop.

## Validation

Run `python tools/test_settings_orientation.py` for native `/W4 /WX` or
`-Wall -Wextra -Werror` checks of four directions, dwell, motion/flat rejection,
lock, interaction suppression and stale/backward clocks. Those tests never
connect a board or require ESP-IDF. Display pauses, NVS and battery integration
require the pinned firmware build and the repository's device acceptance flow.
