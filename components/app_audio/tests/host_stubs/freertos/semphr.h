// SPDX-License-Identifier: Apache-2.0
#pragma once
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateCounting(unsigned maximum, unsigned initial);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
