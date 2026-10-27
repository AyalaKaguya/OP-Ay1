// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
typedef struct fake_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned item_size);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
int xQueueReset(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
