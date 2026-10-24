// SPDX-License-Identifier: Apache-2.0
/*
 * ARYX tracker controller.
 *
 * Pages: WAVES | PATTERN | PLAYBACK | MIX | SEQUENCER.
 * MIX shows the master section on top and four channel slots below; a drag
 * inside the list scrolls the slots over all channels of the module.
 */

#include "app_tracker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_audio.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "tracker_render.h"

#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"

#define PAGE_WAVES 0
#define PAGE_PATTERN 1
#define PAGE_PLAYBACK 2
#define PAGE_MIX 3
#define PAGE_ORDER 4

#define FRAME_BYTES (TRACKER_FRAME_W * TRACKER_FRAME_H * sizeof(uint16_t))
#define UI_TICK_MS 50
#define PATTERN_REFRESH_TICKS 2
#define LABEL_REFRESH_TICKS 10
#define METER_REFRESH_TICKS 2
#define MIX_ROW_H 74
#define MIX_VIEW_Y 150
#define MIX_VIEW_H 292
#define PAGE_W_PX 480
#define MIX_MAX_SCROLL ((TRACKER_MIX_STRIPS - 4) * MIX_ROW_H)
/* Inertia tuning for the list: cap the launch speed and ignore momentum until
 * the finger has been quiet for a moment, so the drag itself is never doubled. */
#define MIX_FLING_MAX 90.0f
#define MIX_RELEASE_US 90000
#define GAIN_STEPS 120

static const char *TAG = "app_tracker";

typedef esp_err_t (*vis_fn)(esp_gsp_handle_t, bool);
typedef esp_err_t (*value_fn)(esp_gsp_handle_t, int32_t);
typedef esp_err_t (*text_fn)(esp_gsp_handle_t, const char *);

static const vis_fn k_mute_off[4] = {
    gsp_tracker_smo0_set_visible, gsp_tracker_smo1_set_visible,
    gsp_tracker_smo2_set_visible, gsp_tracker_smo3_set_visible,
};
static const vis_fn k_mute_on[4] = {
    gsp_tracker_smn0_set_visible, gsp_tracker_smn1_set_visible,
    gsp_tracker_smn2_set_visible, gsp_tracker_smn3_set_visible,
};
static const vis_fn k_solo_off[4] = {
    gsp_tracker_sso0_set_visible, gsp_tracker_sso1_set_visible,
    gsp_tracker_sso2_set_visible, gsp_tracker_sso3_set_visible,
};
static const vis_fn k_solo_on[4] = {
    gsp_tracker_ssn0_set_visible, gsp_tracker_ssn1_set_visible,
    gsp_tracker_ssn2_set_visible, gsp_tracker_ssn3_set_visible,
};
static const value_fn k_fader_set[4] = {
    gsp_tracker_sf0_set_value, gsp_tracker_sf1_set_value,
    gsp_tracker_sf2_set_value, gsp_tracker_sf3_set_value,
};
static const text_fn k_fader_db[4] = {
    gsp_tracker_slot0_db_set_text, gsp_tracker_slot1_db_set_text,
    gsp_tracker_slot2_db_set_text, gsp_tracker_slot3_db_set_text,
};
static const value_fn k_meter_g[4] = {
    gsp_tracker_m0_g_set_value, gsp_tracker_m1_g_set_value,
    gsp_tracker_m2_g_set_value, gsp_tracker_m3_g_set_value,
};
static const value_fn k_meter_y[4] = {
    gsp_tracker_m0_y_set_value, gsp_tracker_m1_y_set_value,
    gsp_tracker_m2_y_set_value, gsp_tracker_m3_y_set_value,
};
static const value_fn k_meter_r[4] = {
    gsp_tracker_m0_r_set_value, gsp_tracker_m1_r_set_value,
    gsp_tracker_m2_r_set_value, gsp_tracker_m3_r_set_value,
};
static const gsp_component_key_t k_slot_keys[4] = {
    (gsp_component_key_t)GSP_TRACKER_OBJ_KEY_SLOT0,
    (gsp_component_key_t)GSP_TRACKER_OBJ_KEY_SLOT1,
    (gsp_component_key_t)GSP_TRACKER_OBJ_KEY_SLOT2,
    (gsp_component_key_t)GSP_TRACKER_OBJ_KEY_SLOT3,
};

struct app_tracker {
    esp_gsp_handle_t ui;
    tracker_engine_t *engine;
    void *timer;
    esp_timer_handle_t stall_timer;
    uint16_t *grid_frame;
    uint16_t *waves_frame;
    int page;
    int selected_step;
    int selected_pattern;
    int shown_step;
    int shown_pattern;
    int volume;
    int mix_scroll;
    int mix_channels;
    int64_t mix_last_input_us;
    float mix_fling;
    float mix_fling_rest;
    int last_song_key;
    int last_playing;
    volatile unsigned tick_count;
    int clip_hold;
    int slot_channel[4];
    uint8_t fader_value[TRACKER_MIX_STRIPS];
    bool user_mute[TRACKER_MIX_STRIPS];
    bool auto_mute[TRACKER_MIX_STRIPS];
    bool soloed[TRACKER_MIX_STRIPS];
    uint8_t level_hold[TRACKER_MIX_STRIPS];
    uint8_t master_hold;
    button_handle_t ai_button;
};

static void update_labels(app_tracker_t *state, bool force);
static void refresh_mix(app_tracker_t *state);
static void update_transport(app_tracker_t *state);
static void update_master_meter(app_tracker_t *state);

static int normalized_pattern(app_tracker_t *state, int pattern)
{
    const int patterns = tracker_engine_patterns(state->engine);
    if (pattern < 0 || pattern >= patterns) {
        return 0;
    }
    return pattern;
}

static void update_dots(app_tracker_t *state)
{
    gsp_tracker_dot_on_0_set_visible(state->ui, state->page == 0);
    gsp_tracker_dot_on_1_set_visible(state->ui, state->page == 1);
    gsp_tracker_dot_on_2_set_visible(state->ui, state->page == 2);
    gsp_tracker_dot_on_3_set_visible(state->ui, state->page == 3);
    gsp_tracker_dot_on_4_set_visible(state->ui, state->page == 4);
}

static void update_order_list(app_tracker_t *state)
{
    char text[512];
    size_t used = 0;
    const int orders = tracker_engine_orders(state->engine);
    const int patterns = tracker_engine_patterns(state->engine);
    text[0] = '\0';
    for (int offset = -3; offset <= 3; ++offset) {
        const int step = state->selected_step + offset;
        if (step < 0 || step >= orders) {
            continue;
        }
        const int pattern = tracker_engine_pattern_at(state->engine, step);
        const char marker = step == state->selected_step ? '>' : ' ';
        const int written =
            (pattern >= 0 && pattern < patterns)
                ? snprintf(text + used, sizeof(text) - used,
                           "%c%02X  PATTERN %02X\n", marker, step, pattern)
                : snprintf(text + used, sizeof(text) - used,
                           "%c%02X  PATTERN --\n", marker, step);
        if (written <= 0 || used + (size_t)written >= sizeof(text)) {
            break;
        }
        used += (size_t)written;
    }
    gsp_tracker_order_list_set_text(state->ui, text);
    state->shown_step = state->selected_step;
    state->shown_pattern = state->selected_pattern;
}

static void update_labels(app_tracker_t *state, bool force)
{
    tracker_status_t status;
    tracker_engine_get_status(state->engine, &status);
    char buffer[40];

    const float multiplier = tracker_engine_speed_multiplier(state->engine);
    snprintf(buffer, sizeof(buffer), "%.2fx", multiplier);
    gsp_tracker_pb_speed_value_set_text(state->ui, buffer);
    gsp_tracker_speed_05_sel_set_visible(state->ui, multiplier < 0.75f);
    gsp_tracker_speed_10_sel_set_visible(state->ui,
                                         multiplier >= 0.75f && multiplier < 1.25f);
    gsp_tracker_speed_15_sel_set_visible(state->ui,
                                         multiplier >= 1.25f && multiplier < 1.75f);
    gsp_tracker_speed_20_sel_set_visible(state->ui, multiplier >= 1.75f);

    snprintf(buffer, sizeof(buffer), "ORD %02X  PAT %02X  ROW %02X",
             status.order_index, status.pattern, status.row);
    gsp_tracker_pb_pos_set_text(state->ui, buffer);
    gsp_tracker_seq_pos_set_text(state->ui, buffer);

    snprintf(buffer, sizeof(buffer), "%d", state->volume);
    gsp_tracker_master_value_set_text(state->ui, buffer);

    if (force || state->shown_step != state->selected_step ||
        state->shown_pattern != state->selected_pattern) {
        update_order_list(state);
    }
    snprintf(buffer, sizeof(buffer), "%02X / %02X", state->selected_step,
             status.orders > 0 ? status.orders : 0);
    gsp_tracker_step_value_set_text(state->ui, buffer);
    snprintf(buffer, sizeof(buffer), "%02X", state->selected_pattern);
    gsp_tracker_pat_value_set_text(state->ui, buffer);
}

/* Meter zones in dBFS: the bar spans -48..0, green ends at -12 and yellow at
 * -3 (red only near clipping). The scene draws the matching segment widths and
 * the printed ticks, so a full green segment means -12 dBFS. */
#define METER_FLOOR_DB (-48.0f)
#define METER_YELLOW_DB (-12.0f)
#define METER_RED_DB (-3.0f)
#define FADER_UNITY 100
#define FADER_DB_PER_STEP 0.3f
#define FADER_MIN_DB (-60.0f)

static void zone_value(float db, float low, float high, int32_t *out)
{
    float fraction = (db - low) / (high - low);
    if (fraction < 0.0f) {
        fraction = 0.0f;
    } else if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    *out = (int32_t)(fraction * 255.0f);
}

static void meter_levels(int level, int32_t *g, int32_t *y, int32_t *r)
{
    float db = level > 0 ? 20.0f * log10f((float)level / 255.0f)
                         : METER_FLOOR_DB;
    if (db < METER_FLOOR_DB) {
        db = METER_FLOOR_DB;
    }
    zone_value(db, METER_FLOOR_DB, METER_YELLOW_DB, g);
    zone_value(db, METER_YELLOW_DB, METER_RED_DB, y);
    zone_value(db, METER_RED_DB, 0.0f, r);
}

static void update_transport(app_tracker_t *state)
{
    tracker_status_t status;
    tracker_engine_get_status(state->engine, &status);
    const bool playing = status.playing;
    gsp_tracker_pp_play_text_set_text(state->ui, playing ? "" : ">");
    gsp_tracker_pp_pause_text_set_text(state->ui, playing ? "II" : "");
    gsp_tracker_nav_icon_set_text(state->ui, playing ? "[]" : "|<");
}

static void update_master_meter(app_tracker_t *state)
{
    int32_t g = 0;
    int32_t y = 0;
    int32_t r = 0;
    meter_levels(state->master_hold, &g, &y, &r);
    gsp_tracker_lvl_g_set_value(state->ui, g);
    gsp_tracker_lvl_y_set_value(state->ui, y);
    gsp_tracker_lvl_r_set_value(state->ui, r);
    if (tracker_engine_consume_master_clip(state->engine)) {
        state->clip_hold = 15;
    } else if (state->clip_hold > 0) {
        --state->clip_hold;
    }
    gsp_tracker_clip_on_set_visible(state->ui, state->clip_hold > 0);
    gsp_tracker_clip_off_set_visible(state->ui, state->clip_hold == 0);
}

static bool channel_muted(app_tracker_t *state, int channel)
{
    return state->user_mute[channel] || state->auto_mute[channel];
}

/** The list shows TRACKER_MIX_STRIPS rows, but only module channels exist. */
static bool channel_present(const app_tracker_t *state, int channel)
{
    return channel >= 0 && channel < state->mix_channels;
}

static void push_channel_buttons(app_tracker_t *state, int channel)
{
    for (int slot = 0; slot < 4; ++slot) {
        if (state->slot_channel[slot] != channel) {
            continue;
        }
        const bool muted = channel_muted(state, channel);
        (void)k_mute_off[slot](state->ui, !muted);
        (void)k_mute_on[slot](state->ui, muted);
        (void)k_solo_off[slot](state->ui, !state->soloed[channel]);
        (void)k_solo_on[slot](state->ui, state->soloed[channel]);
    }
}

static void apply_channel(app_tracker_t *state, int channel)
{
    tracker_engine_set_channel_mute(state->engine, channel,
                                    channel_muted(state, channel));
    tracker_engine_set_channel_solo(state->engine, channel,
                                    state->soloed[channel]);
}

static int solo_count(app_tracker_t *state)
{
    int count = 0;
    for (int channel = 0; channel < TRACKER_MIX_STRIPS; ++channel) {
        if (state->soloed[channel]) {
            ++count;
        }
    }
    return count;
}

static void toggle_mute(app_tracker_t *state, int channel)
{
    if (!channel_present(state, channel)) {
        return;
    }
    if (solo_count(state) > 0 && !state->soloed[channel]) {
        return;
    }
    state->user_mute[channel] = !state->user_mute[channel];
    apply_channel(state, channel);
    push_channel_buttons(state, channel);
}

static void toggle_solo(app_tracker_t *state, int channel)
{
    if (!channel_present(state, channel)) {
        return;
    }
    if (!state->soloed[channel]) {
        state->soloed[channel] = true;
        state->user_mute[channel] = false;
        state->auto_mute[channel] = false;
        apply_channel(state, channel);
        push_channel_buttons(state, channel);
        for (int other = 0; other < TRACKER_MIX_STRIPS; ++other) {
            if (other == channel || state->soloed[other]) {
                continue;
            }
            state->auto_mute[other] = true;
            apply_channel(state, other);
            push_channel_buttons(state, other);
        }
        return;
    }

    state->soloed[channel] = false;
    apply_channel(state, channel);
    push_channel_buttons(state, channel);
    if (solo_count(state) > 0) {
        state->auto_mute[channel] = true;
        apply_channel(state, channel);
        push_channel_buttons(state, channel);
        return;
    }
    for (int other = 0; other < TRACKER_MIX_STRIPS; ++other) {
        state->auto_mute[other] = false;
        state->user_mute[other] = false;
        apply_channel(state, other);
        push_channel_buttons(state, other);
    }
}

/** Channel fader law: 100 = unity, 120 = +6 dB, and the lower half tapers in
 *  dB so the travel behaves like a console channel fader (50 = -15 dB,
 *  0 = silent) instead of a linear attenuator. */
static float fader_gain(int value)
{
    if (value <= 0) {
        return 0.0f;
    }
    float db = (float)(value - FADER_UNITY) * FADER_DB_PER_STEP;
    if (db < FADER_MIN_DB) {
        db = FADER_MIN_DB;
    }
    return powf(10.0f, db / 20.0f);
}

/** Shows the fader law next to the slider so a change is visible immediately. */
static void push_fader_db(app_tracker_t *state, int slot, int value)
{
    char text[12];
    if (value <= 0) {
        snprintf(text, sizeof(text), "-inf");
    } else {
        const float db = (float)(value - FADER_UNITY) * FADER_DB_PER_STEP;
        if (fabsf(db) < 0.05f) {
            snprintf(text, sizeof(text), "0.0");
        } else {
            snprintf(text, sizeof(text), "%+.1f", db);
        }
    }
    (void)k_fader_db[slot](state->ui, text);
}

static void set_gain(app_tracker_t *state, int channel, int value)
{
    if (value < 0) {
        value = 0;
    } else if (value > GAIN_STEPS) {
        value = GAIN_STEPS;
    }
    state->fader_value[channel] = (uint8_t)value;
    tracker_engine_set_channel_gain(state->engine, channel,
                                    fader_gain(value));
    for (int slot = 0; slot < 4; ++slot) {
        if (state->slot_channel[slot] == channel) {
            push_fader_db(state, slot, value);
        }
    }
}

static void apply_master(app_tracker_t *state, int raw_value)
{
    int value = ((raw_value + 2) / 5) * 5;
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }
    state->volume = value;
    app_audio_set_volume(value);
    update_labels(state, false);
}

static void mix_scroll_delta(app_tracker_t *state, int delta)
{
    int scroll = state->mix_scroll + delta;
    if (scroll < 0) {
        scroll = 0;
    } else if (scroll > MIX_MAX_SCROLL) {
        scroll = MIX_MAX_SCROLL;
    }
    if (scroll != state->mix_scroll) {
        state->mix_scroll = scroll;
        refresh_mix(state);
    }
}

/** Vertical drag deltas inside the MIX viewport (render-task context). The
 *  gesture layer gives a scroll region priority over the page swipe, so a list
 *  drag never turns into a page change. The region is only registered while the
 *  MIX page is visible; elsewhere page swipes keep the whole screen. */
static void mix_scroll_cb(esp_gsp_handle_t gsp, int32_t delta_y, void *context)
{
    (void)gsp;
    app_tracker_t *state = context;
    if (state->page != PAGE_MIX || delta_y == 0) {
        return;
    }
    mix_scroll_delta(state, -(int)delta_y);
    state->mix_last_input_us = esp_timer_get_time();
    state->mix_fling_rest = 0.0f;
    float fling = (float)-delta_y;
    if (fling > MIX_FLING_MAX) {
        fling = MIX_FLING_MAX;
    } else if (fling < -MIX_FLING_MAX) {
        fling = -MIX_FLING_MAX;
    }
    state->mix_fling = fling;
}

/** Registers the list drag region for the MIX page and empties it otherwise. */
static void mix_scroll_region(app_tracker_t *state, bool active)
{
    gsp_rect_t rect = {0, 0, 0, 0};
    if (active) {
        rect = (gsp_rect_t){0, MIX_VIEW_Y, PAGE_W_PX, MIX_VIEW_Y + MIX_VIEW_H};
    }
    (void)esp_gsp_on_scroll(state->ui, rect, mix_scroll_cb, state);
}

static void mix_momentum(app_tracker_t *state)
{
    if (fabsf(state->mix_fling) < 0.5f) {
        state->mix_fling = 0.0f;
        /* At rest the list settles on a row boundary, so no half row is left
         * showing at the top edge of the viewport. */
        const int remainder = state->mix_scroll % MIX_ROW_H;
        if (remainder != 0) {
            int target = state->mix_scroll - remainder;
            if (remainder * 2 >= MIX_ROW_H) {
                target += MIX_ROW_H;
            }
            if (target < 0) {
                target = 0;
            } else if (target > MIX_MAX_SCROLL) {
                target = MIX_MAX_SCROLL;
            }
            int delta = (target - state->mix_scroll) / 2;
            if (delta == 0) {
                delta = (target > state->mix_scroll) ? 1 : -1;
            }
            mix_scroll_delta(state, delta);
        }
        return;
    }
    if (esp_timer_get_time() - state->mix_last_input_us < MIX_RELEASE_US) {
        return;
    }
    state->mix_fling_rest += state->mix_fling;
    const int step = (int)state->mix_fling_rest;
    state->mix_fling_rest -= (float)step;
    if (step != 0) {
        mix_scroll_delta(state, step);
    }
    state->mix_fling *= 0.86f;
}

static void refresh_mix(app_tracker_t *state)
{
    for (int slot = 0; slot < 4; ++slot) {
        const int channel = state->mix_scroll / MIX_ROW_H + slot;
        int32_t g = 0;
        int32_t y = 0;
        int32_t r = 0;
        const bool visible = channel < TRACKER_MIX_STRIPS;
        (void)esp_gsp_component_set_position(
            state->ui, k_slot_keys[slot], 0,
            slot * MIX_ROW_H - state->mix_scroll % MIX_ROW_H);
        (void)esp_gsp_component_set_visible(state->ui, k_slot_keys[slot],
                                            visible);
        if (!visible) {
            continue;
        }
        meter_levels(state->level_hold[channel], &g, &y, &r);
        (void)k_meter_g[slot](state->ui, g);
        (void)k_meter_y[slot](state->ui, y);
        (void)k_meter_r[slot](state->ui, r);
        if (state->slot_channel[slot] != channel) {
            state->slot_channel[slot] = channel;
            char name[16];
            snprintf(name, sizeof(name), "CH %02X", channel);
            switch (slot) {
            case 0: gsp_tracker_slot0_name_set_text(state->ui, name); break;
            case 1: gsp_tracker_slot1_name_set_text(state->ui, name); break;
            case 2: gsp_tracker_slot2_name_set_text(state->ui, name); break;
            default: gsp_tracker_slot3_name_set_text(state->ui, name); break;
            }
            (void)k_fader_set[slot](state->ui, state->fader_value[channel]);
            push_fader_db(state, slot, state->fader_value[channel]);
            push_channel_buttons(state, channel);
        }
    }
}

static void update_meters(app_tracker_t *state)
{
    if (!tracker_engine_is_playing(state->engine)) {
        for (int channel = 0; channel < TRACKER_MIX_STRIPS; ++channel) {
            state->level_hold[channel] = 0;
        }
        state->master_hold = 0;
        update_master_meter(state);
        if (state->page == PAGE_MIX) {
            refresh_mix(state);
        }
        return;
    }
    for (int channel = 0; channel < TRACKER_MIX_STRIPS; ++channel) {
        const int level = tracker_engine_channel_level(state->engine, channel);
        const int hold = state->level_hold[channel] * 7 / 8;
        state->level_hold[channel] = (uint8_t)(level > hold ? level : hold);
    }
    const int master = tracker_engine_master_level(state->engine);
    const int hold = state->master_hold * 7 / 8;
    state->master_hold = (uint8_t)(master > hold ? master : hold);
    update_master_meter(state);
    if (state->page == PAGE_MIX) {
        refresh_mix(state);
    }
}

static void render_pattern_page(app_tracker_t *state)
{
    tracker_render_pattern(state->grid_frame, state->engine);
    esp_gsp_canvas_invalidate(state->ui, GSP_TRACKER_BIND_GRID_CANVAS);
}

static void render_waves_page(app_tracker_t *state)
{
    tracker_render_waves(state->waves_frame, state->engine);
    esp_gsp_canvas_invalidate(state->ui, GSP_TRACKER_BIND_WAVES_CANVAS);
}

static void render_page(app_tracker_t *state, int page)
{
    if (page == PAGE_PATTERN) {
        render_pattern_page(state);
    } else if (page == PAGE_WAVES) {
        render_waves_page(state);
    }
}

/* --- UI stall detector ----------------------------------------------------
 * ui_tick advances on every 50 ms GSP tick, so a frozen render task shows up as
 * a counter that stops moving. The esp_timer task is independent: when the UI
 * stops we log a task/heap snapshot and then panic, so the retained pre-crash
 * log ring and a core dump survive the reboot instead of leaving the device
 * stuck until it is power cycled. */
static void stall_log(app_tracker_t *state)
{
    multi_heap_info_t internal_info;
    multi_heap_info_t spiram_info;
    heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&spiram_info, MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "UI STALL ticks=%u I=%u/%u S=%u/%u largest=%u",
             (unsigned)state->tick_count,
             (unsigned)internal_info.total_free_bytes,
             (unsigned)internal_info.minimum_free_bytes,
             (unsigned)spiram_info.total_free_bytes,
             (unsigned)spiram_info.minimum_free_bytes,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    static TaskStatus_t tasks[24];
    const UBaseType_t count = uxTaskGetSystemState(tasks, 24, NULL);
    for (UBaseType_t index = 0; index < count; ++index) {
        ESP_LOGE(TAG, "UI STALL task=%s state=%u stack_free=%u",
                 tasks[index].pcTaskName, (unsigned)tasks[index].eCurrentState,
                 (unsigned)tasks[index].usStackHighWaterMark);
    }
}

static void stall_check(void *arg)
{
    app_tracker_t *state = arg;
    static unsigned last_tick;
    static unsigned stalled;
    const unsigned tick = state->tick_count;
    if (tick != last_tick) {
        last_tick = tick;
        stalled = 0;
        return;
    }
    ++stalled;
    if (stalled == 5) {
        stall_log(state);
    } else if (stalled == 7) {
        esp_system_abort("UI task stalled");
    }
}

static void ui_tick(esp_gsp_handle_t ui, void *context)
{
    (void)ui;
    app_tracker_t *state = context;
    ++state->tick_count;
    /* Temporary memory watchdog: a 5 s heartbeat with free/min heap on both
     * pools makes a leak or an exhausted allocator visible before a hang. */
    if (state->tick_count % 100 == 0) {
        multi_heap_info_t internal_info;
        multi_heap_info_t spiram_info;
        heap_caps_get_info(&internal_info, MALLOC_CAP_INTERNAL);
        heap_caps_get_info(&spiram_info, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "hb t=%u play=%d I=%u/%u S=%u/%u",
                 (unsigned)state->tick_count,
                 (int)tracker_engine_is_playing(state->engine),
                 (unsigned)internal_info.total_free_bytes,
                 (unsigned)internal_info.minimum_free_bytes,
                 (unsigned)spiram_info.total_free_bytes,
                 (unsigned)spiram_info.minimum_free_bytes);
    }
    mix_momentum(state);
    if (state->tick_count % METER_REFRESH_TICKS == 0) {
        update_meters(state);
    }
    if (state->tick_count % LABEL_REFRESH_TICKS == 0) {
        update_labels(state, false);
        update_transport(state);
    }
    const bool playing = tracker_engine_is_playing(state->engine);
    if (playing != state->last_playing) {
        state->last_playing = playing;
        if (state->page == PAGE_WAVES) {
            render_waves_page(state);
        }
    }
    tracker_status_t status;
    tracker_engine_get_status(state->engine, &status);
    const int song_key = (status.order_index << 16) | (status.pattern << 8) |
                         status.row;
    if (playing && state->page == PAGE_PATTERN &&
        state->tick_count % PATTERN_REFRESH_TICKS == 0 &&
        song_key != state->last_song_key) {
        state->last_song_key = song_key;
        render_pattern_page(state);
    } else if (playing && state->page == PAGE_WAVES) {
        render_waves_page(state);
    }
}

static void copy_region(const esp_gsp_canvas_surface_t *surface,
                        const uint16_t *source)
{
    if (surface->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565) {
        return;
    }
    for (uint16_t row = 0; row < surface->height; ++row) {
        uint8_t *destination = (uint8_t *)surface->pixels +
                               (size_t)row * surface->stride_bytes;
        const uint16_t *line = source +
                               (size_t)(surface->y + row) * TRACKER_FRAME_W +
                               surface->x;
        memcpy(destination, line, (size_t)surface->width * sizeof(uint16_t));
    }
}

static void grid_canvas_draw(const esp_gsp_canvas_surface_t *surface, void *context)
{
    app_tracker_t *state = context;
    if (state->grid_frame != NULL) {
        copy_region(surface, state->grid_frame);
    }
}

static void waves_canvas_draw(const esp_gsp_canvas_surface_t *surface, void *context)
{
    app_tracker_t *state = context;
    if (state->waves_frame != NULL) {
        copy_region(surface, state->waves_frame);
    }
}

static void on_component_event(esp_gsp_handle_t ui,
                               const esp_gsp_component_event_t *event, void *context)
{
    (void)ui;
    app_tracker_t *state = context;
    if (event->key != GSP_TRACKER_OBJ_KEY_PAGES) {
        return;
    }
    const int page = (int)event->state.value;
    if ((event->flags & ESP_GSP_COMPONENT_EVENT_VALUE_CHANGED) == 0 ||
        event->state.dragging || page < 0 || page > PAGE_ORDER ||
        page == state->page) {
        return;
    }
    state->page = page;
    mix_scroll_region(state, page == PAGE_MIX);
    state->last_song_key = -1;
    update_dots(state);
    render_page(state, page);
    update_labels(state, true);
    update_transport(state);
    if (page == PAGE_MIX) {
        refresh_mix(state);
    }
}

static void ai_short_press(void *handle, void *data)
{
    (void)handle;
    app_tracker_t *state = data;
    if (tracker_engine_is_playing(state->engine)) {
        tracker_engine_pause(state->engine);
    } else {
        tracker_engine_play(state->engine);
    }
    update_labels(state, true);
    update_transport(state);
}

static void ai_long_press(void *handle, void *data)
{
    (void)handle;
    app_tracker_t *state = data;
    tracker_engine_pause(state->engine);
    tracker_engine_restart(state->engine);
    update_labels(state, true);
    update_transport(state);
}

static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *context)
{
    (void)ui;
    app_tracker_t *state = context;

    if (gsp_tracker_event_is_pp_cb(event)) {
        if (tracker_engine_is_playing(state->engine)) {
            tracker_engine_pause(state->engine);
        } else {
            tracker_engine_play(state->engine);
        }
        update_labels(state, true);
        update_transport(state);
        return;
    }
    if (gsp_tracker_event_is_nav_cb(event)) {
        tracker_engine_pause(state->engine);
        tracker_engine_restart(state->engine);
        update_labels(state, true);
        update_transport(state);
        return;
    }
    if (gsp_tracker_event_is_master_fader_cb(event)) {
        apply_master(state, (int)event->arg);
        return;
    }

    if (gsp_tracker_event_is_slot_fader0_cb(event)) {
        const int ch = state->slot_channel[0];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            set_gain(state, ch, (int)event->arg);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_fader1_cb(event)) {
        const int ch = state->slot_channel[1];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            set_gain(state, ch, (int)event->arg);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_fader2_cb(event)) {
        const int ch = state->slot_channel[2];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            set_gain(state, ch, (int)event->arg);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_fader3_cb(event)) {
        const int ch = state->slot_channel[3];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            set_gain(state, ch, (int)event->arg);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_mute0_cb(event)) {
        const int ch = state->slot_channel[0];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_mute(state, ch);
        }
        return;
    }
    if (gsp_tracker_event_is_slot_solo0_cb(event)) {
        const int ch = state->slot_channel[0];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_solo(state, ch);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_mute1_cb(event)) {
        const int ch = state->slot_channel[1];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_mute(state, ch);
        }
        return;
    }
    if (gsp_tracker_event_is_slot_solo1_cb(event)) {
        const int ch = state->slot_channel[1];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_solo(state, ch);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_mute2_cb(event)) {
        const int ch = state->slot_channel[2];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_mute(state, ch);
        }
        return;
    }
    if (gsp_tracker_event_is_slot_solo2_cb(event)) {
        const int ch = state->slot_channel[2];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_solo(state, ch);
        }
        return;
    }

    if (gsp_tracker_event_is_slot_mute3_cb(event)) {
        const int ch = state->slot_channel[3];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_mute(state, ch);
        }
        return;
    }
    if (gsp_tracker_event_is_slot_solo3_cb(event)) {
        const int ch = state->slot_channel[3];
        if (ch >= 0 && ch < TRACKER_MIX_STRIPS) {
            toggle_solo(state, ch);
        }
        return;
    }

    if (gsp_tracker_event_is_speed_05_cb(event)) {
        tracker_engine_set_speed_multiplier(state->engine, 0.5f);
        update_labels(state, false);
        return;
    }
    if (gsp_tracker_event_is_speed_10_cb(event)) {
        tracker_engine_set_speed_multiplier(state->engine, 1.0f);
        update_labels(state, false);
        return;
    }
    if (gsp_tracker_event_is_speed_15_cb(event)) {
        tracker_engine_set_speed_multiplier(state->engine, 1.5f);
        update_labels(state, false);
        return;
    }
    if (gsp_tracker_event_is_speed_20_cb(event)) {
        tracker_engine_set_speed_multiplier(state->engine, 2.0f);
        update_labels(state, false);
        return;
    }
    if (gsp_tracker_event_is_order_play_cb(event)) {
        tracker_engine_play(state->engine);
        update_labels(state, true);
        update_transport(state);
        return;
    }
    if (gsp_tracker_event_is_order_pause_cb(event)) {
        tracker_engine_pause(state->engine);
        update_labels(state, true);
        update_transport(state);
        return;
    }
    if (gsp_tracker_event_is_order_restart_cb(event)) {
        tracker_engine_restart(state->engine);
        update_labels(state, true);
        update_transport(state);
        return;
    }
    if (gsp_tracker_event_is_step_prev_cb(event) ||
        gsp_tracker_event_is_step_next_cb(event)) {
        const int orders = tracker_engine_orders(state->engine);
        if (orders > 0) {
            const int delta = gsp_tracker_event_is_step_next_cb(event) ? 1 : -1;
            state->selected_step =
                (state->selected_step + delta + orders) % orders;
            state->selected_pattern = normalized_pattern(
                state, tracker_engine_pattern_at(state->engine,
                                                 state->selected_step));
            update_labels(state, true);
        }
        return;
    }
    if (gsp_tracker_event_is_pat_prev_cb(event) ||
        gsp_tracker_event_is_pat_next_cb(event)) {
        const int patterns = tracker_engine_patterns(state->engine);
        if (patterns > 0) {
            const int delta = gsp_tracker_event_is_pat_next_cb(event) ? 1 : -1;
            state->selected_pattern =
                (normalized_pattern(state, state->selected_pattern) + delta +
                 patterns) %
                patterns;
            update_labels(state, true);
        }
        return;
    }
    if (gsp_tracker_event_is_apply_pattern_cb(event)) {
        tracker_engine_set_pattern_at(state->engine, state->selected_step,
                                      state->selected_pattern);
        tracker_engine_restart(state->engine);
        update_labels(state, true);
        return;
    }
}

esp_err_t app_tracker_start(esp_gsp_handle_t ui, tracker_engine_t *engine,
                            app_tracker_t **out_handle)
{
    ESP_RETURN_ON_FALSE(ui != NULL && engine != NULL && out_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *out_handle = NULL;

    app_tracker_t *state = calloc(1, sizeof(*state));
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_NO_MEM, TAG, "state alloc failed");
    state->ui = ui;
    state->engine = engine;
    state->page = PAGE_PATTERN;
    state->volume = 100;
    state->last_song_key = -1;
    for (int slot = 0; slot < 4; ++slot) {
        state->slot_channel[slot] = -1;
    }
    tracker_engine_set_loop(engine, false);
    for (int channel = 0; channel < TRACKER_MIX_STRIPS; ++channel) {
        state->fader_value[channel] = 100;
        tracker_engine_set_channel_gain(engine, channel, 1.0f);
        tracker_engine_set_channel_mute(engine, channel, false);
        tracker_engine_set_channel_solo(engine, channel, false);
    }
    state->grid_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    state->waves_frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (state->grid_frame == NULL || state->waves_frame == NULL) {
        ESP_LOGE(TAG, "frame buffers unavailable");
        goto fail;
    }

    tracker_status_t status;
    tracker_engine_get_status(engine, &status);
    state->mix_channels = status.channels;
    if (state->mix_channels > TRACKER_MIX_STRIPS) {
        state->mix_channels = TRACKER_MIX_STRIPS;
    }
    state->selected_step = status.order_index;
    state->selected_pattern = normalized_pattern(
        state, tracker_engine_pattern_at(engine, status.order_index));

    esp_gsp_err_t error = esp_gsp_on_event(ui, on_event, state);
    if (error == ESP_GSP_OK) {
        error = esp_gsp_on_component_event(ui, on_component_event, state);
    }
    if (error == ESP_GSP_OK) {
        error = esp_gsp_canvas_set_draw_cb(ui, GSP_TRACKER_BIND_GRID_CANVAS,
                                           grid_canvas_draw, state);
    }
    if (error == ESP_GSP_OK) {
        error = esp_gsp_canvas_set_draw_cb(ui, GSP_TRACKER_BIND_WAVES_CANVAS,
                                           waves_canvas_draw, state);
    }
    if (error != ESP_GSP_OK) {
        ESP_LOGE(TAG, "GSP binding failed: 0x%x", error);
        goto fail;
    }

    /* The list drag region only exists on the MIX page. */
    mix_scroll_region(state, false);

    tracker_render_pattern(state->grid_frame, engine);
    tracker_render_waves(state->waves_frame, engine);
    esp_gsp_page_flow_set_page(ui, GSP_TRACKER_OBJ_KEY_PAGES, PAGE_PATTERN, false);
    update_dots(state);
    update_labels(state, true);
    update_transport(state);
    gsp_tracker_master_fader_set_value(ui, state->volume);
    app_audio_set_volume(state->volume);
    update_master_meter(state);
    state->mix_scroll = 0;
    for (int slot = 0; slot < 4; ++slot) {
        state->slot_channel[slot] = -1;
    }
    refresh_mix(state);

    state->timer = esp_gsp_timer_create(ui, UI_TICK_MS, ui_tick, state);
    if (state->timer == NULL) {
        ESP_LOGE(TAG, "UI timer unavailable");
        goto fail;
    }

    /* Independent watchdog for the render/UI task, which the task watchdog
     * cannot see when the task spins or deadlocks with IDLE still running. */
    const esp_timer_create_args_t stall_args = {
        .callback = stall_check,
        .arg = state,
        .name = "ui_stall",
    };
    if (esp_timer_create(&stall_args, &state->stall_timer) != ESP_OK) {
        ESP_LOGW(TAG, "UI stall timer unavailable");
    } else if (esp_timer_start_periodic(state->stall_timer, 1000000) != ESP_OK) {
        ESP_LOGW(TAG, "UI stall timer could not start");
    }

    button_handle_t buttons[BSP_BUTTON_NUM] = {0};
    int button_count = 0;
    if (bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM) == ESP_OK &&
        button_count > BSP_BUTTON_AI) {
        state->ai_button = buttons[BSP_BUTTON_AI];
        iot_button_register_cb(state->ai_button, BUTTON_SINGLE_CLICK, NULL,
                               ai_short_press, state);
        iot_button_register_cb(state->ai_button, BUTTON_LONG_PRESS_START, NULL,
                               ai_long_press, state);
        ESP_LOGI(TAG, "AI button bound: short=play/pause, long=rewind");
    }

    *out_handle = state;
    return ESP_OK;

fail:
    mix_scroll_region(state, false);
    (void)esp_gsp_on_event(ui, NULL, NULL);
    (void)esp_gsp_on_component_event(ui, NULL, NULL);
    (void)esp_gsp_canvas_set_draw_cb(ui, GSP_TRACKER_BIND_GRID_CANVAS, NULL,
                                     NULL);
    (void)esp_gsp_canvas_set_draw_cb(ui, GSP_TRACKER_BIND_WAVES_CANVAS, NULL,
                                     NULL);
    if (state->timer != NULL) {
        esp_gsp_timer_delete(ui, state->timer);
    }
    if (state->stall_timer != NULL) {
        (void)esp_timer_stop(state->stall_timer);
        (void)esp_timer_delete(state->stall_timer);
    }
    free(state->grid_frame);
    free(state->waves_frame);
    free(state);
    return ESP_ERR_NO_MEM;
}

void app_tracker_set_page(app_tracker_t *handle, int page)
{
    if (handle == NULL || page < 0 || page > PAGE_ORDER) {
        return;
    }
    handle->page = page;
    mix_scroll_region(handle, page == PAGE_MIX);
    handle->last_song_key = -1;
    (void)esp_gsp_page_flow_set_page(handle->ui, GSP_TRACKER_OBJ_KEY_PAGES,
                                     (uint32_t)page, true);
    update_dots(handle);
    render_page(handle, page);
    update_labels(handle, true);
    update_transport(handle);
    if (page == PAGE_MIX) {
        refresh_mix(handle);
    }
}

void app_tracker_stop(app_tracker_t **handle)
{
    if (handle == NULL || *handle == NULL) {
        return;
    }
    app_tracker_t *state = *handle;
    if (state->stall_timer != NULL) {
        (void)esp_timer_stop(state->stall_timer);
        (void)esp_timer_delete(state->stall_timer);
        state->stall_timer = NULL;
    }
    if (state->timer != NULL) {
        esp_gsp_timer_delete(state->ui, state->timer);
        state->timer = NULL;
    }
    mix_scroll_region(state, false);
    (void)esp_gsp_on_event(state->ui, NULL, NULL);
    (void)esp_gsp_on_component_event(state->ui, NULL, NULL);
    (void)esp_gsp_canvas_set_draw_cb(state->ui, GSP_TRACKER_BIND_GRID_CANVAS,
                                     NULL, NULL);
    (void)esp_gsp_canvas_set_draw_cb(state->ui, GSP_TRACKER_BIND_WAVES_CANVAS,
                                     NULL, NULL);
    free(state->grid_frame);
    free(state->waves_frame);
    free(state);
    *handle = NULL;
}
