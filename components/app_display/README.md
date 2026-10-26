# app_display

Adapts the BSP panel and optional touch to GSP's display configuration. It owns
application display policy (RGB565, byte order, rotation, TE and render alignment),
not UI behavior. The component explicitly creates and owns the presenter;
GSP borrows it through `esp_gsp_esp_lcd_config_t.presenter`. The public
dependencies are ESP-GSP and display-present; BSP, LCD and logging are private.
There is no dependency on `app_tracker`, `app_settings` or `main`.

```c
esp_gsp_esp_lcd_config_t display;
esp_err_t err = app_display_init(&display);
/* On ESP_OK, pass &display to esp_gsp_esp_lcd_start with the bundle config.
 * display.presenter can also be borrowed by the settings worker. */
```

The CO5300 QSPI panel requires both the refresh window's X origin and width to
be multiples of 4 pixels. `app_display_init()` explicitly sets GSP's
`render_alignment.x_pixels = 4` and `render_alignment.width_pixels = 4` on the
device. Keep both values: GSP defaults to no expansion, and the BSP's area rounder
only handles LVGL. Existing generated applications must apply the same change
to their device display configuration; updating the template does not patch them.

After changing display initialization, verify partial refresh on the physical
panel: repeated counter updates, the celebration animation, and scrolling in
applications that provide it. Check for shifted pixels, corruption and stale
regions. Host simulation and Iris screenshots cannot establish panel alignment;
Iris captures the pixel buffer before the panel transfer.

Call once from boot, before GSP starts; do not call from ISR or concurrently.
After settings and GSP stop, `app_display_deinit()` releases the presenter.
Deletion failure retains its handle for a later retry. Repeated initialization
while a presenter exists returns `ESP_ERR_INVALID_STATE`; configuration output
is unchanged. Display rotation and hardware writes must be serialized with GSP
by an external pause/quiesce owner, not performed from its draw callback.
Configuration is copied to caller storage only on success. Hardware handles are
borrowed from the singleton BSP and remain alive until reboot. Optional touch
failure yields `ESP_OK` with `display.touch == NULL`; display failure propagates.
The caller owns configuration storage, not hardware handles; do not free those
handles or reinitialize the BSP while GSP uses them. Resolution, pixel clock (Hz),
rotation and TE wiring come from BSP settings. NULL output returns
`ESP_ERR_INVALID_ARG`; missing panel/io returns `ESP_ERR_INVALID_STATE`; other
display initialization errors propagate from the BSP.
Hardware initialization may block using driver timeouts; there is no additional
application deadline. A failed presenter creation publishes no owned presenter;
the BSP's process-lifetime panel/touch remain initialized for a retry. Keep
Vibe Mode reachable if UI startup cannot proceed.

Private helpers remain static in the source file. If private headers become
necessary, put them under `private/` and declare only `PRIV_INCLUDE_DIRS`.
