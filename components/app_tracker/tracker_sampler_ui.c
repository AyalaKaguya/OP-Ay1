// SPDX-License-Identifier: Apache-2.0
#include "tracker_workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tracker_graphics.h"

#define SAMPLE_WAVE_LEFT 25
#define SAMPLE_WAVE_WIDTH 430
#define SAMPLE_WAVE_TOP 182
#define SAMPLE_WAVE_HEIGHT 100

static bool inside(int x, int y, int left, int top, int width, int height)
{
    return x >= left && x < left + width && y >= top && y < top + height;
}

static void result(tracker_workspace_t *w, app_sampler_result_t value)
{
    w->sampler_error = value;
    if (value != APP_SAMPLER_OK)
        tracker_workspace_message(w, app_sampler_result_name(value));
    w->dirty = true;
}

static void stop_audition(tracker_workspace_t *w)
{
    if (w->sampler) app_sampler_audition_stop(w->sampler);
    w->auditioning = false;
}

void tracker_sampler_ui_cancel(tracker_workspace_t *w)
{
    stop_audition(w);
    if (w->sampler && w->sample_edit_active) app_sampler_edit_cancel(w->sampler);
    w->sample_edit_active = w->sample_edit_dirty = false;
    w->sampler_pointer = false;
    w->sample_drag = TRACKER_SAMPLE_DRAG_NONE;
    w->dirty = true;
}

void tracker_sampler_ui_poll(tracker_workspace_t *w)
{
    if (!w->sampler) return;
    const unsigned count = app_sampler_count(w->sampler);
    const uint64_t revision = app_sampler_revision(w->sampler);
    if (count != w->sample_count || revision != w->sampler_revision) w->dirty = true;
    w->sample_count = count;
    w->sampler_revision = revision;
    if (w->sample >= count) w->sample = count ? count - 1U : 0U;
    if (w->sample_first > w->sample) w->sample_first = w->sample / 4U * 4U;
    app_sampler_sample_info_t next = {0};
    const bool valid = count && app_sampler_get_info(w->sampler, w->sample, &next);
    if (valid != w->sample_valid || (valid && memcmp(&next, &w->sample_info, sizeof(next)))) {
        if (valid) w->sample_info = next;
        w->dirty = true;
    }
    w->sample_valid = valid;
    for (unsigned row = 0; row < TRACKER_WORKSPACE_ROWS; ++row)
        w->library_valid[row] = app_sampler_get_info(w->sampler, w->sample_first + row,
                                                     &w->library_rows[row]);
    if (valid && w->dirty)
        w->waveform_count = app_sampler_waveform(w->sampler, w->sample, w->waveform,
                                                 TRACKER_WORKSPACE_WAVE_SAMPLES);
    else if (!valid) w->waveform_count = 0;
    app_sampler_record_status_t record;
    app_sampler_record_get_status(w->sampler, &record);
    if (memcmp(&record, &w->record, sizeof(record))) {
        w->record = record;
        w->mic_gain = record.gain_db;
        w->mic_monitor = record.monitor;
        w->dirty = true;
    }
    if (w->sample_edit_active) {
        app_sampler_edit_t edit;
        if (app_sampler_edit_get(w->sampler, &edit)) w->edit = edit;
    }
}

static void audition(tracker_workspace_t *w)
{
    if (w->auditioning) {
        stop_audition(w);
    } else if (w->sample_valid && w->sample_info.frames) {
        const app_sampler_result_t value = app_sampler_audition(w->sampler, w->sample);
        result(w, value);
        w->auditioning = value == APP_SAMPLER_OK;
    } else tracker_workspace_message(w, "EMPTY SAMPLE SLOT / NOTHING TO AUDITION");
    w->dirty = true;
}

static void begin_edit(tracker_workspace_t *w)
{
    if (!w->sample_valid || !w->sample_info.frames) {
        tracker_workspace_message(w, "SELECT A NONEMPTY SAMPLE FIRST");
        return;
    }
    stop_audition(w);
    const app_sampler_result_t value = app_sampler_edit_begin(w->sampler, w->sample);
    result(w, value);
    if (value == APP_SAMPLER_OK && app_sampler_edit_get(w->sampler, &w->edit)) {
        w->original_edit = w->edit;
        w->sample_edit_active = true;
        w->sample_edit_dirty = false;
        w->loop_handles = false;
        w->sampler_tab = TRACKER_SAMPLE_EDIT;
    }
}

static void set_edit(tracker_workspace_t *w)
{
    const app_sampler_result_t value = app_sampler_edit_set(w->sampler, &w->edit);
    result(w, value);
    if (value == APP_SAMPLER_OK) {
        w->sample_edit_dirty = memcmp(&w->edit, &w->original_edit, sizeof(w->edit)) != 0;
    } else (void)app_sampler_edit_get(w->sampler, &w->edit);
}

static void drag_edit(tracker_workspace_t *w, int x)
{
    if (!w->sample_edit_active || !w->sample_info.frames) return;
    if (x < SAMPLE_WAVE_LEFT) x = SAMPLE_WAVE_LEFT;
    if (x >= SAMPLE_WAVE_LEFT + SAMPLE_WAVE_WIDTH) x = SAMPLE_WAVE_LEFT + SAMPLE_WAVE_WIDTH - 1;
    const uint32_t frame = (uint32_t)((uint64_t)(x - SAMPLE_WAVE_LEFT) *
                                     w->sample_info.frames / (SAMPLE_WAVE_WIDTH - 1));
    switch (w->sample_drag) {
    case TRACKER_SAMPLE_DRAG_TRIM_START:
        w->edit.trim_start = frame < w->edit.trim_end ? frame : w->edit.trim_end - 1U;
        if (w->edit.loop_start < w->edit.trim_start) w->edit.loop_start = w->edit.trim_start;
        if (w->edit.loop_end <= w->edit.loop_start) w->edit.loop_end = w->edit.loop_start + 1U;
        break;
    case TRACKER_SAMPLE_DRAG_TRIM_END:
        w->edit.trim_end = frame > w->edit.trim_start ? frame : w->edit.trim_start + 1U;
        if (w->edit.loop_end > w->edit.trim_end) w->edit.loop_end = w->edit.trim_end;
        if (w->edit.loop_start >= w->edit.loop_end) w->edit.loop_start = w->edit.loop_end - 1U;
        break;
    case TRACKER_SAMPLE_DRAG_LOOP_START:
        w->edit.loop_start = frame < w->edit.trim_start ? w->edit.trim_start :
                             frame < w->edit.loop_end ? frame : w->edit.loop_end - 1U;
        break;
    case TRACKER_SAMPLE_DRAG_LOOP_END:
        w->edit.loop_end = frame > w->edit.trim_end ? w->edit.trim_end :
                           frame > w->edit.loop_start ? frame : w->edit.loop_start + 1U;
        break;
    default: return;
    }
    set_edit(w);
}

static void worker(tracker_workspace_t *w, app_project_action_t action)
{
    tracker_workspace_end_source_edit(w, false);
    memset(&w->request, 0, sizeof(w->request));
    w->request.sample_index = w->sample;
    w->request.target_slot = w->assign_slot;
    if (action == APP_PROJECT_RECORD_KEEP)
        snprintf(w->request.name, sizeof(w->request.name), "TAKE %02u", w->sample_count + 1U);
    tracker_workspace_request(w, action);
}

static void tap_library(tracker_workspace_t *w, int x, int y)
{
    if (inside(x, y, 24, 140, 432, 180)) {
        const unsigned index = w->sample_first + (unsigned)(y - 140) / 45U;
        if (index < w->sample_count) {
            stop_audition(w);
            w->sample = index;
        }
    } else if (inside(x, y, 345, 125, 111, 15)) {
        w->import_wav = true;
        w->screen = TRACKER_WORKSPACE_IMPORT;
    } else if (inside(x, y, 24, 324, 62, 25)) {
        w->sample_first = w->sample_first >= 4U ? w->sample_first - 4U : 0;
        w->sample = w->sample_first;
    } else if (inside(x, y, 394, 324, 62, 25)) {
        if (w->sample_first + 4U < w->sample_count) w->sample_first += 4U;
        w->sample = w->sample_first;
    } else if (inside(x, y, 24, 382, 136, 40)) audition(w);
    else if (inside(x, y, 172, 382, 136, 40)) begin_edit(w);
    else if (inside(x, y, 320, 382, 136, 40) && w->sample_valid && w->sample_info.frames) {
        stop_audition(w);
        w->sampler_tab = TRACKER_SAMPLE_ASSIGN;
        w->assign_slot = w->sample_info.assigned_slot ? w->sample_info.assigned_slot :
                         w->sample_info.original_slot ? w->sample_info.original_slot : 1U;
    }
    w->dirty = true;
}

static void tap_edit(tracker_workspace_t *w, int x, int y)
{
    if (inside(x, y, 350, 136, 106, 26)) worker(w, APP_PROJECT_UNDO_SAMPLE);
    else if (!w->sample_edit_active) {
        if (inside(x, y, 24, 382, 432, 40)) begin_edit(w);
    } else if (inside(x, y, 24, 382, 136, 40)) {
        tracker_sampler_ui_cancel(w);
        w->sampler_tab = TRACKER_SAMPLE_LIBRARY;
    } else if (inside(x, y, 172, 382, 136, 40)) audition(w);
    else if (inside(x, y, 320, 382, 136, 40)) worker(w, APP_PROJECT_APPLY_SAMPLE);
    else if (inside(x, y, 24, 288, 318, 22)) w->loop_handles = false;
    else if (inside(x, y, 24, 312, 318, 22)) w->loop_handles = true;
    else if (inside(x, y, 350, 308, 106, 30)) {
        w->edit.loop_enabled = !w->edit.loop_enabled;
        if (w->edit.loop_end <= w->edit.loop_start) {
            w->edit.loop_start = w->edit.trim_start;
            w->edit.loop_end = w->edit.trim_end;
        }
        set_edit(w);
    } else if (inside(x, y, 24, 342, 264, 30)) {
        const int64_t delta = x < 155 ? -100 : 100;
        int64_t next = (int64_t)w->edit.c2_speed + delta;
        if (next < 100) next = 100;
        if (next > 192000) next = 192000;
        w->edit.c2_speed = (uint32_t)next;
        set_edit(w);
    } else if (inside(x, y, 300, 342, 156, 30)) {
        float next = w->edit.gain + (x < 378 ? -0.05f : 0.05f);
        w->edit.gain = next < 0 ? 0 : next > 2 ? 2 : next;
        set_edit(w);
    }
    w->dirty = true;
}

static void tap_record(tracker_workspace_t *w, int x, int y)
{
    const app_sampler_record_state_t state = w->record.state;
    if (inside(x, y, 24, 182, 208, 32)) {
        float gain = w->mic_gain + (x < 128 ? -6.0f : 6.0f);
        if (gain < 0.0f) gain = 0.0f;
        if (gain > 42.0f) gain = 42.0f;
        result(w, app_sampler_record_set_gain(w->sampler, gain));
    } else if (inside(x, y, 248, 182, 208, 32)) {
        app_sampler_record_set_monitor(w->sampler, !w->mic_monitor);
    } else if (state == APP_SAMPLER_RECORD_ERROR) {
        if (inside(x, y, 24, 382, 432, 40)) {
            stop_audition(w);
            app_sampler_record_discard(w->sampler);
            tracker_workspace_message(w, "FAILED TAKE DISCARDED / PROJECT UNCHANGED");
        }
    } else if (state == APP_SAMPLER_RECORD_READY) {
        if (inside(x, y, 24, 382, 136, 40)) {
            stop_audition(w);
            app_sampler_record_discard(w->sampler);
            tracker_workspace_message(w, "TAKE DISCARDED / PROJECT UNCHANGED");
        } else if (inside(x, y, 172, 382, 136, 40)) {
            if (w->auditioning) stop_audition(w);
            else {
                const app_sampler_result_t value = app_sampler_record_audition(w->sampler);
                result(w, value);
                w->auditioning = value == APP_SAMPLER_OK;
            }
        } else if (inside(x, y, 320, 382, 136, 40)) worker(w, APP_PROJECT_RECORD_KEEP);
    } else if (inside(x, y, 24, 382, 432, 40)) {
        if (state == APP_SAMPLER_RECORD_RECORDING) app_sampler_record_stop(w->sampler);
        else if (state != APP_SAMPLER_RECORD_STOPPING) worker(w, APP_PROJECT_RECORD_START);
    }
    w->dirty = true;
}

static void tap_assign(tracker_workspace_t *w, int x, int y)
{
    if (inside(x, y, 24, 208, 432, 108)) {
        const unsigned slot = (unsigned)((y - 208) / 54) * 8U + (unsigned)(x - 24) / 54U + 1U;
        if (slot <= 16U) w->assign_slot = slot;
    } else if (inside(x, y, 24, 382, 208, 40)) w->sampler_tab = TRACKER_SAMPLE_LIBRARY;
    else if (inside(x, y, 248, 382, 208, 40)) worker(w, APP_PROJECT_ASSIGN_SAMPLE);
    w->dirty = true;
}

static void tap(tracker_workspace_t *w, int x, int y)
{
    if (inside(x, y, 24, 92, 432, 36)) {
        const int tab = (x - 24) / 148;
        if (tab < 3 && (x - 24) % 148 < 136) {
            if (tab == (int)w->sampler_tab) return;
            if (tab == TRACKER_SAMPLE_EDIT && !w->sample_edit_active) begin_edit(w);
            else {
                tracker_sampler_ui_cancel(w);
                w->sampler_tab = (tracker_sampler_tab_t)tab;
            }
        }
    } else if (w->sampler_tab == TRACKER_SAMPLE_LIBRARY) tap_library(w, x, y);
    else if (w->sampler_tab == TRACKER_SAMPLE_EDIT) tap_edit(w, x, y);
    else if (w->sampler_tab == TRACKER_SAMPLE_RECORD) tap_record(w, x, y);
    else tap_assign(w, x, y);
}

void tracker_sampler_ui_pointer(tracker_workspace_t *w, tracker_model_t *model,
                                 int x, int y, bool pressed)
{
    (void)model;
    if (!w->sampler || tracker_workspace_busy(w)) return;
    if (!tracker_workspace_begin_source_edit(w)) return;
    if (pressed && !w->sampler_pointer) {
        w->sampler_pointer = true;
        w->start_x = x;
        w->start_y = y;
        w->moved = false;
        if (w->sampler_tab == TRACKER_SAMPLE_EDIT && w->sample_edit_active &&
                inside(x, y, SAMPLE_WAVE_LEFT, SAMPLE_WAVE_TOP, SAMPLE_WAVE_WIDTH, SAMPLE_WAVE_HEIGHT)) {
            const uint32_t frame = (uint32_t)((uint64_t)(x - SAMPLE_WAVE_LEFT) *
                                             w->sample_info.frames / SAMPLE_WAVE_WIDTH);
            const uint32_t a = w->loop_handles ? w->edit.loop_start : w->edit.trim_start;
            const uint32_t b = w->loop_handles ? w->edit.loop_end : w->edit.trim_end;
            const uint32_t da = frame > a ? frame - a : a - frame;
            const uint32_t db = frame > b ? frame - b : b - frame;
            w->sample_drag = w->loop_handles ?
                (da <= db ? TRACKER_SAMPLE_DRAG_LOOP_START : TRACKER_SAMPLE_DRAG_LOOP_END) :
                (da <= db ? TRACKER_SAMPLE_DRAG_TRIM_START : TRACKER_SAMPLE_DRAG_TRIM_END);
            drag_edit(w, x);
        }
    } else if (w->sampler_pointer) {
        if (abs(x - w->start_x) > 12 || abs(y - w->start_y) > 12) w->moved = true;
        if (w->sample_drag != TRACKER_SAMPLE_DRAG_NONE) drag_edit(w, x);
        if (!pressed) {
            const bool tapped = !w->moved && w->sample_drag == TRACKER_SAMPLE_DRAG_NONE;
            w->sampler_pointer = false;
            w->sample_drag = TRACKER_SAMPLE_DRAG_NONE;
            /* worker() releases this guard before submitting asynchronous work.
             * Bounded preview/record controls keep it through their mutation. */
            if (tapped) tap(w, x, y);
            tracker_workspace_end_source_edit(w, false);
            return;
        }
    }
    tracker_workspace_end_source_edit(w, false);
}

static void label(tracker_graphics_t *g, int x, int y, const char *text, uint16_t color)
{
    tracker_gfx_text(g, x, y, 1, color, text);
}

static void button(tracker_graphics_t *g, int x, int y, int width, int height,
                     const char *text, bool active)
{
    tracker_gfx_button(g, x, y, width, height, text, active ? C_CYAN : C_MUTED, active);
}

static void waveform(tracker_graphics_t *g, const tracker_workspace_t *w, int top, int height)
{
    tracker_gfx_rect(g, 25, top, 430, height, C_PANEL);
    tracker_gfx_outline(g, 25, top, 430, height, C_BORDER);
    tracker_gfx_line(g, 26, top + height / 2, 453, top + height / 2, C_BORDER);
    for (unsigned i = 0; i < w->waveform_count; ++i) {
        const int amplitude = (int)w->waveform[i] * (height / 2 - 4) / 32768;
        const int x = 27 + (int)i * 426 / (int)TRACKER_WORKSPACE_WAVE_SAMPLES;
        tracker_gfx_line(g, x, top + height / 2, x, top + height / 2 - amplitude, C_CYAN);
    }
}

static void library(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    char text[80];
    snprintf(text, sizeof(text), "%u SLOTS / %u-%u", w->sample_count,
             w->sample_count ? w->sample_first + 1U : 0U,
             w->sample_first + 4U < w->sample_count ? w->sample_first + 4U : w->sample_count);
    label(g, 24, 130, text, C_MUTED);
    label(g, 398, 130, "+ IMPORT", C_CYAN);
    for (unsigned row = 0; row < TRACKER_WORKSPACE_ROWS; ++row) {
        if (!w->library_valid[row]) break;
        const app_sampler_sample_info_t *info = &w->library_rows[row];
        const unsigned index = w->sample_first + row;
        const int y = 140 + (int)row * 45;
        char name[27];
        tracker_workspace_pixel_name(name, sizeof(name), info->name, 24);
        tracker_gfx_rect(g, 24, y, 432, 40, C_PANEL);
        tracker_gfx_outline(g, 24, y, 432, 40, index == w->sample ? C_CYAN : C_BORDER);
        snprintf(text, sizeof(text), "%02u %s", index + 1U, name);
        label(g, 36, y + 9, text, index == w->sample ? C_CYAN : C_TEXT);
        const char *source = info->source == APP_SAMPLER_SOURCE_S3M ? "S3M" :
                             info->source == APP_SAMPLER_SOURCE_WAV ? "WAV" : "MIC";
        snprintf(text, sizeof(text), "%s / SLOT %02u / %u FR%s", source,
                 info->original_slot, (unsigned)info->frames,
                 !info->frames ? " / EMPTY" : info->loop_enabled ? " / LOOP" : "");
        label(g, 36, y + 25, text, C_MUTED);
    }
    if (!w->sample_count) label(g, 24, 220, "NO SAMPLES / IMPORT WAV OR RECORD A TAKE", C_MUTED);
    button(g, 24, 324, 62, 25, "<", false);
    button(g, 394, 324, 62, 25, ">", false);
    if (w->message[0]) {
        char text_name[49];
        tracker_workspace_pixel_name(text_name, sizeof(text_name), w->message, 47);
        label(g, 92, 334, text_name, w->sampler_error ? C_ACCENT : C_MUTED);
    }
    waveform(g, w, 353, 23);
    button(g, 24, 382, 136, 40, w->auditioning ? "STOP" : "AUDITION", w->auditioning);
    button(g, 172, 382, 136, 40, "EDIT", false);
    button(g, 320, 382, 136, 40, "ASSIGN", false);
}

static void edit(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    char text[80], name[27];
    tracker_workspace_pixel_name(name, sizeof(name), w->sample_valid ? w->sample_info.name : "NO SAMPLE", 24);
    tracker_gfx_text(g, 24, 145, 2, C_TEXT, name);
    button(g, 350, 136, 106, 26, "UNDO", false);
    if (!w->sample_edit_active) {
        label(g, 24, 194, "APPLY COMPLETE / ORIGINAL RESOURCE RETAINED", C_MUTED);
        label(g, 24, 220, "UNDO RESTORES THE PREVIOUS APPLIED EDIT", C_MUTED);
        button(g, 24, 382, 432, 40, "EDIT AGAIN", true);
        return;
    }
    waveform(g, w, SAMPLE_WAVE_TOP, SAMPLE_WAVE_HEIGHT);
    const uint32_t frames = w->sample_info.frames ? w->sample_info.frames : 1U;
    const uint32_t a = w->loop_handles ? w->edit.loop_start : w->edit.trim_start;
    const uint32_t b = w->loop_handles ? w->edit.loop_end : w->edit.trim_end;
    const int left = SAMPLE_WAVE_LEFT + (int)((uint64_t)a * (SAMPLE_WAVE_WIDTH - 1) / frames);
    const int right = SAMPLE_WAVE_LEFT + (int)((uint64_t)b * (SAMPLE_WAVE_WIDTH - 1) / frames);
    tracker_gfx_line(g, left, 180, left, 281, C_CYAN);
    tracker_gfx_line(g, right, 180, right, 281, C_ACCENT);
    label(g, 24, 170, w->loop_handles ? "DRAG NEAREST LOOP HANDLE" : "DRAG NEAREST TRIM HANDLE", C_CYAN);
    snprintf(text, sizeof(text), "TRIM %u - %u FR", (unsigned)w->edit.trim_start, (unsigned)w->edit.trim_end);
    label(g, 24, 294, text, w->loop_handles ? C_MUTED : C_CYAN);
    snprintf(text, sizeof(text), "LOOP %u - %u", (unsigned)w->edit.loop_start, (unsigned)w->edit.loop_end);
    label(g, 24, 318, text, w->loop_handles ? C_CYAN : C_MUTED);
    button(g, 350, 308, 106, 30, w->edit.loop_enabled ? "LOOP ON" : "LOOP OFF", w->edit.loop_enabled);
    snprintf(text, sizeof(text), "- C2 %u HZ +", (unsigned)w->edit.c2_speed);
    button(g, 24, 342, 264, 30, text, false);
    snprintf(text, sizeof(text), "- GAIN %u%% +", (unsigned)(w->edit.gain * 100.0f + 0.5f));
    button(g, 300, 342, 156, 30, text, false);
    button(g, 24, 382, 136, 40, "CANCEL", false);
    button(g, 172, 382, 136, 40, w->auditioning ? "STOP" : "PREVIEW", w->auditioning);
    button(g, 320, 382, 136, 40, "APPLY", true);
}

static void record(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    const app_sampler_record_status_t *r = &w->record;
    tracker_gfx_text(g, 24, 149, 2, C_TEXT, "MIC");
    label(g, 310, 155, "48K / 16 BIT / MONO", C_MUTED);
    char text[72];
    snprintf(text, sizeof(text), "- GAIN %+.0F DB +", (double)w->mic_gain);
    button(g, 24, 182, 208, 32, text, false);
    button(g, 248, 182, 208, 32, w->mic_monitor ? "MONITOR ON" : "MONITOR OFF", w->mic_monitor);
    const uint64_t cs = r->sample_rate ? (uint64_t)r->frames * 100U / r->sample_rate : 0;
    snprintf(text, sizeof(text), "%02llu:%02llu.%02llu", (unsigned long long)(cs / 6000U),
             (unsigned long long)(cs / 100U % 60U), (unsigned long long)(cs % 100U));
    tracker_gfx_text_center(g, 240, 245, 4,
                            r->state == APP_SAMPLER_RECORD_RECORDING ? C_ACCENT : C_TEXT, text);
    const int level = (int)(r->peak * 255.0f);
    tracker_gfx_meter(g, 24, 298, 432, 35, level < 0 ? 0 : level > 255 ? 255 : level);
    const char *state = r->state == APP_SAMPLER_RECORD_RECORDING ? "RECORDING / REAL MICROPHONE PCM" :
                        r->state == APP_SAMPLER_RECORD_STOPPING ? "STOPPING CAPTURE / PLEASE WAIT" :
                        r->state == APP_SAMPLER_RECORD_READY ? "TAKE READY / PREVIEW BEFORE KEEPING" :
                        r->state == APP_SAMPLER_RECORD_UNSUPPORTED ? "CAPTURE UNAVAILABLE ON THIS HOST" :
                        r->state == APP_SAMPLER_RECORD_ERROR ? app_sampler_result_name(r->result) :
                        "READY / NEW TAKE IN CURRENT PROJECT";
    label(g, 24, 350, state, r->state == APP_SAMPLER_RECORD_ERROR ? C_ACCENT : C_MUTED);
    if (r->state == APP_SAMPLER_RECORD_READY) {
        button(g, 24, 382, 136, 40, "DISCARD", false);
        button(g, 172, 382, 136, 40, w->auditioning ? "STOP" : "PREVIEW", w->auditioning);
        button(g, 320, 382, 136, 40, "KEEP TAKE", true);
    } else if (r->state == APP_SAMPLER_RECORD_ERROR) {
        button(g, 24, 382, 432, 40, "DISCARD FAILED TAKE", false);
    } else {
        const char *caption = r->state == APP_SAMPLER_RECORD_RECORDING ? "STOP" :
                              r->state == APP_SAMPLER_RECORD_STOPPING ? "STOPPING..." : "REC";
        tracker_gfx_button(g, 24, 382, 432, 40, caption, C_ACCENT, true);
    }
}

static void assign(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    tracker_gfx_text(g, 24, 146, 2, C_TEXT, "ASSIGN TO TRACKER SLOT");
    label(g, 24, 178, "EXPLICIT REPLACEMENT / PATTERN CELLS RETAINED", C_MUTED);
    for (unsigned slot = 1; slot <= 16U; ++slot) {
        char text[5];
        snprintf(text, sizeof(text), "%02u", slot);
        button(g, 24 + (int)(slot - 1U) % 8 * 54, 208 + (int)(slot - 1U) / 8 * 54,
               48, 46, text, slot == w->assign_slot);
    }
    label(g, 24, 334, "SECTION PADS STILL LAUNCH PATTERNS", C_MUTED);
    label(g, 24, 353, "SAMPLE ASSIGNMENT NEVER CHANGES PAD MAPPINGS", C_MUTED);
    button(g, 24, 382, 208, 40, "CANCEL", false);
    button(g, 248, 382, 208, 40, "ASSIGN", true);
}

void tracker_sampler_ui_render(uint16_t *frame, const tracker_workspace_t *w)
{
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    static const char *const tabs[] = {"LIBRARY", "EDIT", "RECORD"};
    for (int tab = 0; tab < 3; ++tab)
        button(&g, 24 + tab * 148, 92, 136, 36, tabs[tab], (int)w->sampler_tab == tab);
    if (!w->sampler) {
        label(&g, 24, 190, "SAMPLER SERVICE UNAVAILABLE", C_ACCENT);
        return;
    }
    if (w->sampler_tab == TRACKER_SAMPLE_LIBRARY) library(&g, w);
    else if (w->sampler_tab == TRACKER_SAMPLE_EDIT) edit(&g, w);
    else if (w->sampler_tab == TRACKER_SAMPLE_RECORD) record(&g, w);
    else assign(&g, w);
    if (w->sampler_error != APP_SAMPLER_OK)
        label(&g, 24, 374, app_sampler_result_name(w->sampler_error), C_ACCENT);
}
