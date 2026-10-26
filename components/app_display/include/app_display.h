// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "esp_gsp_esp_lcd.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize BSP hardware and an application-owned presenter, then fill out_config.
 * Call once at boot before GSP; hardware ownership and timing: ../README.md.
 * Returns ESP_OK or an initialization error; output unchanged on failure. */
esp_err_t app_display_init(esp_gsp_esp_lcd_config_t *out_config);

/** Release the presenter after settings and GSP have stopped using it.
 * On failure ownership is retained for retry. BSP panel/touch remain alive. */
esp_err_t app_display_deinit(void);

#ifdef __cplusplus
}
#endif
