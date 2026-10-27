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
    app_note_router_t *router;
    app_usb_midi_t *usb;
    app_settings_snapshot_t settings;
    app_settings_idle_t idle;
    bool idle_busy;
    host_audio_t *audio;
    uint8_t *module;
    void *audio_timer;
    void *state_timer;
    FILE *state_log;
} backend;

static bool read_settings(void *context, app_settings_snapshot_t *snapshot)
{
    (void)context;
    app_settings_idle_target_t target;
    app_settings_idle_evaluate(&backend.idle, gsp_sim_bridge_time_ms(), backend.idle_busy,
                               backend.settings.brightness, &target);
    backend.settings.effective_brightness = target.effective_brightness;
    backend.settings.dimmed = target.dimmed;
    backend.settings.screen_off = target.screen_off;
    *snapshot = backend.settings;
    return true;
}

static bool request_idle(void *context, uint32_t dim_after_s, uint32_t off_after_s)
{
    (void)context;
    if (!app_settings_idle_configure(&backend.idle, dim_after_s, off_after_s, gsp_sim_bridge_time_ms())) {
        return false;
    }
    backend.settings.dim_after_s = dim_after_s;
    backend.settings.off_after_s = off_after_s;
    return true;
}

static bool record_activity(void *context)
{
    (void)context;
    app_settings_snapshot_t before;
    (void)read_settings(NULL, &before);
    app_settings_idle_record_activity(&backend.idle, gsp_sim_bridge_time_ms());
    return before.screen_off;
}

static void interaction_busy(void *context, bool busy)
{
    (void)context;
    backend.idle_busy = busy;
}

static bool request_settings(void *context, int brightness, int rotation, bool locked)
{
    (void)context;
    backend.settings.brightness = brightness;
    backend.settings.rotation = rotation;
    backend.settings.rotation_locked = locked;
    return true;
}

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
                "\"mute\":[%d,%d,%d,%d],\"solo\":[%d,%d,%d,%d],\"error\":%d,\"page_motion\":%d,\"menu_motion\":%d,\"progress\":%u}\n",
                (unsigned long long)gsp_sim_bridge_time_ms(), s.page, s.volume,
                s.mix_scroll, s.held_note, s.synth_tab, s.preset,
                s.playing ? "true" : "false", s.launched_pattern,
                s.pending_pattern, s.stop_pending ? "true" : "false",
                (unsigned)s.active_voices, (double)s.synth_peak,
                (unsigned long long)s.rendered_frames,
                s.gain_steps[0], s.gain_steps[1], s.gain_steps[2], s.gain_steps[3],
                s.muted[0], s.muted[1], s.muted[2], s.muted[3],
                s.soloed[0], s.soloed[1], s.soloed[2], s.soloed[3], s.last_error,
                s.page_animating, s.menu_animating, (unsigned)s.transition_progress);
        fflush(backend.state_log);
    }
    app_tracker_settings_snapshot_t q;
    if (app_tracker_read_settings(backend.controller, &q) == ESP_GSP_OK) {
        const char *path = getenv("OP_AY1_SIM_SETTINGS_LOG");
        if (path && *path) {
            FILE *file = fopen(path, "a");
            if (file) {
                fprintf(file, "{\"open\":%d,\"confirm\":%d,\"timeouts\":%d,\"brightness\":%d,\"effective\":%d,\"dim_s\":%lu,\"off_s\":%lu,\"dimmed\":%d,\"screen_off\":%d,\"rotation\":%d,\"locked\":%d,\"usb_mode\":%d,\"usb_state\":%d,\"usb_error\":%ld}\n",
                        q.menu_open, q.confirmation_open, q.timeouts_open, q.display.brightness, q.display.effective_brightness,
                        (unsigned long)q.display.dim_after_s, (unsigned long)q.display.off_after_s,
                        q.display.dimmed, q.display.screen_off, q.display.rotation,
                        q.display.rotation_locked, q.usb.actual_mode, q.usb.state, (long)q.usb.last_error);
                fclose(file);
            }
        }
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
    backend.router = app_note_router_create(backend.synth);
    if (!backend.router || !app_usb_midi_create(backend.router, &backend.usb)) {
        return ESP_GSP_ERR_NO_MEM;
    }
    app_settings_snapshot_defaults(&backend.settings);
    app_settings_idle_init(&backend.idle, gsp_sim_bridge_time_ms());
    const char *state_path = getenv("OP_AY1_SIM_STATE_LOG");
    if (state_path && *state_path) {
        backend.state_log = fopen(state_path, "w");
        if (!backend.state_log) {
            return ESP_GSP_FAIL;
        }
    }
    const app_tracker_config_t config = {
        .tracker = backend.tracker, .synth = backend.synth, .audio_bus = backend.bus,
        .note_router = backend.router, .usb_midi = backend.usb,
        .visualize_display_power = true,
        .hooks = {
            .time_ms = clock_ms, .set_volume = set_volume,
            .read_settings = read_settings, .request_settings = request_settings,
            .request_idle = request_idle, .record_activity = record_activity,
            .interaction_busy = interaction_busy,
        },
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
    (void)app_usb_midi_shutdown(&backend.usb, 1000);
    app_note_router_destroy(backend.router);
    app_audio_bus_destroy(backend.bus);
    app_synth_destroy(backend.synth);
    tracker_engine_destroy(backend.tracker);
    free(backend.module);
    if (backend.state_log) {
        fclose(backend.state_log);
    }
}
