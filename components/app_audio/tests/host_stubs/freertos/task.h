// SPDX-License-Identifier: Apache-2.0
#pragma once
typedef void *TaskHandle_t;
unsigned ulTaskNotifyTake(int clear, unsigned timeout);
int xTaskNotifyGive(TaskHandle_t task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(unsigned ticks);
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                              unsigned stack, void *argument, unsigned priority,
                              TaskHandle_t *handle, int core);
