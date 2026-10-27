// SPDX-License-Identifier: Apache-2.0
#include "project_platform.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    project_platform_t *platform;
    void (*worker)(void *);
    void *argument;
} worker_start_t;

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#define PROJECT_WORKER_STACK_BYTES (20U * 1024U)
static TickType_t ticks(uint32_t ms) { return pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : ms ? 1 : 0; }

bool project_platform_init(project_platform_t *p)
{
    memset(p, 0, sizeof(*p));
    p->mutex = xSemaphoreCreateMutex();
    p->wake = xSemaphoreCreateBinary();
    p->done = xSemaphoreCreateBinary();
    if (!p->mutex || !p->wake || !p->done) { project_platform_destroy(p); return false; }
    return true;
}
void project_platform_destroy(project_platform_t *p)
{
    if (p->mutex) vSemaphoreDelete(p->mutex);
    if (p->wake) vSemaphoreDelete(p->wake);
    if (p->done) vSemaphoreDelete(p->done);
    memset(p, 0, sizeof(*p));
}
static void worker_entry(void *argument)
{
    worker_start_t *start = argument;
    project_platform_t *p = start->platform;
    void (*worker)(void *) = start->worker;
    void *context = start->argument;
    free(start);
    worker(context);
    (void)xSemaphoreGive(p->done);
    /* The join owner deletes this inert task. Its handle remains valid across
     * a timeout, and no worker/context access occurs after the done signal. */
    vTaskSuspend(NULL);
}
bool project_platform_start(project_platform_t *p, void (*worker)(void *), void *argument)
{
    worker_start_t *start = malloc(sizeof(*start));
    if (!start) return false;
    *start = (worker_start_t) {p, worker, argument};
    if (xTaskCreate(worker_entry, "project_io", PROJECT_WORKER_STACK_BYTES, start, 2, &p->task) != pdPASS) {
        free(start); return false;
    }
    return true;
}
void project_platform_lock(project_platform_t *p) { (void)xSemaphoreTake(p->mutex, portMAX_DELAY); }
bool project_platform_trylock(project_platform_t *p) { return xSemaphoreTake(p->mutex, 0) == pdTRUE; }
void project_platform_unlock(project_platform_t *p) { (void)xSemaphoreGive(p->mutex); }
void project_platform_wake(project_platform_t *p) { (void)xSemaphoreGive(p->wake); }
void project_platform_wait(project_platform_t *p, uint32_t ms) { (void)xSemaphoreTake(p->wake, ticks(ms)); }
bool project_platform_join(project_platform_t *p, uint32_t ms)
{
    if (xSemaphoreTake(p->done, ticks(ms)) != pdTRUE) return false;
    vTaskDelete(p->task); p->task = NULL; return true;
}
uint64_t project_platform_time_ms(void) { return (uint64_t)esp_timer_get_time() / 1000U; }

#elif defined(_WIN32)

bool project_platform_init(project_platform_t *p)
{
    memset(p, 0, sizeof(*p));
    if (!InitializeCriticalSectionEx(&p->mutex, 0, 0)) return false;
    p->mutex_ready = true;
    p->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!p->wake) { project_platform_destroy(p); return false; }
    return true;
}
void project_platform_destroy(project_platform_t *p)
{
    if (p->task) CloseHandle(p->task);
    if (p->wake) CloseHandle(p->wake);
    if (p->mutex_ready) DeleteCriticalSection(&p->mutex);
    memset(p, 0, sizeof(*p));
}
static DWORD WINAPI worker_entry(void *argument)
{
    worker_start_t *start = argument;
    void (*worker)(void *) = start->worker;
    void *context = start->argument;
    free(start);
    worker(context);
    return 0;
}
bool project_platform_start(project_platform_t *p, void (*worker)(void *), void *argument)
{
    worker_start_t *start = malloc(sizeof(*start));
    if (!start) return false;
    *start = (worker_start_t) {p, worker, argument};
    p->task = CreateThread(NULL, 0, worker_entry, start, 0, NULL);
    if (!p->task) { free(start); return false; }
    return true;
}
void project_platform_lock(project_platform_t *p) { EnterCriticalSection(&p->mutex); }
bool project_platform_trylock(project_platform_t *p) { return TryEnterCriticalSection(&p->mutex) != 0; }
void project_platform_unlock(project_platform_t *p) { LeaveCriticalSection(&p->mutex); }
void project_platform_wake(project_platform_t *p) { (void)SetEvent(p->wake); }
void project_platform_wait(project_platform_t *p, uint32_t ms) { (void)WaitForSingleObject(p->wake, ms); }
bool project_platform_join(project_platform_t *p, uint32_t ms)
{
    if (WaitForSingleObject(p->task, ms) != WAIT_OBJECT_0) return false;
    CloseHandle(p->task); p->task = NULL; return true;
}
uint64_t project_platform_time_ms(void) { return GetTickCount64(); }

#else
#include <errno.h>
#include <time.h>

static struct timespec deadline(uint32_t ms)
{
    struct timespec end;
    (void)clock_gettime(CLOCK_REALTIME, &end);
    end.tv_sec += ms / 1000U;
    end.tv_nsec += (long)(ms % 1000U) * 1000000L;
    if (end.tv_nsec >= 1000000000L) { ++end.tv_sec; end.tv_nsec -= 1000000000L; }
    return end;
}
bool project_platform_init(project_platform_t *p)
{
    memset(p, 0, sizeof(*p));
    if (pthread_mutex_init(&p->mutex, NULL)) return false;
    if (pthread_mutex_init(&p->wake_mutex, NULL)) { pthread_mutex_destroy(&p->mutex); return false; }
    if (pthread_cond_init(&p->wake, NULL)) {
        pthread_mutex_destroy(&p->wake_mutex); pthread_mutex_destroy(&p->mutex); return false;
    }
    p->initialized = true;
    return true;
}
void project_platform_destroy(project_platform_t *p)
{
    if (p->initialized) {
        pthread_cond_destroy(&p->wake); pthread_mutex_destroy(&p->wake_mutex); pthread_mutex_destroy(&p->mutex);
    }
    memset(p, 0, sizeof(*p));
}
static void *worker_entry(void *argument)
{
    worker_start_t *start = argument;
    project_platform_t *p = start->platform;
    void (*worker)(void *) = start->worker;
    void *context = start->argument;
    free(start);
    worker(context);
    pthread_mutex_lock(&p->wake_mutex); p->finished = true;
    pthread_cond_broadcast(&p->wake); pthread_mutex_unlock(&p->wake_mutex);
    return NULL;
}
bool project_platform_start(project_platform_t *p, void (*worker)(void *), void *argument)
{
    worker_start_t *start = malloc(sizeof(*start));
    if (!start) return false;
    *start = (worker_start_t) {p, worker, argument};
    if (pthread_create(&p->task, NULL, worker_entry, start)) { free(start); return false; }
    p->task_started = true;
    return true;
}
void project_platform_lock(project_platform_t *p) { pthread_mutex_lock(&p->mutex); }
bool project_platform_trylock(project_platform_t *p) { return pthread_mutex_trylock(&p->mutex) == 0; }
void project_platform_unlock(project_platform_t *p) { pthread_mutex_unlock(&p->mutex); }
void project_platform_wake(project_platform_t *p)
{
    pthread_mutex_lock(&p->wake_mutex); p->signaled = true;
    pthread_cond_broadcast(&p->wake); pthread_mutex_unlock(&p->wake_mutex);
}
void project_platform_wait(project_platform_t *p, uint32_t ms)
{
    const struct timespec end = deadline(ms);
    pthread_mutex_lock(&p->wake_mutex);
    while (!p->signaled) { if (pthread_cond_timedwait(&p->wake, &p->wake_mutex, &end)) break; }
    p->signaled = false;
    pthread_mutex_unlock(&p->wake_mutex);
}
bool project_platform_join(project_platform_t *p, uint32_t ms)
{
    const struct timespec end = deadline(ms);
    pthread_mutex_lock(&p->wake_mutex);
    while (!p->finished) {
        if (pthread_cond_timedwait(&p->wake, &p->wake_mutex, &end) == ETIMEDOUT) break;
    }
    const bool done = p->finished;
    pthread_mutex_unlock(&p->wake_mutex);
    if (!done || pthread_join(p->task, NULL)) return false;
    p->task_started = false;
    return true;
}
uint64_t project_platform_time_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
#endif
