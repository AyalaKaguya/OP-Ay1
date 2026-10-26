// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskCreate(void (*entry)(void *), const char *name, unsigned bytes, void *context,
                  unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
