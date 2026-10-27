// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
typedef struct {
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t wake;
    SemaphoreHandle_t done;
    TaskHandle_t task;
} project_platform_t;
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct {
    CRITICAL_SECTION mutex;
    HANDLE wake;
    HANDLE task;
    bool mutex_ready;
} project_platform_t;
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t mutex;
    pthread_mutex_t wake_mutex;
    pthread_cond_t wake;
    pthread_t task;
    bool initialized;
    bool task_started;
    bool signaled;
    bool finished;
} project_platform_t;
#endif

bool project_platform_init(project_platform_t *platform);
void project_platform_destroy(project_platform_t *platform);
bool project_platform_start(project_platform_t *platform, void (*worker)(void *), void *argument);
void project_platform_lock(project_platform_t *platform);
bool project_platform_trylock(project_platform_t *platform);
void project_platform_unlock(project_platform_t *platform);
void project_platform_wake(project_platform_t *platform);
void project_platform_wait(project_platform_t *platform, uint32_t timeout_ms);
bool project_platform_join(project_platform_t *platform, uint32_t timeout_ms);
uint64_t project_platform_time_ms(void);
