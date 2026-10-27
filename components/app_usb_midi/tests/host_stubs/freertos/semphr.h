// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
typedef struct fake_semaphore *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
