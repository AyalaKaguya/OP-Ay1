// SPDX-License-Identifier: Apache-2.0
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_audio_bus.h"

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE test_thread_t;
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t test_thread_t;
#endif

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "audio_edit_gate:%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef struct {
    app_audio_bus_t *bus;
    atomic_bool acquired;
    atomic_bool release;
    atomic_bool done;
} holder_t;

typedef struct {
    app_audio_bus_t *bus;
    uint32_t timeout_ms;
    atomic_bool started;
    atomic_bool done;
    bool acquired;
    uint64_t elapsed_ms;
} waiter_t;

static uint64_t time_ms(void)
{
#if defined(_WIN32)
    return GetTickCount64();
#else
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
#endif
}

static void pause_ms(unsigned milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    const struct timespec delay = {milliseconds / 1000U, (long)(milliseconds % 1000U) * 1000000L};
    (void)nanosleep(&delay, NULL);
#endif
}

static void wait_flag(const atomic_bool *flag)
{
    const uint64_t start = time_ms();
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        CHECK(time_ms() - start < 2000U);
        pause_ms(1);
    }
}

static void holder_body(holder_t *holder)
{
    CHECK(app_audio_bus_begin_edit(holder->bus, 0));
    atomic_store_explicit(&holder->acquired, true, memory_order_release);
    wait_flag(&holder->release);
    app_audio_bus_end_edit(holder->bus);
    atomic_store_explicit(&holder->done, true, memory_order_release);
}

static void waiter_body(waiter_t *waiter)
{
    const uint64_t start = time_ms();
    atomic_store_explicit(&waiter->started, true, memory_order_release);
    waiter->acquired = app_audio_bus_begin_edit(waiter->bus, waiter->timeout_ms);
    waiter->elapsed_ms = time_ms() - start;
    if (waiter->acquired) {
        app_audio_bus_end_edit(waiter->bus);
    }
    atomic_store_explicit(&waiter->done, true, memory_order_release);
}

#if defined(_WIN32)
static DWORD WINAPI holder_entry(LPVOID argument)
{
    holder_body(argument); return 0;
}
static DWORD WINAPI waiter_entry(LPVOID argument)
{
    waiter_body(argument); return 0;
}
#else
static void *holder_entry(void *argument)
{
    holder_body(argument); return NULL;
}
static void *waiter_entry(void *argument)
{
    waiter_body(argument); return NULL;
}
#endif

static test_thread_t start_holder(holder_t *holder, app_audio_bus_t *bus)
{
    holder->bus = bus;
    atomic_init(&holder->acquired, false);
    atomic_init(&holder->release, false);
    atomic_init(&holder->done, false);
#if defined(_WIN32)
    test_thread_t thread = CreateThread(NULL, 0, holder_entry, holder, 0, NULL);
    CHECK(thread != NULL);
#else
    test_thread_t thread;
    CHECK(pthread_create(&thread, NULL, holder_entry, holder) == 0);
#endif
    wait_flag(&holder->acquired);
    return thread;
}

static test_thread_t start_waiter(waiter_t *waiter, app_audio_bus_t *bus, uint32_t timeout_ms)
{
    waiter->bus = bus;
    waiter->timeout_ms = timeout_ms;
    waiter->acquired = false;
    waiter->elapsed_ms = 0;
    atomic_init(&waiter->started, false);
    atomic_init(&waiter->done, false);
#if defined(_WIN32)
    test_thread_t thread = CreateThread(NULL, 0, waiter_entry, waiter, 0, NULL);
    CHECK(thread != NULL);
#else
    test_thread_t thread;
    CHECK(pthread_create(&thread, NULL, waiter_entry, waiter) == 0);
#endif
    wait_flag(&waiter->started);
    return thread;
}

static void join_thread(test_thread_t thread, const atomic_bool *done)
{
    wait_flag(done);
#if defined(_WIN32)
    CHECK(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CHECK(CloseHandle(thread));
#else
    CHECK(pthread_join(thread, NULL) == 0);
#endif
}

static void released_before_deadline(app_audio_bus_t *bus)
{
    holder_t holder;
    waiter_t waiter;
    const test_thread_t holding = start_holder(&holder, bus);
    const test_thread_t waiting = start_waiter(&waiter, bus, 1000);
    pause_ms(50);
    CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));
    atomic_store_explicit(&holder.release, true, memory_order_release);
    join_thread(holding, &holder.done);
    join_thread(waiting, &waiter.done);
    CHECK(waiter.acquired && waiter.elapsed_ms >= 40U && waiter.elapsed_ms < 1000U);
    puts("audio edit gate: a worker acquires after a concurrent holder releases before its deadline PASS");
}

static void bounded_timeout(app_audio_bus_t *bus)
{
    holder_t holder;
    waiter_t waiter;
    const test_thread_t holding = start_holder(&holder, bus);
    const test_thread_t waiting = start_waiter(&waiter, bus, 60);
    join_thread(waiting, &waiter.done);
    CHECK(!waiter.acquired && waiter.elapsed_ms >= 60U && waiter.elapsed_ms < 1000U);
    CHECK(!atomic_load_explicit(&holder.done, memory_order_acquire));
    atomic_store_explicit(&holder.release, true, memory_order_release);
    join_thread(holding, &holder.done);
    puts("audio edit gate: a held gate expires within the worker timeout without stealing ownership PASS");
}

static void nonblocking_render(app_audio_bus_t *bus)
{
    holder_t holder;
    const test_thread_t holding = start_holder(&holder, bus);
    const uint64_t start = time_ms();
    CHECK(!app_audio_bus_begin_edit(bus, 0));
    CHECK(time_ms() - start < 100U);
    app_audio_bus_status_t before, after;
    app_audio_bus_get_status(bus, &before);
    int16_t output[4] = {1, 1, 1, 1};
    app_audio_bus_render(bus, output, 2);
    app_audio_bus_get_status(bus, &after);
    CHECK(output[0] == 0 && output[1] == 0 && output[2] == 0 && output[3] == 0);
    CHECK(after.frames == before.frames);
    atomic_store_explicit(&holder.release, true, memory_order_release);
    join_thread(holding, &holder.done);
    CHECK(app_audio_bus_begin_edit(bus, 0));
    app_audio_bus_end_edit(bus);
    puts("audio edit gate: timeout zero stays nonblocking and a gated renderer emits silence without advancing PASS");
}

int main(void)
{
    app_synth_t *synth = app_synth_create(APP_AUDIO_SAMPLE_RATE);
    CHECK(synth != NULL);
    app_audio_bus_t *bus = app_audio_bus_create(NULL, synth);
    CHECK(bus != NULL);
    CHECK(!app_audio_bus_begin_edit(NULL, 100));
    released_before_deadline(bus);
    bounded_timeout(bus);
    nonblocking_render(bus);
    app_audio_bus_destroy(bus);
    app_synth_destroy(synth);
    puts("audio edit gate concurrency: PASS");
    return 0;
}
