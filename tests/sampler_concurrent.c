// SPDX-License-Identifier: Apache-2.0
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "app_sampler.h"
#include "app_audio_bus.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

typedef struct {
    tracker_engine_t *engine;
    app_sampler_t *sampler;
    app_audio_bus_t *bus;
    atomic_bool stop;
    atomic_uint failures, reads, renders;
} concurrent_t;

static void one_check(concurrent_t *c, bool success)
{ if (!success) { atomic_fetch_add_explicit(&c->failures, 1U, memory_order_relaxed); } }
static void reader_body(concurrent_t *c)
{
    while (!atomic_load_explicit(&c->stop, memory_order_acquire)) {
        tracker_status_t status; tracker_engine_get_status(c->engine, &status);
        one_check(c, status.channels == 12 && status.patterns == 12 && status.orders == 44);
        one_check(c, tracker_engine_patterns(c->engine) == 12 && tracker_engine_orders(c->engine) == 44);
        const int pattern = tracker_engine_pattern_at(c->engine, 0);
        one_check(c, pattern == 3);
        tracker_cell_t cell; tracker_engine_cell(c->engine, 3, 0, 0, &cell);
        one_check(c, cell.effect == 1 && cell.param == 2);
        app_sampler_sample_info_t info;
        one_check(c, app_sampler_get_info(c->sampler, 0, &info) && info.frames == 174 && info.c2_speed == 44444);
        atomic_fetch_add_explicit(&c->reads, 1U, memory_order_relaxed);
    }
}
static void renderer_body(concurrent_t *c)
{
    int16_t output[256];
    while (!atomic_load_explicit(&c->stop, memory_order_acquire)) {
        app_audio_bus_render(c->bus, output, 128);
        app_audio_bus_status_t status; app_audio_bus_get_status(c->bus, &status);
        one_check(c, isfinite(status.master_peak));
        atomic_fetch_add_explicit(&c->renders, 1U, memory_order_relaxed);
    }
}
#if defined(_WIN32)
static DWORD WINAPI reader_entry(LPVOID arg) { reader_body(arg); return 0; }
static DWORD WINAPI render_entry(LPVOID arg) { renderer_body(arg); return 0; }
#else
static void *reader_entry(void *arg) { reader_body(arg); return NULL; }
static void *render_entry(void *arg) { renderer_body(arg); return NULL; }
#endif

void sampler_concurrency(const uint8_t *module, size_t size)
{
    concurrent_t *c = calloc(1, sizeof(*c));
    if (!c) { abort(); }
    atomic_init(&c->stop, false); atomic_init(&c->failures, 0U);
    atomic_init(&c->reads, 0U); atomic_init(&c->renders, 0U);
    c->engine = tracker_engine_create(module, size); c->sampler = app_sampler_create();
    c->bus = app_audio_bus_create(c->engine, NULL);
    if (!c->engine || !c->sampler || !c->bus || !app_audio_bus_set_sampler(c->bus, c->sampler)) { abort(); }
    app_sampler_result_t result;
    app_sampler_bank_t *initial = app_sampler_bank_from_s3m(module, size, &result);
    if (!initial) { abort(); }
    app_sampler_bank_commit(c->sampler, initial);
#if defined(_WIN32)
    HANDLE reader = CreateThread(NULL, 0, reader_entry, c, 0, NULL);
    HANDLE renderer = CreateThread(NULL, 0, render_entry, c, 0, NULL);
    if (!reader || !renderer) { abort(); }
#else
    pthread_t reader, renderer;
    if (pthread_create(&reader, NULL, reader_entry, c) || pthread_create(&renderer, NULL, render_entry, c)) { abort(); }
#endif
    /* Explicit start barrier: a busy compiler host may otherwise let the
     * writer finish before the OS schedules the new renderer thread. */
    for (unsigned wait = 0; wait < 10000U; ++wait) {
        if (atomic_load_explicit(&c->reads, memory_order_relaxed) &&
            atomic_load_explicit(&c->renders, memory_order_relaxed)) { break; }
#if defined(_WIN32)
        Sleep(1);
#else
        const struct timespec pause = {0, 1000000};
        (void)nanosleep(&pause, NULL);
#endif
    }
    if (!atomic_load_explicit(&c->reads, memory_order_relaxed) ||
        !atomic_load_explicit(&c->renders, memory_order_relaxed)) { abort(); }
    tracker_engine_t *identity = c->engine;
    for (unsigned i = 0; i < 100U; ++i) {
        tracker_engine_prepared_t *song = tracker_engine_prepare(module, size);
        app_sampler_bank_t *bank = app_sampler_bank_from_s3m(module, size, &result);
        if (!song || !bank) { abort(); }
        one_check(c, app_sampler_bank_apply_slots(bank, song) == APP_SAMPLER_OK);
        bool gate = false;
        for (unsigned attempt = 0; attempt < 100000U; ++attempt) {
            if (app_audio_bus_begin_edit(c->bus, 0)) { gate = true; break; }
#if defined(_WIN32)
            SwitchToThread();
#else
            sched_yield();
#endif
        }
        if (!gate) { abort(); }
        tracker_engine_commit(c->engine, song);
        app_sampler_bank_commit(c->sampler, bank);
        tracker_engine_play(c->engine);
        app_audio_bus_end_edit(c->bus);
        one_check(c, c->engine == identity);
        one_check(c, app_sampler_audition(c->sampler, 0) == APP_SAMPLER_OK);
        app_sampler_audition_stop(c->sampler);
    }
    atomic_store_explicit(&c->stop, true, memory_order_release);
#if defined(_WIN32)
    if (WaitForSingleObject(reader, 10000) != WAIT_OBJECT_0 || WaitForSingleObject(renderer, 10000) != WAIT_OBJECT_0) { abort(); }
    CloseHandle(reader); CloseHandle(renderer);
#else
    (void)pthread_join(reader, NULL); (void)pthread_join(renderer, NULL);
#endif
    const unsigned failures = atomic_load_explicit(&c->failures, memory_order_relaxed);
    const unsigned reads = atomic_load_explicit(&c->reads, memory_order_relaxed);
    const unsigned renders = atomic_load_explicit(&c->renders, memory_order_relaxed);
    if (failures || !reads || !renders) {
        fprintf(stderr, "sampler concurrent FAIL: %u failures, %u reads, %u renders\n", failures, reads, renders); exit(1);
    }
    app_audio_bus_destroy(c->bus); app_sampler_destroy(c->sampler); tracker_engine_destroy(c->engine); free(c);
    printf("sampler: 100 staged project swaps with %u live metadata reads / %u render blocks PASS\n", reads, renders);
}
