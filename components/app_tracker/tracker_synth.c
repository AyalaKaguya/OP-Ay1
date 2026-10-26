// SPDX-License-Identifier: Apache-2.0
#include "tracker_model.h"

#include <math.h>
#include <stdio.h>

#include "tracker_graphics.h"

enum {
    PARAM_DETUNE,
    PARAM_LEVEL,
    PARAM_TABLE,
    PARAM_CUTOFF,
    PARAM_RESONANCE,
    PARAM_ATTACK,
    PARAM_DECAY,
    PARAM_SUSTAIN,
    PARAM_RELEASE,
    PARAM_LFO_RATE,
    PARAM_LFO_DEPTH,
};

static float fraction_at(int x, int left, int width)
{
    return fmaxf(0.0f, fminf(1.0f, (float)(x - left) / (float)(width - 1)));
}

int tracker_synth_note_at(int x, int y, int octave)
{
    static const int white[] = {0, 2, 4, 5, 7, 9, 11, 12};
    static const int black_boundary[] = {1, 2, 4, 5, 6};
    static const int black_note[] = {1, 3, 6, 8, 10};
    if (x < 30 || x >= 450 || y < 327 || y >= 432) {
        return -1;
    }
    if (y < 390) {
        for (int key = 0; key < 5; ++key) {
            const int center = 30 + black_boundary[key] * 420 / 8;
            if (x >= center - 15 && x < center + 15) {
                return (octave + 1) * 12 + black_note[key];
            }
        }
    }
    const int key = (x - 30) * 8 / 420;
    const int note = (octave + 1) * 12 + white[key];
    return note > 127 ? -1 : note;
}

int tracker_synth_parameter_at(int tab, int x, int y)
{
    if (y < 154 || y >= 263) {
        return -1;
    }
    if (tab == 0 && x >= 234 && x < 440) {
        return y < 207 ? PARAM_DETUNE : PARAM_LEVEL;
    }
    if (tab == 1 && x >= 130 && x < 440) {
        return y < 207 ? PARAM_CUTOFF : PARAM_RESONANCE;
    }
    if (tab == 2 && x >= 38 && x < 440) {
        return PARAM_ATTACK + (x - 38) / 101;
    }
    if (tab == 3 && x >= 234 && x < 440) {
        return y < 207 ? PARAM_LFO_RATE : PARAM_LFO_DEPTH;
    }
    return -1;
}

void tracker_synth_set_parameter(tracker_synth_view_t *v, int parameter, int x)
{
    float f = fraction_at(x, v->tab == 1 ? 130 : 234, v->tab == 1 ? 300 : 196);
    app_synth_patch_t *p = &v->patch;
    switch (parameter) {
    case PARAM_DETUNE:
        if (p->waveform == APP_SYNTH_WAVE_WAVETABLE) {
            p->wavetable_position = f;
        } else {
            p->detune_cents = roundf(f * 200.0f) - 100.0f;
        }
        break;
    case PARAM_LEVEL:
        p->gain = f <= 0.01f ? 0.0f : powf(10.0f, (-48.0f + 51.5f * f) / 20.0f);
        break;
    case PARAM_TABLE:
        p->wavetable_position = f;
        break;
    case PARAM_CUTOFF:
        p->filter_cutoff_hz = 20.0f * powf(1000.0f, f);
        break;
    case PARAM_RESONANCE:
        p->filter_resonance = 0.95f * f;
        break;
    case PARAM_ATTACK:
    case PARAM_DECAY:
    case PARAM_SUSTAIN:
    case PARAM_RELEASE: {
        const int slot = parameter - PARAM_ATTACK;
        f = fraction_at(x, 44 + slot * 101, 80);
        const float seconds = f * f * 5.0f;
        if (parameter == PARAM_ATTACK) {
            p->attack_s = seconds;
        } else if (parameter == PARAM_DECAY) {
            p->decay_s = seconds;
        } else if (parameter == PARAM_SUSTAIN) {
            p->sustain = f;
        } else {
            p->release_s = seconds;
        }
        break;
    }
    case PARAM_LFO_RATE:
        p->lfo_hz = f * f * 40.0f;
        break;
    case PARAM_LFO_DEPTH:
        p->lfo_depth = f * (p->lfo_target == APP_SYNTH_LFO_TARGET_PITCH ? 12.0f : 1.0f);
        break;
    default:
        break;
    }
}

static void parameter(tracker_graphics_t *g, const char *name, const char *value,
                      int y, float fraction, int left, int width)
{
    tracker_gfx_text(g, left, y, 1, C_MUTED, name);
    tracker_gfx_text_right(g, left + width, y - 2, 1, C_TEXT, value);
    tracker_gfx_slider(g, left, y + 24, width, fraction, C_CYAN, false);
}

static float shape_at(app_synth_waveform_t waveform, float phase, float position)
{
    switch (waveform) {
    case APP_SYNTH_WAVE_SAW:
        return 2.0f * phase - 1.0f;
    case APP_SYNTH_WAVE_SQUARE:
        return phase < 0.5f ? 0.8f : -0.8f;
    case APP_SYNTH_WAVE_TRIANGLE:
        return 1.0f - 4.0f * fabsf(phase - 0.5f);
    case APP_SYNTH_WAVE_WAVETABLE:
        return 0.65f * sinf(phase * 6.2831853f) +
               position * 0.3f * sinf(phase * 18.849556f);
    default:
        return sinf(phase * 6.2831853f);
    }
}

static void oscillator(tracker_graphics_t *g, const app_synth_patch_t *p)
{
    static const char *const names[] = {"SAW", "SQUARE", "TRIANGLE", "SINE", "TABLE"};
    char value[24];
    const int wave = p->waveform >= 0 && p->waveform <= APP_SYNTH_WAVE_WAVETABLE ? p->waveform : 0;
    tracker_gfx_text(g, 44, 168, 2, C_TEXT, names[wave]);
    tracker_gfx_text(g, 194, 171, 1, C_MUTED, ">");
    tracker_gfx_text(g, 44, 190, 1, C_MUTED,
                     wave == APP_SYNTH_WAVE_WAVETABLE ? "WAVETABLE" : "ANALOG");
    tracker_gfx_rect(g, 44, 232, 152, 1, C_LINE);
    int previous = 231;
    for (int px = 0; px < 152; ++px) {
        const float phase = fmodf(px / 48.0f, 1.0f);
        const int y = 232 - (int)(shape_at(p->waveform, phase, p->wavetable_position) * 18.0f);
        tracker_gfx_line(g, 44 + px, previous, 44 + px, y, C_CYAN);
        previous = y;
    }
    tracker_gfx_rect(g, 216, 169, 1, 79, C_BORDER);
    if (p->waveform == APP_SYNTH_WAVE_WAVETABLE) {
        snprintf(value, sizeof(value), "%d%%", (int)(p->wavetable_position * 100.0f));
        parameter(g, "POSITION", value, 167, p->wavetable_position, 234, 196);
    } else {
        snprintf(value, sizeof(value), "%+.0f CT", (double)p->detune_cents);
        parameter(g, "DETUNE", value, 167, (p->detune_cents + 100.0f) / 200.0f, 234, 196);
    }
    if (p->gain <= 0.0f) {
        snprintf(value, sizeof(value), "MUTE");
    } else {
        snprintf(value, sizeof(value), "%+.1f DB", (double)(20.0f * log10f(p->gain)));
    }
    const float fraction = p->gain > 0.0f ?
                           (20.0f * log10f(p->gain) + 48.0f) / 51.5f : 0.0f;
    parameter(g, "LEVEL", value, 215, fraction, 234, 196);
}

static void filter(tracker_graphics_t *g, const app_synth_patch_t *p)
{
    char value[24];
    tracker_gfx_text(g, 44, 168, 2, C_TEXT, "LPF");
    tracker_gfx_text(g, 44, 193, 1, C_MUTED, "LOWPASS");
    snprintf(value, sizeof(value), "%.0f HZ", (double)p->filter_cutoff_hz);
    parameter(g, "CUTOFF", value, 167,
              logf(p->filter_cutoff_hz / 20.0f) / logf(1000.0f), 130, 300);
    snprintf(value, sizeof(value), "%.0f%%", (double)(p->filter_resonance / 0.95f * 100.0f));
    parameter(g, "RESONANCE", value, 215, p->filter_resonance / 0.95f, 130, 300);
}

static void envelope(tracker_graphics_t *g, const app_synth_patch_t *p)
{
    static const char *const names[] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
    const float values[] = {p->attack_s, p->decay_s, p->sustain, p->release_s};
    char text[20];
    for (int i = 0; i < 4; ++i) {
        const int x = 44 + i * 101;
        tracker_gfx_text(g, x, 169, 1, C_MUTED, names[i]);
        if (i == 2) {
            snprintf(text, sizeof(text), "%.0f%%", (double)(values[i] * 100.0f));
        } else if (values[i] < 1.0f) {
            snprintf(text, sizeof(text), "%.0fMS", (double)(values[i] * 1000.0f));
        } else {
            snprintf(text, sizeof(text), "%.2fS", (double)values[i]);
        }
        tracker_gfx_text(g, x, 196, 1, C_TEXT, text);
        const float f = i == 2 ? values[i] : sqrtf(values[i] / 5.0f);
        tracker_gfx_slider(g, x, 238, 80, f, C_CYAN, false);
    }
}

static void lfo(tracker_graphics_t *g, const app_synth_patch_t *p)
{
    static const char *const names[] = {"PITCH", "CUTOFF", "AMP"};
    char value[20];
    tracker_gfx_text(g, 44, 168, 2, C_TEXT, "LFO");
    tracker_gfx_text(g, 44, 193, 1, C_MUTED, "SINE");
    tracker_gfx_text(g, 44, 236, 1, C_CYAN, names[p->lfo_target]);
    tracker_gfx_text(g, 194, 236, 1, C_MUTED, ">");
    snprintf(value, sizeof(value), "%.2f HZ", (double)p->lfo_hz);
    parameter(g, "RATE", value, 167, sqrtf(p->lfo_hz / 40.0f), 234, 196);
    snprintf(value, sizeof(value), "%.2f", (double)p->lfo_depth);
    parameter(g, "DEPTH", value, 215,
              p->lfo_depth / (p->lfo_target == APP_SYNTH_LFO_TARGET_PITCH ? 12.0f : 1.0f),
              234, 196);
}

void tracker_render_synth(uint16_t *frame, const tracker_synth_view_t *v)
{
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_graphics_t *g = &gfx;
    char text[32];
    tracker_gfx_rect(g, 0, 0, 480, 432, C_BG);
    tracker_gfx_text(g, 30, 14, 2, C_TEXT, "SYNTH");
    snprintf(text, sizeof(text), "%s / %u:04",
             v->patch.waveform == APP_SYNTH_WAVE_WAVETABLE ? "WT" : "SUB",
             (unsigned)v->status.active_voices);
    tracker_gfx_text_right(g, 450, 19, 1, C_MUTED, text);
    tracker_gfx_rect(g, 30, 52, 420, 44, C_PANEL);
    tracker_gfx_outline(g, 30, 52, 420, 44, C_BORDER);
    tracker_gfx_text(g, 52, 68, 2, C_MUTED, "<");
    snprintf(text, sizeof(text), "%02d %s", v->preset + 1,
             app_synth_preset_name((app_synth_patch_id_t)v->preset));
    tracker_gfx_text(g, 84, 68, 2, C_TEXT, text);
    tracker_gfx_text(g, 420, 68, 2, C_CYAN, ">");
    static const char *const tabs[] = {"OSC", "FILTER", "ENV", "LFO"};
    for (int i = 0; i < 4; ++i) {
        const int x = 30 + 107 * i;
        tracker_gfx_rect(g, x, 108, 99, 35, i == v->tab ? C_PANEL : C_BG);
        tracker_gfx_text_center(g, x + 49, 120, 1,
                                i == v->tab ? C_CYAN : C_MUTED, tabs[i]);
        if (i == v->tab) {
            tracker_gfx_rect(g, x + 12, 141, 75, 2, C_CYAN);
        }
    }
    tracker_gfx_rect(g, 30, 154, 420, 109, C_PANEL);
    if (v->tab == 0) {
        oscillator(g, &v->patch);
    } else if (v->tab == 1) {
        filter(g, &v->patch);
    } else if (v->tab == 2) {
        envelope(g, &v->patch);
    } else {
        lfo(g, &v->patch);
    }
    tracker_gfx_button(g, 30, 277, 42, 36, "-", C_TEXT, false);
    snprintf(text, sizeof(text), "OCT %d", v->octave);
    tracker_gfx_text_center(g, 101, 291, 1, C_TEXT, text);
    tracker_gfx_button(g, 130, 277, 42, 36, "+", C_TEXT, false);
    if (v->note >= 0) {
        static const char *const note_names[] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        snprintf(text, sizeof(text), "%s%d", note_names[v->note % 12], v->note / 12 - 1);
        tracker_gfx_text_center(g, 260, 281, 2, C_ACCENT, text);
        snprintf(text, sizeof(text), "%.1f HZ", (double)(440.0f * powf(2.0f, (v->note - 69) / 12.0f)));
        tracker_gfx_text_center(g, 260, 305, 1, C_MUTED, text);
    } else {
        tracker_gfx_text_center(g, 260, 291, 1, v->command_failed ? C_ACCENT : C_MUTED,
                                v->command_failed ? "BUSY" : "READY");
    }
    tracker_gfx_button(g, 348, 277, 102, 36, "PANIC", C_ACCENT, false);
    static const char *const whites[] = {"C", "D", "E", "F", "G", "A", "B", "C"};
    for (int key = 0; key < 8; ++key) {
        const int x = 30 + key * 420 / 8;
        const int right = 30 + (key + 1) * 420 / 8;
        const bool active = v->note == tracker_synth_note_at(x + 22, 420, v->octave);
        tracker_gfx_rect(g, x + 1, 327, right - x - 2, 105,
                         active ? TRACKER_RGB565(32, 66, 77) : TRACKER_RGB565(220, 225, 223));
        tracker_gfx_text_center(g, (x + right) / 2, 409, 2,
                                active ? C_TEXT : C_BORDER, whites[key]);
        if (active) {
            tracker_gfx_rect(g, x + 7, 426, right - x - 14, 3, C_ACCENT);
        }
    }
    static const int boundaries[] = {1, 2, 4, 5, 6};
    for (int key = 0; key < 5; ++key) {
        const int x = 30 + boundaries[key] * 420 / 8;
        const bool active = v->note == tracker_synth_note_at(x, 350, v->octave);
        tracker_gfx_rect(g, x - 15, 327, 30, 63, active ? C_ACCENT : C_PANEL);
        tracker_gfx_outline(g, x - 15, 327, 30, 63, active ? C_ACCENT : C_BORDER);
    }
}
