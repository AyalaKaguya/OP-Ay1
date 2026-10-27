// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#define CONFIG_FREERTOS_UNICORE 1
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define tskNO_AFFINITY -1
#define pdMS_TO_TICKS(value) (value)
