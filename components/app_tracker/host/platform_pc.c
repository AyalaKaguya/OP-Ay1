// SPDX-License-Identifier: Apache-2.0
#include "app_tracker.h"

#include <stdio.h>
#include <stdlib.h>

#include "audio_pc.h"
#include "gsp_sim_bridge.h"

/* The fixed bridge ABI has no caller context. This adapter owns one lifecycle. */
static struct {
    app_tracker_t *controller;
    tracker_engine_t *tracker;
    app_synth_t *synth;
    app_audio_bus_t *bus;
    host_audio_t *audio;
    uint8_t *module;
    void *audio_timer;
    void *state_timer;
    FILE *state_log;
} backend;

static uint64_t clock_ms(void *context)
{
    (void)context;
    return gsp_sim_bridge_time_ms();
}

static void set_volume(void *context, int percent)
{
    (void)context;
    host_audio_volume(backend.audio, percent);
}

static void render_audio(esp_gsp_handle_t ui, void *context)
{
    (void)ui;
    (void)context;
    host_audio_step(backend.audio, gsp_sim_bridge_time_ms());
}

static void report_state(esp_gsp_handle_t ui, void *context)
{
    (void)ui;
    (void)context;
    if (!backend.state_log || !backend.controller) {
        return;
    }
    app_tracker_snapshot_t s;
    if (app_tracker_read(backend.controller, &s) == ESP_GSP_OK) {
        fprintf(backend.state_log,
                "{\"time\":%llu,\"page\":%d,\"volume\":%d,\"scroll\":%d,"
                "\"note\":%d,\"tab\":%d,\"preset\":%d,\"playing\":%s,"
                "\"launched\":%d,\"pending\":%d,\"stop\":%s,\"voices\":%u,"
                "\"peak\":%.6f,\"frames\":%llu,\"gain\":[%d,%d,%d,%d],"
                "\"mute\":[%d,%d,%d,%d],\"solo\":[%d,%d,%d,%d],\"error\":%d}\n",
                (unsigned long long)gsp_sim_bridge_time_ms(), s.page, s.volume,
                s.mix_scroll, s.held_note, s.synth_tab, s.preset,
                s.playing ? "true" : "false", s.launched_pattern,
                s.pending_pattern, s.stop_pending ? "true" : "false",
                (unsigned)s.active_voices, (double)s.synth_peak,
                (unsigned long long)s.rendered_frames,
                s.gain_steps[0], s.gain_steps[1], s.gain_steps[2], s.gain_steps[3],
                s.muted[0], s.muted[1], s.muted[2], s.muted[3],
                s.soloed[0], s.soloed[1], s.soloed[2], s.soloed[3], s.last_error);
        fflush(backend.state_log);
    }
}

static bool read_module(void)
{
    FILE *f = fopen(OP_AY1_MODULE_PATH, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    const long bytes = ftell(f);
    if (bytes <= 0 || bytes > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    backend.module = malloc((size_t)bytes);
    const bool loaded = backend.module && fread(backend.module, 1, (size_t)bytes, f) == (size_t)bytes;
    fclose(f);
    if (loaded) {
        backend.tracker = tracker_engine_create(backend.module, (size_t)bytes);
    }
    return backend.tracker != NULL;
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    if (!read_module()) {
        return ESP_GSP_ERR_INVALID_STATE;
    }
    backend.synth = app_synth_create(APP_AUDIO_SAMPLE_RATE);
    backend.bus = app_audio_bus_create(backend.tracker, backend.synth);
    if (!backend.synth || !backend.bus) {
        return ESP_GSP_ERR_NO_MEM;
    }
    backend.audio = host_audio_create(backend.bus);
    if (!backend.audio) {
        return ESP_GSP_ERR_NO_MEM;
    }
    const char *state_path = getenv("OP_AY1_SIM_STATE_LOG");
    if (state_path && *state_path) {
        backend.state_log = fopen(state_path, "w");
        if (!backend.state_log) {
            return ESP_GSP_FAIL;
        }
    }
    const app_tracker_config_t config = {
        .tracker = backend.tracker, .synth = backend.synth, .audio_bus = backend.bus,
        .hooks = {.time_ms = clock_ms, .set_volume = set_volume},
    };
    const esp_gsp_err_t result = app_tracker_create(ui, &config, &backend.controller);
    if (result != ESP_GSP_OK) {
        return result;
    }
    backend.audio_timer = esp_gsp_timer_create(ui, 10, render_audio, NULL);
    backend.state_timer = esp_gsp_timer_create(ui, 50, report_state, NULL);
    return backend.audio_timer && backend.state_timer ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    report_state(ui, NULL);
    if (backend.audio_timer) {
        (void)esp_gsp_timer_delete(ui, backend.audio_timer);
    }
    if (backend.state_timer) {
        (void)esp_gsp_timer_delete(ui, backend.state_timer);
    }
    app_tracker_stop(&backend.controller);
    if (backend.controller) {
        /* Keep borrowed sources alive until process exit if the bridge no
         * longer accepts the detach. It cannot call us after close(). */
        host_audio_destroy(backend.audio);
        if (backend.state_log) {
            fclose(backend.state_log);
        }
        return;
    }
    host_audio_destroy(backend.audio);
    app_audio_bus_destroy(backend.bus);
    app_synth_destroy(backend.synth);
    tracker_engine_destroy(backend.tracker);
    free(backend.module);
    if (backend.state_log) {
        fclose(backend.state_log);
    }
}
