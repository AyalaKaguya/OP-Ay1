// SPDX-License-Identifier: Apache-2.0
#include "tracker_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_render.h"

#define TAP_SLOP 12
#define DRAG_SLOP 8
#define PAGE_SWIPE_DISTANCE 64
#define MIX_FADER_HIT_Y TRACKER_MIX_FADER_HIT_Y
#define MIX_MAX_SCROLL ((TRACKER_MAX_CHANNELS - 4) * TRACKER_MIX_STRIP_H)

static int clamp_int(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static bool in_rect(int x, int y, int left, int top, int width, int height)
{
    return x >= left && x < left + width && y >= top && y < top + height;
}

static float gain_for_step(int step)
{
    return step == 0 ? 0.0f : powf(10.0f, (step - 100) * 0.3f / 20.0f);
}

static void set_gain(tracker_model_t *m, int channel, int x)
{
    if (channel < 0 || channel >= m->view.channel_count) {
        return;
    }
    const int step = clamp_int((x - TRACKER_MIX_FADER_X) * 120 /
                               (TRACKER_MIX_FADER_W - 1), 0, 120);
    if (m->view.gain_steps[channel] != (uint8_t)step) {
        ++m->edit_revision;
    }
    m->view.gain_steps[channel] = (uint8_t)step;
    tracker_engine_set_channel_gain(m->engine, channel, gain_for_step(step));
    m->dirty = true;
}

static void set_master(tracker_model_t *m, int x)
{
    const int raw = clamp_int((x - TRACKER_MIX_MASTER_X) * 100 /
                              (TRACKER_MIX_MASTER_W - 1), 0, 100);
    const int volume = ((raw + 2) / 5) * 5;
    if (m->view.volume != volume) {
        ++m->edit_revision;
    }
    m->view.volume = volume;
    m->dirty = true;
}

static int solo_count(const tracker_model_t *m)
{
    int result = 0;
    for (int ch = 0; ch < m->view.channel_count; ++ch) {
        result += m->view.soloed[ch] ? 1 : 0;
    }
    return result;
}

static void apply_channel(tracker_model_t *m, int ch)
{
    m->view.muted[ch] = m->user_mute[ch] || m->auto_mute[ch];
    tracker_engine_set_channel_mute(m->engine, ch, m->view.muted[ch]);
    tracker_engine_set_channel_solo(m->engine, ch, m->view.soloed[ch]);
}

static void toggle_mute(tracker_model_t *m, int ch)
{
    if (ch < 0 || ch >= m->view.channel_count) {
        return;
    }
    if (solo_count(m) > 0 && !m->view.soloed[ch]) {
        return;
    }
    m->user_mute[ch] = !m->user_mute[ch];
    ++m->edit_revision;
    apply_channel(m, ch);
    m->dirty = true;
}

static void toggle_solo(tracker_model_t *m, int ch)
{
    if (ch < 0 || ch >= m->view.channel_count) {
        return;
    }
    m->view.soloed[ch] = !m->view.soloed[ch];
    ++m->edit_revision;
    if (m->view.soloed[ch]) {
        m->user_mute[ch] = false;
    }
    const int solos = solo_count(m);
    for (int index = 0; index < m->view.channel_count; ++index) {
        if (solos == 0) {
            m->user_mute[index] = false;
            m->auto_mute[index] = false;
        } else {
            m->auto_mute[index] = !m->view.soloed[index];
        }
        apply_channel(m, index);
    }
    m->dirty = true;
}

static void change_note(tracker_model_t *m, int note)
{
    if (note == m->synth_view.note) {
        return;
    }
    if (m->synth_view.note >= 0) {
        if (m->router) {
            app_note_router_touch_note_off(m->router, m->synth_view.note);
        } else {
            (void)app_synth_note_off(m->synth, m->synth_view.note);
        }
    }
    m->synth_view.note = -1;
    const bool accepted = note >= 0 && (m->router ?
                                        app_note_router_touch_note_on(m->router, note, 0.8f) : app_synth_note_on(m->synth, note, 0.8f));
    if (accepted) {
        m->synth_view.note = note;
    } else if (note >= 0) {
        m->synth_view.command_failed = true;
    }
    m->dirty = true;
}

void tracker_model_cancel_input(tracker_model_t *m)
{
    change_note(m, -1);
    if (m->router) {
        app_note_router_release_touch(m->router);
    } else if (m->synth != NULL) {
        app_synth_all_notes_off(m->synth);
    }
    m->pointer_down = false;
    m->input = TRACKER_INPUT_NONE;
    m->fling = 0.0f;
}

void tracker_model_set_page(tracker_model_t *m, int page)
{
    if (page < 0 || page >= TRACKER_PAGE_COUNT || page == m->view.page) {
        return;
    }
    tracker_model_cancel_input(m);
    m->view.page = page;
    m->dirty = true;
}

void tracker_model_transport(tracker_model_t *m, bool restart, bool song_mode)
{
    if (restart) {
        tracker_engine_pause(m->engine);
        tracker_engine_restart(m->engine);
    } else if (tracker_engine_is_playing(m->engine)) {
        tracker_engine_pause(m->engine);
    } else {
        if (song_mode) {
            tracker_engine_stop_pattern_launch(m->engine);
        }
        tracker_engine_play(m->engine);
    }
    m->dirty = true;
}

static void launch_pad(tracker_model_t *m, int pad)
{
    if (pad < 0 || pad >= tracker_engine_patterns(m->engine)) {
        return;
    }
    const int launched = tracker_engine_launched_pattern(m->engine);
    if (launched == pad && tracker_engine_is_playing(m->engine)) {
        tracker_engine_queue_pattern_stop(m->engine);
    } else if (launched == pad) {
        tracker_engine_play(m->engine);
    } else {
        tracker_engine_launch_pattern(m->engine, pad);
        if (!tracker_engine_is_playing(m->engine)) {
            tracker_engine_play(m->engine);
        }
    }
    m->dirty = true;
}

static void submit_patch(tracker_model_t *m)
{
    ++m->edit_revision;
    m->synth_view.command_failed =
        !app_synth_set_patch(m->synth, &m->synth_view.patch);
    m->patch_pending = m->synth_view.command_failed;
    m->pending_preset = -1;
    m->dirty = true;
}

static void synth_tap(tracker_model_t *m, int x, int y)
{
    tracker_synth_view_t *v = &m->synth_view;
    if (in_rect(x, y, 30, 52, 420, 44)) {
        const int delta = x < 240 ? -1 : 1;
        v->preset = (v->preset + delta + APP_SYNTH_PATCH_COUNT) % APP_SYNTH_PATCH_COUNT;
        ++m->edit_revision;
        v->patch = app_synth_patch_for_preset((app_synth_patch_id_t)v->preset);
        v->command_failed = !app_synth_set_patch_id(m->synth, (app_synth_patch_id_t)v->preset);
        m->patch_pending = v->command_failed;
        m->pending_preset = v->preset;
    } else if (in_rect(x, y, 30, 104, 420, 44)) {
        v->tab = clamp_int((x - 30) / 107, 0, 3);
    } else if (in_rect(x, y, 30, 271, 52, 49)) {
        v->octave = clamp_int(v->octave - 1, 0, 8);
    } else if (in_rect(x, y, 124, 271, 52, 49)) {
        v->octave = clamp_int(v->octave + 1, 0, 8);
    } else if (in_rect(x, y, 348, 271, 102, 49)) {
        if (m->router) {
            app_note_router_panic(m->router);
        } else {
            app_synth_panic(m->synth);
        }
        v->note = -1;
        v->command_failed = false;
    } else if (v->tab == 0 && in_rect(x, y, 30, 154, 186, 109)) {
        v->patch.waveform = (app_synth_waveform_t)((v->patch.waveform + 1) % 5);
        submit_patch(m);
    } else if (v->tab == 3 && in_rect(x, y, 30, 235, 186, 28)) {
        v->patch.lfo_target = (app_synth_lfo_target_t)((v->patch.lfo_target + 1) % 3);
        v->patch.lfo_depth = 0.0f;
        submit_patch(m);
    }
    m->dirty = true;
}

static void tap(tracker_model_t *m, int x, int y)
{
    if (y >= 432) {
        return;
    }
    if (m->view.page == TRACKER_PAGE_PLAYBACK) {
        if (in_rect(x, y, 16, 218, 218, 77)) {
            tracker_model_transport(m, true, true);
        } else if (in_rect(x, y, 246, 218, 218, 77)) {
            tracker_model_transport(m, false, true);
        } else if (in_rect(x, y, 16, 338, 448, 48)) {
            const int slot = clamp_int((x - 16) / 114, 0, 3);
            const float speed = (slot + 1) * 0.5f;
            if (m->view.speed_multiplier != speed) {
                ++m->edit_revision;
            }
            tracker_engine_set_speed_multiplier(m->engine, speed);
            m->view.speed_multiplier = speed;
        }
    } else if (m->view.page == TRACKER_PAGE_PADS) {
        if (in_rect(x, y, 16, 115, 448, 292)) {
            const int column = (x - 16) / 114;
            const int row = (y - 115) / 75;
            if ((x - 16) % 114 < 106 && (y - 115) % 75 < 67) {
                launch_pad(m, row * 4 + column);
            }
        }
    } else if (m->view.page == TRACKER_PAGE_SYNTH) {
        synth_tap(m, x, y);
    } else if (m->view.page == TRACKER_PAGE_MIXER &&
               in_rect(x, y, 16, TRACKER_MIX_HEADER_H, 434, TRACKER_MIX_VIEW_H)) {
        const int relative = y - TRACKER_MIX_HEADER_H + m->view.mix_scroll;
        const int ch = relative / TRACKER_MIX_STRIP_H;
        const int row_y = relative % TRACKER_MIX_STRIP_H;
        if (ch >= m->view.channel_count) {
            return;
        }
        if (in_rect(x, row_y, 334, 9, 44, 46)) {
            toggle_mute(m, ch);
        } else if (in_rect(x, row_y, 392, 9, 44, 46)) {
            toggle_solo(m, ch);
        } else if (row_y >= MIX_FADER_HIT_Y && x >= 86 && x < 318) {
            set_gain(m, ch, x);
        }
    }
    m->dirty = true;
}

static void begin_pointer(tracker_model_t *m, int x, int y, uint64_t now_ms)
{
    m->pointer_down = true;
    m->moved = false;
    m->start_x = x;
    m->start_y = y;
    m->last_y = y;
    m->input_time = now_ms;
    m->last_pointer_time = now_ms;
    m->scroll_start = m->view.mix_scroll;
    m->fling = 0.0f;
    m->input = TRACKER_INPUT_NAV;
    if (m->view.page == TRACKER_PAGE_SYNTH) {
        if (in_rect(x, y, 30, 327, 420, 105)) {
            m->input = TRACKER_INPUT_KEYBOARD;
            change_note(m, tracker_synth_note_at(x, y, m->synth_view.octave));
        } else {
            m->drag_parameter = tracker_synth_parameter_at(m->synth_view.tab, x, y);
            if (m->drag_parameter >= 0) {
                m->input = TRACKER_INPUT_SYNTH_PARAM;
                tracker_synth_set_parameter(&m->synth_view, m->drag_parameter, x);
                submit_patch(m);
            }
        }
    } else if (m->view.page == TRACKER_PAGE_MIXER) {
        if (in_rect(x, y, 86, 69, 318, 38)) {
            m->input = TRACKER_INPUT_MASTER;
            set_master(m, x);
        } else if (in_rect(x, y, 16, TRACKER_MIX_HEADER_H, 448, TRACKER_MIX_VIEW_H)) {
            m->input = TRACKER_INPUT_MIX_PENDING;
            m->drag_channel = (y - TRACKER_MIX_HEADER_H + m->view.mix_scroll) /
                              TRACKER_MIX_STRIP_H;
        }
    }
}

void tracker_model_pointer(tracker_model_t *m, int x, int y,
                           bool pressed, uint64_t now_ms)
{
    if (pressed && !m->pointer_down) {
        begin_pointer(m, x, y, now_ms);
        return;
    }
    if (!m->pointer_down) {
        return;
    }
    const int dx = x - m->start_x;
    const int dy = y - m->start_y;
    if (abs(dx) > TAP_SLOP || abs(dy) > TAP_SLOP) {
        m->moved = true;
    }
    if (m->input == TRACKER_INPUT_MIX_PENDING) {
        const int local_y = (m->start_y - TRACKER_MIX_HEADER_H +
                             m->scroll_start) % TRACKER_MIX_STRIP_H;
        if (abs(dy) > DRAG_SLOP && abs(dy) > abs(dx)) {
            m->input = TRACKER_INPUT_MIX_SCROLL;
        } else if (abs(dx) > DRAG_SLOP && abs(dx) > abs(dy) &&
                   m->start_x >= 86 && m->start_x < 318 &&
                   local_y >= MIX_FADER_HIT_Y) {
            m->input = TRACKER_INPUT_GAIN;
        }
    }
    if (m->input == TRACKER_INPUT_KEYBOARD) {
        change_note(m, pressed ? tracker_synth_note_at(x, y, m->synth_view.octave) : -1);
    } else if (m->input == TRACKER_INPUT_MASTER) {
        set_master(m, x);
    } else if (m->input == TRACKER_INPUT_GAIN) {
        set_gain(m, m->drag_channel, x);
    } else if (m->input == TRACKER_INPUT_MIX_SCROLL) {
        m->view.mix_scroll = clamp_int(m->scroll_start - dy, 0, MIX_MAX_SCROLL);
        const uint64_t elapsed = now_ms - m->last_pointer_time;
        if (elapsed > 0 && elapsed < 200) {
            m->fling = (m->last_y - y) * 30.0f / (float)elapsed;
            m->fling = fmaxf(-60.0f, fminf(m->fling, 60.0f));
        }
        m->dirty = true;
    } else if (m->input == TRACKER_INPUT_SYNTH_PARAM) {
        tracker_synth_set_parameter(&m->synth_view, m->drag_parameter, x);
        submit_patch(m);
    }
    m->last_y = y;
    m->last_pointer_time = now_ms;
    if (!pressed) {
        const tracker_input_kind_t kind = m->input;
        m->pointer_down = false;
        m->input = TRACKER_INPUT_NONE;
        if (kind == TRACKER_INPUT_MIX_SCROLL && fabsf(m->fling) <= 0.5f) {
            m->view.mix_scroll = clamp_int(
                                     ((m->view.mix_scroll + TRACKER_MIX_STRIP_H / 2) /
                                      TRACKER_MIX_STRIP_H) * TRACKER_MIX_STRIP_H, 0, MIX_MAX_SCROLL);
        }
        if ((kind == TRACKER_INPUT_NAV || kind == TRACKER_INPUT_MIX_PENDING) &&
                abs(dx) >= PAGE_SWIPE_DISTANCE && abs(dx) > abs(dy) * 2) {
            tracker_model_set_page(m, m->view.page + (dx < 0 ? 1 : -1));
        } else if (!m->moved && (kind == TRACKER_INPUT_NAV ||
                                 kind == TRACKER_INPUT_MIX_PENDING)) {
            tap(m, x, y);
        }
    }
}

void tracker_model_init(tracker_model_t *m, tracker_engine_t *engine,
                        app_synth_t *synth, app_audio_bus_t *bus)
{
    memset(m, 0, sizeof(*m));
    m->engine = engine;
    m->synth = synth;
    m->bus = bus;
    m->view.page = TRACKER_PAGE_PATTERN;
    m->view.volume = 100;
    m->view.speed_multiplier = 1.0f;
    m->view.pad_playing = -1;
    m->view.pad_next = -1;
    m->pending_preset = -1;
    m->synth_view.octave = 4;
    m->synth_view.note = -1;
    m->synth_view.patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
    (void)app_synth_get_patch(synth, &m->synth_view.patch);
    tracker_engine_get_status(engine, &m->view.status);
    m->view.channel_count = m->view.status.channels;
    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        m->view.gain_steps[ch] = 100;
    }
    /* Source ownership belongs to the active project; UI creation never resets
     * a loaded song's loop, channel controls or patch. */
    tracker_model_sync_sources(m);
    m->dirty = true;
}

void tracker_model_sync_sources(tracker_model_t *m)
{
    /* A fresh UI has no pointer ownership to release. In particular do not
     * send a global synth all-off before create() attaches its note router. */
    if (m->pointer_down || m->synth_view.note >= 0 || m->router) {
        tracker_model_cancel_input(m);
    }
    m->patch_pending = false;
    m->pending_preset = -1;
    m->synth_view.command_failed = false;
    (void)app_synth_get_patch(m->synth, &m->synth_view.patch);
    if (app_synth_get_status(m->synth, &m->synth_view.status)) {
        const int preset = (int)m->synth_view.status.patch_id;
        m->synth_view.preset = preset < APP_SYNTH_PATCH_COUNT ? preset : -1;
    }
    tracker_engine_get_status(m->engine, &m->view.status);
    m->view.channel_count = m->view.status.channels;
    m->view.speed_multiplier = tracker_engine_speed_multiplier(m->engine);
    m->view.mix_scroll = 0;
    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        const float gain = tracker_engine_channel_gain(m->engine, ch);
        const int step = gain > 0.0f ? (int)lroundf(100.0f + 20.0f * log10f(gain) / 0.3f) : 0;
        m->view.gain_steps[ch] = (uint8_t)clamp_int(step, 0, 120);
        m->view.soloed[ch] = tracker_engine_channel_solo(m->engine, ch);
        m->view.muted[ch] = tracker_engine_channel_mute(m->engine, ch);
        m->user_mute[ch] = m->view.muted[ch];
        m->auto_mute[ch] = false;
        m->view.levels[ch] = 0;
    }
    if (solo_count(m) > 0) {
        for (int ch = 0; ch < m->view.channel_count; ++ch) {
            m->auto_mute[ch] = !m->view.soloed[ch];
            if (m->auto_mute[ch]) {
                m->user_mute[ch] = false;
            }
        }
    }
    m->dirty = true;
}

static int next_pattern(tracker_model_t *m)
{
    for (int order = m->view.status.order_index + 1;
            order < tracker_engine_orders(m->engine); ++order) {
        const int pattern = tracker_engine_pattern_at(m->engine, order);
        if (pattern >= 0 && pattern < tracker_engine_patterns(m->engine)) {
            return pattern;
        }
    }
    return -1;
}

void tracker_model_tick(tracker_model_t *m, uint64_t now_ms)
{
    m->now_ms = now_ms;
    if (m->patch_pending && m->synth) {
        const bool accepted = m->pending_preset >= 0 ?
                              app_synth_set_patch_id(m->synth, (app_synth_patch_id_t)m->pending_preset) :
                              app_synth_set_patch(m->synth, &m->synth_view.patch);
        if (accepted) {
            m->patch_pending = false;
            m->synth_view.command_failed = false;
            m->dirty = true;
        }
    }
    if (!m->pointer_down && fabsf(m->fling) > 0.5f) {
        m->view.mix_scroll = clamp_int(m->view.mix_scroll + (int)m->fling,
                                       0, MIX_MAX_SCROLL);
        m->fling *= 0.75f;
        if (fabsf(m->fling) <= 0.5f) {
            m->view.mix_scroll = clamp_int(
                                     ((m->view.mix_scroll + TRACKER_MIX_STRIP_H / 2) /
                                      TRACKER_MIX_STRIP_H) * TRACKER_MIX_STRIP_H, 0, MIX_MAX_SCROLL);
        }
        m->dirty = true;
    }
    tracker_engine_get_status(m->engine, &m->view.status);
    m->view.speed_multiplier = tracker_engine_speed_multiplier(m->engine);
    app_audio_bus_status_t audio;
    app_audio_bus_get_status(m->bus, &audio);
    const int master = m->bus ? audio.master_level : tracker_engine_master_level(m->engine);
    m->view.master_level = (uint8_t)fmaxf((float)master, m->view.master_level * 0.875f);
    const bool clip = m->bus ? app_audio_bus_consume_clip(m->bus)
                      : tracker_engine_consume_master_clip(m->engine);
    m->clip_ticks = clip ? 30 : m->clip_ticks > 0 ? m->clip_ticks - 1 : 0;
    m->view.clipped = m->clip_ticks > 0;
    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        const int level = tracker_engine_channel_level(m->engine, ch);
        m->view.levels[ch] = (uint8_t)fmaxf((float)level, m->view.levels[ch] * 0.875f);
        if (!m->view.status.playing || m->view.muted[ch]) {
            m->view.levels[ch] = 0;
        }
    }
    const int launched = tracker_engine_launched_pattern(m->engine);
    m->view.pad_playing = m->view.status.playing ?
                          (launched >= 0 ? launched : m->view.status.pattern) : -1;
    m->view.pad_next = !m->view.status.playing ? -1 : launched >= 0 ?
                       tracker_engine_pending_pattern(m->engine) : next_pattern(m);
    if (m->view.pad_next == m->view.pad_playing) {
        m->view.pad_next = -1;
    }
    m->view.pad_stop = tracker_engine_pattern_stop_pending(m->engine);
    const float bpm = m->view.status.tempo * m->view.speed_multiplier;
    const uint64_t half_beat = bpm >= 1.0f ?
                               (uint64_t)(APP_AUDIO_SAMPLE_RATE * 30.0 / (double)bpm) : 0;
    m->view.pad_blink = !m->view.status.playing || half_beat == 0 ||
                        ((m->view.status.frames / half_beat) & 1U) != 0;
    (void)app_synth_get_status(m->synth, &m->synth_view.status);
    /* A touch stream without a release must not leave a held voice forever.
     * Native input normally sends held samples; allow a long stationary hold. */
    if (m->pointer_down && now_ms > m->last_pointer_time + 30000) {
        tracker_model_cancel_input(m);
    }
}
