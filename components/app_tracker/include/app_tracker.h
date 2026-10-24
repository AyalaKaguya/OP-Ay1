// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "esp_gsp.h"
#include "tracker_engine.h"

typedef struct app_tracker app_tracker_t;

/* Binds the tracker controller to the generated scene and starts UI timers. */
esp_err_t app_tracker_start(esp_gsp_handle_t ui, tracker_engine_t *engine,
                            app_tracker_t **out_handle);

void app_tracker_stop(app_tracker_t **handle);

/* Diagnostic/test hook: jump directly to a page (0..4). */
void app_tracker_set_page(app_tracker_t *handle, int page);
