// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_settings.h"
#include "esp_display_present.h"
#include "esp_err.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_settings app_settings_t;

/* Start once, after GSP and NVS initialization, from a non-UI task. Borrows
 * ui and presenter until stop. Sensors/storage may be unavailable without
 * failing startup; their error and validity remain visible in snapshots. */
esp_err_t app_settings_start(esp_gsp_handle_t ui,
                             esp_display_presenter_t *presenter,
                             app_settings_t **out_settings);

/* Nonblocking, coalesced UI-thread requests. Brightness accepts 0..100;
 * rotation accepts 0/90/180/270 and also works while rotation is locked. */
esp_err_t app_settings_request_brightness(app_settings_t *settings, int percent);
esp_err_t app_settings_request_rotation(app_settings_t *settings, int degrees);
esp_err_t app_settings_request_lock(app_settings_t *settings, bool locked);
/* UI reports any active pointer/held note. Rotation waits until false;
 * brightness and battery updates continue while an interaction is active. */
void app_settings_set_interaction_busy(app_settings_t *settings, bool busy);
/* Zero-wait snapshot read. On ESP_ERR_TIMEOUT, out_snapshot is unchanged. */
esp_err_t app_settings_read_snapshot(app_settings_t *settings,
                                     app_settings_snapshot_t *out_snapshot);
/* Stop from a non-UI task after callers stop using the request/read API.
 * On timeout/failure retain *settings and retry; it is freed only on success.
 * Stop settings before stopping GSP or destroying its borrowed presenter. */
esp_err_t app_settings_stop(app_settings_t **settings, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
