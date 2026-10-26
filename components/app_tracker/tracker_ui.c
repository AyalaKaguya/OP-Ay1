// SPDX-License-Identifier: Apache-2.0
/* The same GSP canvas, input state machine and renderers run on both hosts. */
#include "app_tracker.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "tracker_graphics.h"
#include "tracker_model.h"
#include "tracker_render.h"

#define UI_TICK_MS 50
#define FRAME_WIDTH 480
#define FRAME_HEIGHT 480
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * sizeof(uint16_t))

struct app_tracker {
    esp_gsp_handle_t ui;
    app_tracker_hooks_t hooks;
    tracker_model_t model;
    uint16_t *frames[2];
    atomic_uint frame_index;
    int16_t wave_scratch[TRACKER_WAVEFORM_SAMPLES];
    void *timer;
    atomic_uint transport_requests;
    atomic_bool ready;
    uint64_t elapsed_ms;
    unsigned ticks;
    int applied_volume;
    esp_gsp_err_t last_error;
};

static uint64_t now_ms(app_tracker_t *state)
{
    return state->hooks.time_ms ? state->hooks.time_ms(state->hooks.context)
           : state->elapsed_ms;
}

static void remember_error(app_tracker_t *state, esp_gsp_err_t result)
{
    if (result != ESP_GSP_OK) {
        state->last_error = result;
    }
}

static void render_frame(app_tracker_t *state)
{
    tracker_model_t *m = &state->model;
    const unsigned current = atomic_load_explicit(&state->frame_index,
                                                  memory_order_acquire) & 1U;
    uint16_t *frame = state->frames[current ^ 1U];
    if (m->view.page == TRACKER_PAGE_PATTERN) {
        tracker_render_pattern(frame, m->engine);
    } else if (m->view.page == TRACKER_PAGE_WAVES) {
        tracker_render_waves(frame, m->engine, state->wave_scratch,
                             TRACKER_WAVEFORM_SAMPLES);
    } else if (m->view.page == TRACKER_PAGE_SYNTH) {
        tracker_render_synth(frame, &m->synth_view);
    } else {
        tracker_render_controls(frame, &m->view);
    }
    /* navigation takes a frame pointer through a short local graphics context. */
    tracker_graphics_t navigation_gfx;
    tracker_gfx_init(&navigation_gfx, frame);
    tracker_gfx_clip(&navigation_gfx, 0, 432, 480, 48);
    tracker_gfx_rect(&navigation_gfx, 0, 432, 480, 48, TRACKER_RGB565(9, 13, 15));
    for (int page = 0; page < TRACKER_PAGE_COUNT; ++page) {
        const int x = 200 + 16 * page;
        const uint16_t color = m->view.page == page ? C_TEXT : C_ROW;
        for (int y = -4; y <= 3; ++y) {
            for (int dx = -4; dx <= 3; ++dx) {
                if ((2 * dx + 1) * (2 * dx + 1) + (2 * y + 1) * (2 * y + 1) <= 64) {
                    tracker_gfx_rect(&navigation_gfx, x + dx, 460 + y, 1, 1, color);
                }
            }
        }
    }
    atomic_store_explicit(&state->frame_index, current ^ 1U, memory_order_release);
    m->dirty = false;
}

static void repaint(app_tracker_t *state)
{
    render_frame(state);
    remember_error(state, esp_gsp_canvas_invalidate(state->ui,
                                                    GSP_TRACKER_BIND_WORKSTATION));
}

static void apply_volume(app_tracker_t *state)
{
    if (state->model.view.volume != state->applied_volume) {
        if (state->hooks.set_volume) {
            state->hooks.set_volume(state->hooks.context, state->model.view.volume);
        }
        state->applied_volume = state->model.view.volume;
    }
}

static void pointer(esp_gsp_handle_t ui, int32_t x, int32_t y,
                    bool pressed, void *context)
{
    (void)ui;
    app_tracker_t *state = context;
    if (!atomic_load_explicit(&state->ready, memory_order_acquire)) {
        return;
    }
    tracker_model_pointer(&state->model, x, y, pressed, now_ms(state));
    tracker_model_tick(&state->model, now_ms(state));
    apply_volume(state);
    if (state->model.dirty) {
        repaint(state);
    }
}

void app_tracker_feed_pointer(app_tracker_t *state, int x, int y, bool pressed)
{
    if (state) {
        pointer(state->ui, x, y, pressed, state);
    }
}

void app_tracker_cancel_pointer(app_tracker_t *state)
{
    if (state) {
        tracker_model_cancel_input(&state->model);
        state->model.dirty = true;
    }
}

static void tick(esp_gsp_handle_t ui, void *context)
{
    (void)ui;
    app_tracker_t *state = context;
    if (!atomic_load_explicit(&state->ready, memory_order_acquire)) {
        return;
    }
    state->elapsed_ms += UI_TICK_MS;
    ++state->ticks;
    if (state->hooks.heartbeat) {
        state->hooks.heartbeat(state->hooks.context);
    }
    const unsigned requests = atomic_exchange_explicit(&state->transport_requests,
                                                       0, memory_order_acquire);
    if (requests) {
        tracker_model_transport(&state->model, (requests & 2U) != 0, false);
    }
    tracker_model_tick(&state->model, now_ms(state));
    apply_volume(state);
    /* Waveforms retain 20Hz; controls refresh at 10Hz and input is immediate. */
    if (state->model.dirty || state->model.view.page == TRACKER_PAGE_WAVES ||
            state->ticks % 2 == 0) {
        repaint(state);
    }
}

static void draw(const esp_gsp_canvas_surface_t *surface, void *context)
{
    const app_tracker_t *state = context;
    const unsigned frame_index = atomic_load_explicit(&state->frame_index,
                                                      memory_order_acquire) & 1U;
    const uint16_t *frame = state->frames[frame_index];
    if (!frame || surface->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565 ||
            surface->x + surface->width > FRAME_WIDTH ||
            surface->y + surface->height > FRAME_HEIGHT) {
        return;
    }
    for (uint16_t row = 0; row < surface->height; ++row) {
        uint8_t *destination = (uint8_t *)surface->pixels + (size_t)row * surface->stride_bytes;
        const uint16_t *source = frame +
                                 (size_t)(surface->y + row) * FRAME_WIDTH + surface->x;
        memcpy(destination, source, (size_t)surface->width * sizeof(uint16_t));
    }
}

esp_gsp_err_t app_tracker_create(esp_gsp_handle_t ui,
                                 const app_tracker_config_t *config,
                                 app_tracker_t **out_handle)
{
    if (!out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;
    if (!ui || !config || !config->tracker ||
            (!!config->hooks.allocate_frame != !!config->hooks.free_frame)) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    app_tracker_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_GSP_ERR_NO_MEM;
    }
    state->ui = ui;
    state->hooks = config->hooks;
    atomic_init(&state->transport_requests, 0);
    atomic_init(&state->frame_index, 0);
    atomic_init(&state->ready, false);
    state->applied_volume = -1;
    for (unsigned index = 0; index < 2; ++index) {
        state->frames[index] = state->hooks.allocate_frame ?
                               state->hooks.allocate_frame(state->hooks.context, FRAME_BYTES) : malloc(FRAME_BYTES);
    }
    if (!state->frames[0] || !state->frames[1]) {
        for (unsigned index = 0; index < 2; ++index) {
            if (state->frames[index]) {
                if (state->hooks.free_frame) {
                    state->hooks.free_frame(state->hooks.context, state->frames[index]);
                } else {
                    free(state->frames[index]);
                }
            }
        }
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
    tracker_model_init(&state->model, config->tracker, config->synth, config->audio_bus);
    tracker_model_tick(&state->model, now_ms(state));
    apply_volume(state);
    render_frame(state);
    esp_gsp_err_t result = esp_gsp_canvas_set_draw_cb(ui, GSP_TRACKER_BIND_WORKSTATION, draw, state);
    if (result == ESP_GSP_OK) {
        result = esp_gsp_set_pointer_observer(ui, pointer, state);
    }
    if (result == ESP_GSP_OK) {
        state->timer = esp_gsp_timer_create(ui, UI_TICK_MS, tick, state);
        if (!state->timer) {
            result = ESP_GSP_ERR_NO_MEM;
        }
    }
    if (result != ESP_GSP_OK) {
        state->last_error = result;
        state->hooks.shutdown = NULL;
        app_tracker_t *remaining = state;
        app_tracker_stop(&remaining);
        if (remaining) {
            /* Detachment could not be fenced: retain the live context for a
             * caller retry instead of freeing a selected callback's memory. */
            remaining->hooks.shutdown = config->hooks.shutdown;
            *out_handle = remaining;
        }
        return result;
    }
    atomic_store_explicit(&state->ready, true, memory_order_release);
    *out_handle = state;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_tracker_read(app_tracker_t *state, app_tracker_snapshot_t *s)
{
    if (!state || !s) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    const tracker_model_t *m = &state->model;
    memset(s, 0, sizeof(*s));
    s->page = m->view.page;
    s->volume = m->view.volume;
    s->mix_scroll = m->view.mix_scroll;
    s->held_note = m->synth_view.note;
    s->synth_tab = m->synth_view.tab;
    s->preset = m->synth_view.preset;
    s->playing = m->view.status.playing;
    s->launched_pattern = tracker_engine_launched_pattern(m->engine);
    s->pending_pattern = tracker_engine_pending_pattern(m->engine);
    s->stop_pending = tracker_engine_pattern_stop_pending(m->engine);
    s->active_voices = m->synth_view.status.active_voices;
    s->synth_peak = m->synth_view.status.peak;
    s->rendered_frames = m->synth_view.status.rendered_frames;
    s->last_error = state->last_error;
    const int first = m->view.mix_scroll / TRACKER_MIX_STRIP_H;
    for (int slot = 0; slot < 4; ++slot) {
        const int ch = first + slot;
        if (ch < TRACKER_MAX_CHANNELS) {
            s->gain_steps[slot] = m->view.gain_steps[ch];
            s->muted[slot] = m->view.muted[ch];
            s->soloed[slot] = m->view.soloed[ch];
        }
    }
    return ESP_GSP_OK;
}

void app_tracker_request_transport(app_tracker_t *state, bool restart)
{
    if (state) {
        atomic_fetch_or_explicit(&state->transport_requests, restart ? 2U : 1U,
                                 memory_order_release);
    }
}

void app_tracker_stop(app_tracker_t **handle)
{
    if (!handle || !*handle) {
        return;
    }
    app_tracker_t *state = *handle;
    atomic_store_explicit(&state->ready, false, memory_order_release);
    esp_gsp_err_t result = esp_gsp_set_pointer_observer(state->ui, NULL, NULL);
    if (state->timer) {
        const esp_gsp_err_t timer_result = esp_gsp_timer_delete(state->ui, state->timer);
        if (timer_result == ESP_GSP_OK) {
            state->timer = NULL;
        } else {
            result = timer_result;
        }
    }
    const esp_gsp_err_t canvas_result = esp_gsp_canvas_stop(state->ui,
                                                            GSP_TRACKER_BIND_WORKSTATION);
    if (canvas_result != ESP_GSP_OK) {
        result = canvas_result;
    }
#if defined(ESP_PLATFORM)
    /* The device teardown contract requires an external control task. */
    if (result == ESP_GSP_OK) {
        result = esp_gsp_flush(state->ui, 2000);
    }
#endif
    if (result != ESP_GSP_OK) {
        state->last_error = result;
        return;
    }
    *handle = NULL;
    tracker_model_cancel_input(&state->model);
    for (unsigned index = 0; index < 2; ++index) {
        if (state->hooks.free_frame) {
            state->hooks.free_frame(state->hooks.context, state->frames[index]);
        } else {
            free(state->frames[index]);
        }
    }
    if (state->hooks.shutdown) {
        state->hooks.shutdown(state->hooks.context);
    }
    free(state);
}
