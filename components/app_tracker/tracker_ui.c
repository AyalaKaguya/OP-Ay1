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
#include "tracker_settings.h"
#include "tracker_transition.h"
#include "tracker_workspace.h"

#define UI_TICK_MS 50
#define FRAME_WIDTH 480
#define FRAME_HEIGHT 480
#define FRAME_BYTES (FRAME_WIDTH * FRAME_HEIGHT * sizeof(uint16_t))

struct app_tracker {
    esp_gsp_handle_t ui;
    app_tracker_hooks_t hooks;
    tracker_model_t model;
    tracker_settings_t settings;
    tracker_workspace_t workspace;
    app_usb_midi_t *usb;
    uint16_t *frames[2];
    uint16_t *motion_frame;
    uint16_t *open_frame;
    tracker_transition_t page_motion;
    tracker_transition_t drawer_motion;
    int rendered_page;
    int page_direction;
    bool page_vertical;
    bool page_zoom;
    bool back_motion;
    bool rendered_menu;
    bool motion_gesture;
    bool navigation_pointer;
    int navigation_x;
    int navigation_y;
    bool back_pointer;
    int back_x;
    int back_y;
    uint64_t navigation_hint_until;
    uint64_t home_hint_until;
    bool visualize_display_power;
    atomic_uint frame_index;
    int16_t wave_scratch[TRACKER_WAVEFORM_SAMPLES];
    void *timer;
    atomic_uint transport_requests;
    atomic_bool back_requests;
    uint64_t transport_generation;
    atomic_bool ready;
    uint64_t elapsed_ms;
    unsigned ticks;
    int applied_volume;
    bool wake_gesture;
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

static void render_navigation_hint(app_tracker_t *state, uint16_t *frame, uint64_t now)
{
    const tracker_workspace_t *w = &state->workspace;
    if (now >= state->navigation_hint_until || w->screen != TRACKER_WORKSPACE_HOME) {
        return;
    }
    const int count = 3;
    const int selected = w->home_page;
    const int width = count * 16 + 20;
    const int left = (FRAME_WIDTH - width) / 2;
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    tracker_gfx_clip(&g, 0, 0, FRAME_WIDTH, FRAME_HEIGHT);
    /* A transient overlay belongs to the displayed frame, never a hit target
     * or a panel that slides with either the source or destination page. */
    tracker_gfx_rect(&g, left, 441, width, 27, C_PANEL);
    for (int i = 0; i < count; ++i) {
        tracker_gfx_rect(&g, left + 14 + i * 16, 451, 7, 7,
                         i == selected ? C_TEXT : C_DISABLED);
    }
}

static bool has_parent(const app_tracker_t *state)
{
    return (state->workspace.screen == TRACKER_WORKSPACE_SETTINGS &&
            (state->settings.timeouts_open || state->settings.usb_confirm)) ||
           tracker_workspace_has_parent(&state->workspace);
}

static int view_route(const app_tracker_t *state)
{
    int route = tracker_workspace_route(&state->workspace, &state->model);
    if (state->workspace.screen == TRACKER_WORKSPACE_SETTINGS && state->settings.timeouts_open) {
        route += 1000;
    }
    if (state->workspace.screen == TRACKER_WORKSPACE_SAMPLER &&
            state->workspace.sampler_tab == TRACKER_SAMPLE_ASSIGN) {
        route += 2000;
    }
    return route;
}

static void render_gesture_hints(app_tracker_t *state, uint16_t *frame, uint64_t now)
{
    if (state->workspace.screen == TRACKER_WORKSPACE_HOME || state->settings.open) {
        return;
    }
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    tracker_gfx_clip(&g, 0, 0, FRAME_WIDTH, FRAME_HEIGHT);
    if (now < state->home_hint_until) {
        tracker_gfx_rect(&g, 214, 472, 52, 3, C_MUTED);
    }
    if (has_parent(state)) {
        tracker_gfx_line(&g, 7, 230, 17, 240, C_CYAN);
        tracker_gfx_line(&g, 17, 240, 7, 250, C_CYAN);
    }
}

static void render_frame(app_tracker_t *state)
{
    tracker_model_t *m = &state->model;
    const unsigned current = atomic_load_explicit(&state->frame_index,
                                                  memory_order_acquire) & 1U;
    uint16_t *output = state->frames[current ^ 1U];
    uint16_t *frame = state->open_frame;
    const uint64_t now = now_ms(state);
    const int route = view_route(state);
    if (state->rendered_page != route) {
        /* Retarget from the image actually displayed, including an interrupted
         * slide. This snapshot is reused; no render-time allocation is needed. */
        memcpy(state->motion_frame, state->frames[current], FRAME_BYTES);
        state->page_vertical = state->workspace.screen == TRACKER_WORKSPACE_HOME &&
                               state->rendered_page >= 10;
        state->page_zoom = state->workspace.screen != TRACKER_WORKSPACE_HOME && !state->back_motion;
        state->page_direction = state->back_motion ? -1 : route > state->rendered_page ? 1 : -1;
        state->back_motion = false;
        state->home_hint_until = state->workspace.screen != TRACKER_WORKSPACE_HOME
                                 ? now + TRACKER_NAVIGATION_HINT_MS : 0;
        tracker_transition_begin(&state->page_motion, 0, TRACKER_TRANSITION_FULL,
                                 now, TRACKER_TRANSITION_PAGE_MS);
        state->rendered_page = route;
    }
    if (state->rendered_menu != state->settings.open) {
        tracker_transition_retarget(&state->drawer_motion,
                                    state->settings.open ? TRACKER_TRANSITION_FULL : 0,
                                    now, TRACKER_TRANSITION_DRAWER_MS);
        state->rendered_menu = state->settings.open;
        /* The drawer reuses the motion snapshot as a fresh background. */
        tracker_transition_init(&state->page_motion, TRACKER_TRANSITION_FULL);
    }
    if (state->workspace.screen == TRACKER_WORKSPACE_SETTINGS) {
        memset(frame, 0, FRAME_BYTES);
        tracker_settings_render_page(frame, &state->settings);
    } else if (state->workspace.screen != TRACKER_WORKSPACE_LEGACY) {
        tracker_workspace_render(frame, &state->workspace, m);
    } else if (m->view.page == TRACKER_PAGE_PATTERN) {
        tracker_render_pattern(frame, m->engine);
    } else if (m->view.page == TRACKER_PAGE_WAVES) {
        tracker_render_waves(frame, m->engine, state->wave_scratch,
                             TRACKER_WAVEFORM_SAMPLES);
    } else if (m->view.page == TRACKER_PAGE_SYNTH) {
        tracker_render_synth(frame, &m->synth_view);
    } else {
        tracker_render_controls(frame, &m->view);
    }
    if (state->workspace.screen != TRACKER_WORKSPACE_HOME) {
        tracker_workspace_fit_content(frame, &state->workspace);
    }
    tracker_workspace_render_header(frame, &state->workspace, m);
    const uint16_t page_progress = tracker_transition_sample(&state->page_motion, now);
    if (state->page_motion.active) {
        if (state->page_zoom) {
            (void)tracker_transition_expand_center_rgb565(output, state->motion_frame, frame,
                                                          FRAME_WIDTH, FRAME_HEIGHT, FRAME_HEIGHT,
                                                          page_progress);
        } else if (state->page_vertical) {
            (void)tracker_transition_slide_up_rgb565(output, state->motion_frame, frame,
                                                     FRAME_WIDTH, FRAME_HEIGHT, FRAME_HEIGHT,
                                                     page_progress);
        } else {
            (void)tracker_transition_slide_rgb565(output, state->motion_frame, frame,
                                                  FRAME_WIDTH, FRAME_HEIGHT, TRACKER_WORKSPACE_CONTENT_HEIGHT,
                                                  page_progress, state->page_direction);
            if (state->workspace.screen == TRACKER_WORKSPACE_HOME) {
                tracker_workspace_render_header(output, &state->workspace, m);
            }
        }
    } else {
        memcpy(output, frame, FRAME_BYTES);
    }
    render_navigation_hint(state, output, now);
    render_gesture_hints(state, output, now);
    const uint16_t drawer_amount = tracker_transition_sample(&state->drawer_motion, now);
    if (drawer_amount || state->drawer_motion.active) {
        memcpy(state->motion_frame, output, FRAME_BYTES);
        tracker_settings_render_panel(output, &state->settings);
        (void)tracker_transition_reveal_top_rgb565(output, state->motion_frame, output,
                                                   FRAME_WIDTH, FRAME_HEIGHT, FRAME_HEIGHT,
                                                   TRACKER_SETTINGS_PANEL_HEIGHT,
                                                   drawer_amount, 192);
    }
    if (state->settings.display.screen_off) {
        memset(output, 0, FRAME_BYTES);
    } else if (state->visualize_display_power && state->settings.display.dimmed) {
        const unsigned preferred = (unsigned)state->settings.display.brightness;
        const unsigned effective = (unsigned)state->settings.display.effective_brightness;
        if (preferred) {
            for (unsigned i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; ++i) {
                const unsigned pixel = output[i];
                const unsigned r = ((pixel >> 11) & 31U) * effective / preferred;
                const unsigned g = ((pixel >> 5) & 63U) * effective / preferred;
                const unsigned b = (pixel & 31U) * effective / preferred;
                output[i] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
    }
    atomic_store_explicit(&state->frame_index, current ^ 1U, memory_order_release);
    m->dirty = false;
    state->workspace.dirty = false;
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

static void update_settings(app_tracker_t *state)
{
    tracker_settings_t *s = &state->settings;
    if (state->hooks.interaction_busy) {
        state->hooks.interaction_busy(state->hooks.context,
                                      state->model.pointer_down || state->workspace.pointer_down ||
                                      state->workspace.sampler_pointer || tracker_workspace_busy(&state->workspace) ||
                                      s->pointer_down || state->motion_gesture ||
                                      state->navigation_pointer ||
                                      state->back_pointer ||
                                      state->wake_gesture || state->model.synth_view.note >= 0 ||
                                      state->page_motion.active || state->drawer_motion.active);
    }
    if (s->display_request && state->hooks.request_settings) {
        if (state->hooks.request_settings(state->hooks.context, s->requested_brightness,
                                          s->requested_rotation, s->requested_locked)) {
            s->display_request = false;
            s->awaiting_display = true;
        }
    }
    if (s->usb_request && state->usb) {
        /* A failed request has a published error. Retry requires another tap,
         * so unsupported native USB and transient failures cannot spin here. */
        (void)app_usb_midi_request_mode(state->usb, s->requested_usb);
        s->usb_request = false;
    }
    if (s->idle_request && state->hooks.request_idle &&
            state->hooks.request_idle(state->hooks.context, s->requested_dim_s, s->requested_off_s)) {
        s->idle_request = false;
        s->awaiting_idle = true;
    }
    if (state->hooks.read_settings) {
        app_settings_snapshot_t next;
        if (state->hooks.read_settings(state->hooks.context, &next)) {
            if (s->display.screen_off != next.screen_off || s->display.dimmed != next.dimmed ||
                    s->display.effective_brightness != next.effective_brightness) {
                state->model.dirty = true;
            }
            tracker_settings_receive_display(s, &next);
        }
    }
    if (state->usb) {
        app_usb_midi_get_status(state->usb, &s->usb);
    }
}

static void finish_source_edit(app_tracker_t *state, uint32_t before)
{
    tracker_workspace_t *w = &state->workspace;
    const bool changed = state->model.edit_revision != before;
    if (changed && w->project) {
        (void)app_project_set_controls(w->project, state->model.view.volume,
                                       w->project_view.pad_patterns);
    }
    tracker_workspace_end_source_edit(w, changed);
}

static void tick_sources(app_tracker_t *state)
{
    if (tracker_workspace_busy(&state->workspace)) {
        (void)atomic_exchange_explicit(&state->transport_requests, 0, memory_order_acquire);
        return;
    }
    if (!tracker_workspace_begin_source_edit(&state->workspace)) {
        return;
    }
    if (state->transport_generation != state->workspace.session_generation) {
        /* A button request made for the old/busy session cannot start the new
         * project after a successful replacement. */
        (void)atomic_exchange_explicit(&state->transport_requests, 0, memory_order_acquire);
        state->transport_generation = state->workspace.session_generation;
    }
    const uint32_t before = state->model.edit_revision;
    const unsigned requests = atomic_exchange_explicit(&state->transport_requests,
                                                       0, memory_order_acquire);
    if (requests && (!state->workspace.project || state->workspace.project_view.opened)) {
        tracker_model_transport(&state->model, (requests & 2U) != 0, false);
    }
    tracker_model_tick(&state->model, now_ms(state));
    apply_volume(state);
    finish_source_edit(state, before);
}

static bool go_back(app_tracker_t *state)
{
    const int before = view_route(state);
    if (state->settings.usb_confirm) {
        state->settings.usb_confirm = false;
    } else if (state->settings.timeouts_open) {
        state->settings.timeouts_open = false;
    } else if (state->settings.open) {
        state->settings.open = false;
    } else {
        (void)tracker_workspace_back(&state->workspace, &state->model);
    }
    state->back_motion = view_route(state) != before;
    state->model.dirty = true;
    return state->back_motion;
}

static bool back_pointer(app_tracker_t *state, int x, int y, bool pressed)
{
    if (!state->back_pointer) {
        if (!pressed || x < 0 || x > 18 || y < 32 || y >= TRACKER_NAVIGATION_EDGE_Y ||
                state->settings.open || !has_parent(state) ||
                state->model.pointer_down || state->workspace.pointer_down || state->workspace.sampler_pointer ||
                state->settings.pointer_down || state->settings.page_pointer_down) {
            return false;
        }
        state->back_pointer = true;
        state->back_x = x;
        state->back_y = y;
    } else if (!pressed) {
        const int dx = x - state->back_x;
        const int dy = y - state->back_y;
        state->back_pointer = false;
        if (dx >= TRACKER_NAVIGATION_TRAVEL && dx > abs(dy) * 2) {
            (void)go_back(state);
        }
    }
    return true;
}

static bool navigation_pointer(app_tracker_t *state, int x, int y, bool pressed)
{
    tracker_workspace_t *w = &state->workspace;
    tracker_model_t *m = &state->model;
    if (!state->navigation_pointer) {
        if (!pressed || y < TRACKER_NAVIGATION_EDGE_Y || state->settings.open ||
                state->settings.pointer_down || state->settings.page_pointer_down ||
                m->pointer_down || w->pointer_down || w->sampler_pointer) {
            return false;
        }
        state->navigation_pointer = true;
        state->navigation_x = x;
        state->navigation_y = y;
    } else if (!pressed) {
        const int dx = x - state->navigation_x;
        const int dy = y - state->navigation_y;
        state->navigation_pointer = false;
        state->back_pointer = false;
        if (dy <= -TRACKER_NAVIGATION_TRAVEL && -dy > abs(dx) * 2) {
            tracker_workspace_cancel_input(w);
            tracker_workspace_home(w, m);
            state->navigation_hint_until = 0;
        } else if (abs(dx) >= TRACKER_NAVIGATION_TRAVEL && abs(dx) > abs(dy) * 2) {
            const int before = tracker_workspace_route(w, m);
            if (w->screen == TRACKER_WORKSPACE_HOME) {
                tracker_workspace_swipe_home(w, m, dx < 0 ? 1 : -1);
            }
            if (before != tracker_workspace_route(w, m)) {
                state->navigation_hint_until = now_ms(state) + TRACKER_NAVIGATION_HINT_MS;
            }
        }
    }
    return true;
}

static void pointer(esp_gsp_handle_t ui, int32_t x, int32_t y,
                    bool pressed, void *context)
{
    (void)ui;
    app_tracker_t *state = context;
    if (!atomic_load_explicit(&state->ready, memory_order_acquire)) {
        return;
    }
    if (state->hooks.record_activity && state->hooks.record_activity(state->hooks.context)) {
        state->wake_gesture = true;
        tracker_model_cancel_input(&state->model);
        tracker_workspace_cancel_input(&state->workspace);
        state->settings.pointer_down = false;
        state->settings.page_pointer_down = false;
        state->settings.top_candidate = false;
        state->settings.brightness_drag = false;
        state->navigation_pointer = false;
        state->back_pointer = false;
    }
    if (state->wake_gesture) {
        if (!pressed) {
            state->wake_gesture = false;
        }
        update_settings(state);
        return;
    }
    const bool drawing_motion = state->page_motion.active || state->drawer_motion.active;
    if (state->motion_gesture || (pressed && drawing_motion &&
                                  !state->navigation_pointer &&
                                  !state->back_pointer &&
                                  !state->model.pointer_down && !state->workspace.pointer_down &&
                                  !state->workspace.sampler_pointer && !state->settings.pointer_down && !state->settings.page_pointer_down &&
                                  (state->drawer_motion.active || y < TRACKER_NAVIGATION_EDGE_Y))) {
        state->motion_gesture = pressed;
        update_settings(state);
        return;
    }
    tracker_workspace_poll(&state->workspace, &state->model);
    if (navigation_pointer(state, x, y, pressed)) {
        update_settings(state);
        tick_sources(state);
        repaint(state);
        return;
    }
    if (back_pointer(state, x, y, pressed)) {
        update_settings(state);
        tick_sources(state);
        repaint(state);
        return;
    }
    const int before_home = state->workspace.home_page;
    const tracker_workspace_screen_t before_screen = state->workspace.screen;
    const int content_y = tracker_workspace_content_y(&state->workspace, y);
    bool opened = false;
    const bool settings_page = !state->settings.open && !state->settings.top_candidate &&
                               state->workspace.screen == TRACKER_WORKSPACE_SETTINGS &&
                               (state->settings.pointer_down ||
                                (!state->workspace.pointer_down && y >= 32 && y < TRACKER_WORKSPACE_CONTENT_HEIGHT));
    const bool top_tap = !pressed && state->settings.top_candidate && !state->settings.moved &&
                         abs(x - state->settings.start_x) <= 12 && abs(y - state->settings.start_y) <= 12;
    if (!settings_page && tracker_settings_pointer(&state->settings, x, y, pressed, &opened)) {
        if (opened) {
            tracker_model_cancel_input(&state->model);
            tracker_workspace_cancel_input(&state->workspace);
        } else if (top_tap && !state->settings.open) {
            (void)tracker_workspace_pointer(&state->workspace, &state->model,
                                            &state->settings, x, content_y, true);
            (void)tracker_workspace_pointer(&state->workspace, &state->model,
                                            &state->settings, x, content_y, false);
        }
        state->model.dirty = true;
    } else {
        const bool consumed = tracker_workspace_pointer(&state->workspace, &state->model,
                                                        &state->settings, x, content_y, pressed);
        if (!consumed && tracker_workspace_begin_source_edit(&state->workspace)) {
            const uint32_t before = state->model.edit_revision;
            tracker_model_pointer(&state->model, x, tracker_workspace_legacy_y(y), pressed, now_ms(state));
            tracker_model_tick(&state->model, now_ms(state));
            apply_volume(state);
            finish_source_edit(state, before);
        }
    }
    if (!pressed && before_screen == TRACKER_WORKSPACE_HOME && before_screen == state->workspace.screen &&
            before_home != state->workspace.home_page) {
        state->navigation_hint_until = now_ms(state) + TRACKER_NAVIGATION_HINT_MS;
    } else if (before_screen != state->workspace.screen) {
        state->navigation_hint_until = 0;
    }
    update_settings(state);
    tick_sources(state);
    if (state->model.dirty || state->workspace.dirty) {
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
        tracker_workspace_cancel_input(&state->workspace);
        state->settings.pointer_down = false;
        state->settings.page_pointer_down = false;
        state->settings.brightness_drag = false;
        state->settings.top_candidate = false;
        state->wake_gesture = false;
        state->motion_gesture = false;
        state->navigation_pointer = false;
        state->back_pointer = false;
        if (state->hooks.interaction_busy) {
            state->hooks.interaction_busy(state->hooks.context, false);
        }
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
    tracker_workspace_poll(&state->workspace, &state->model);
    if (atomic_exchange_explicit(&state->back_requests, false, memory_order_acquire)) {
        const bool woke = state->hooks.record_activity && state->hooks.record_activity(state->hooks.context);
        app_tracker_cancel_pointer(state);
        if (!woke) {
            (void)go_back(state);
        }
    }
    update_settings(state);
    tick_sources(state);
    /* Waveforms retain 20Hz; controls refresh at 10Hz and input is immediate. */
    if (state->model.dirty || state->workspace.dirty || (!state->settings.display.screen_off &&
                                                         (state->page_motion.active || state->drawer_motion.active || state->model.view.page == TRACKER_PAGE_WAVES ||
                                                          state->ticks % 2 == 0))) {
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
    state->visualize_display_power = config->visualize_display_power;
    state->usb = config->usb_midi;
    tracker_settings_init(&state->settings);
    atomic_init(&state->transport_requests, 0);
    atomic_init(&state->back_requests, false);
    atomic_init(&state->frame_index, 0);
    atomic_init(&state->ready, false);
    state->transport_generation = UINT64_MAX;
    state->applied_volume = -1;
    for (unsigned index = 0; index < 2; ++index) {
        state->frames[index] = state->hooks.allocate_frame ?
                               state->hooks.allocate_frame(state->hooks.context, FRAME_BYTES) : malloc(FRAME_BYTES);
    }
    state->motion_frame = state->hooks.allocate_frame ?
                          state->hooks.allocate_frame(state->hooks.context, FRAME_BYTES) : malloc(FRAME_BYTES);
    state->open_frame = state->hooks.allocate_frame ?
                        state->hooks.allocate_frame(state->hooks.context, FRAME_BYTES) : malloc(FRAME_BYTES);
    if (!state->frames[0] || !state->frames[1] || !state->motion_frame || !state->open_frame) {
        for (unsigned index = 0; index < 2; ++index) {
            if (state->frames[index]) {
                if (state->hooks.free_frame) {
                    state->hooks.free_frame(state->hooks.context, state->frames[index]);
                } else {
                    free(state->frames[index]);
                }
            }
        }
        if (state->motion_frame) {
            if (state->hooks.free_frame) {
                state->hooks.free_frame(state->hooks.context, state->motion_frame);
            } else {
                free(state->motion_frame);
            }
        }
        if (state->open_frame) {
            if (state->hooks.free_frame) {
                state->hooks.free_frame(state->hooks.context, state->open_frame);
            } else {
                free(state->open_frame);
            }
        }
        free(state);
        return ESP_GSP_ERR_NO_MEM;
    }
    tracker_model_init(&state->model, config->tracker, config->synth, config->audio_bus);
    tracker_workspace_init(&state->workspace, config->project, config->sampler);
    state->rendered_page = view_route(state);
    tracker_transition_init(&state->page_motion, TRACKER_TRANSITION_FULL);
    tracker_transition_init(&state->drawer_motion, 0);
    state->model.router = config->note_router;
    tracker_workspace_poll(&state->workspace, &state->model);
    update_settings(state);
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
    s->transition_progress = state->page_motion.current;
    s->page_animating = state->page_motion.active;
    s->menu_animating = state->drawer_motion.active;
    s->workspace_screen = state->workspace.screen;
    s->home_page = state->workspace.home_page;
    s->project_opened = state->workspace.project_view.opened;
    memcpy(s->project_name, state->workspace.project_view.name, sizeof(s->project_name));
    s->project_name[sizeof(s->project_name) - 1U] = '\0';
    s->project_busy = tracker_workspace_busy(&state->workspace);
    s->project_dirty = state->workspace.project_view.dirty;
    s->project_generation = state->workspace.project_view.session_generation;
    const tracker_workspace_t *w = &state->workspace;
    s->sampler_tab = w->sampler_tab;
    s->sample_count = w->sample_count;
    s->sample_index = w->sample;
    s->sample_edit_active = w->sample_edit_active;
    s->sample_edit_dirty = w->sample_edit_dirty;
    s->sample_trim_start = w->sample_edit_active ? w->edit.trim_start : w->sample_info.trim_start;
    s->sample_trim_end = w->sample_edit_active ? w->edit.trim_end : w->sample_info.trim_end;
    s->sample_loop_start = w->sample_edit_active ? w->edit.loop_start : w->sample_info.loop_start;
    s->sample_loop_end = w->sample_edit_active ? w->edit.loop_end : w->sample_info.loop_end;
    s->sample_c2 = w->sample_edit_active ? w->edit.c2_speed : w->sample_info.c2_speed;
    s->sample_gain = w->sample_edit_active ? w->edit.gain : w->sample_info.gain;
    s->sample_loop_enabled = w->sample_edit_active ? w->edit.loop_enabled : w->sample_info.loop_enabled;
    s->record_state = w->record.state;
    s->record_frames = w->record.frames;
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

esp_gsp_err_t app_tracker_read_settings(app_tracker_t *state,
                                        app_tracker_settings_snapshot_t *snapshot)
{
    if (!state || !snapshot) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    snapshot->display = state->settings.display;
    snapshot->usb = state->settings.usb;
    snapshot->menu_open = state->settings.open;
    snapshot->confirmation_open = state->settings.usb_confirm;
    snapshot->timeouts_open = state->settings.timeouts_open;
    return ESP_GSP_OK;
}

void app_tracker_request_transport(app_tracker_t *state, bool restart)
{
    if (state) {
        atomic_fetch_or_explicit(&state->transport_requests, restart ? 2U : 1U,
                                 memory_order_release);
    }
}

void app_tracker_request_back(app_tracker_t *state)
{
    if (state) {
        atomic_store_explicit(&state->back_requests, true, memory_order_release);
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
    tracker_sampler_ui_cancel(&state->workspace);
    if (state->hooks.interaction_busy) {
        state->hooks.interaction_busy(state->hooks.context, false);
    }
    for (unsigned index = 0; index < 2; ++index) {
        if (state->hooks.free_frame) {
            state->hooks.free_frame(state->hooks.context, state->frames[index]);
        } else {
            free(state->frames[index]);
        }
    }
    if (state->hooks.free_frame) {
        state->hooks.free_frame(state->hooks.context, state->motion_frame);
        state->hooks.free_frame(state->hooks.context, state->open_frame);
    } else {
        free(state->motion_frame);
        free(state->open_frame);
    }
    if (state->hooks.shutdown) {
        state->hooks.shutdown(state->hooks.context);
    }
    free(state);
}
