// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t esp_iris_usb_set_enabled(bool enabled, uint32_t timeout_ms);
