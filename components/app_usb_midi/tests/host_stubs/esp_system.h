// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
esp_err_t esp_register_shutdown_handler(void (*handler)(void));
esp_err_t esp_unregister_shutdown_handler(void (*handler)(void));
