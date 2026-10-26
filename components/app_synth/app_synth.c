// SPDX-License-Identifier: Apache-2.0
#include "app_synth.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define SYNTH_PI 3.14159265358979323846f
#define SYNTH_TWO_PI (2.0f * SYNTH_PI)
#define SYNTH_TABLE_COUNT 4U
#define SYNTH_CONTROL_INTERVAL 32U
#define SYNTH_SMOOTH_SECONDS 0.010f
#define SYNTH_VOICE_GAIN 0.18f
#define SYNTH_ENV_EPSILON 0.00001f
#define SYNTH_MIN_RATE 8000.0f
#define SYNTH_MAX_RATE 192000.0f
#define SYNTH_PATCH_FLOATS 12U

typedef enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } env_stage_t;
typedef enum { CMD_PATCH, CMD_NOTE_ON, CMD_NOTE_OFF, CMD_ALL_OFF, CMD_PANIC } command_type_t;

typedef struct {
    command_type_t type;
    int note;
    float velocity;
    app_synth_patch_id_t patch_id;
    app_synth_patch_t patch;
} synth_command_t;

typedef struct {
    bool active;
    bool held;
    uint8_t note;
    env_stage_t stage;
    uint64_t age;
    float velocity;
    float phase;
    float lfo_phase;
    float envelope;
    float release_step;
    float phase_step;
    float amp_mod;
    float ic1;
    float ic2;
    float a1;
    float a2;
    float a3;
    float k;
} synth_voice_t;

struct app_synth {
    float sample_rate;
    float smooth_coefficient;
    app_synth_patch_t current;
    app_synth_patch_t target;
    float wave_mix[5];
    float tables[SYNTH_TABLE_COUNT][APP_SYNTH_WAVETABLE_SIZE + 1U];
    synth_voice_t voices[APP_SYNTH_VOICE_COUNT];
    uint64_t next_age;
    uint64_t frames;
    uint32_t control_countdown;
    uint32_t stolen;
    uint32_t emergency_count;
    uint32_t panics;
    app_synth_patch_id_t preset_id;

    atomic_flag queue_lock;
    synth_command_t commands[APP_SYNTH_COMMAND_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    atomic_uint queue_depth;
    atomic_uint dropped;
    atomic_uint release_mask[4];
    atomic_bool emergency_all_off;
    atomic_bool emergency_panic;

    /* Atomic words avoid the C data race of a plain-data seqlock. */
    atomic_uint patch_sequence;
    atomic_uint patch_words[SYNTH_PATCH_FLOATS + 2U];
    atomic_uint status_sequence;
    atomic_uint status_frames_low;
    atomic_uint status_frames_high;
    atomic_uint status_active;
    atomic_uint status_stolen;
    atomic_uint status_emergencies;
    atomic_uint status_panics;
    atomic_uint status_preset;
    atomic_uint status_peak;
    atomic_bool status_clipped;
};

static float clampf(float value, float low, float high)
{
    if (!isfinite(value)) {
        return low;
    }
    return value < low ? low : (value > high ? high : value);
}

static uint32_t float_word(float value)
{
    uint32_t word;
    memcpy(&word, &value, sizeof(word));
    return word;
}

static float word_float(uint32_t word)
{
    float value;
    memcpy(&value, &word, sizeof(value));
    return value;
}

static app_synth_patch_t sanitize_patch(const app_synth_patch_t *input)
{
    app_synth_patch_t patch = *input;
    if (patch.waveform < APP_SYNTH_WAVE_SAW || patch.waveform > APP_SYNTH_WAVE_WAVETABLE) {
        patch.waveform = APP_SYNTH_WAVE_SAW;
    }
    if (patch.lfo_target < APP_SYNTH_LFO_TARGET_PITCH || patch.lfo_target > APP_SYNTH_LFO_TARGET_AMP) {
        patch.lfo_target = APP_SYNTH_LFO_TARGET_PITCH;
    }
    patch.wavetable_position = clampf(patch.wavetable_position, 0.0f, 1.0f);
    patch.detune_cents = clampf(patch.detune_cents, -100.0f, 100.0f);
    patch.gain = clampf(patch.gain, 0.0f, 1.5f);
    patch.pan = clampf(patch.pan, -1.0f, 1.0f);
    patch.filter_cutoff_hz = clampf(patch.filter_cutoff_hz, 20.0f, 20000.0f);
    patch.filter_resonance = clampf(patch.filter_resonance, 0.0f, 0.95f);
    patch.attack_s = clampf(patch.attack_s, 0.0f, 5.0f);
    patch.decay_s = clampf(patch.decay_s, 0.0f, 5.0f);
    patch.sustain = clampf(patch.sustain, 0.0f, 1.0f);
    patch.release_s = clampf(patch.release_s, 0.0f, 5.0f);
    patch.lfo_hz = clampf(patch.lfo_hz, 0.0f, 40.0f);
    patch.lfo_depth = clampf(patch.lfo_depth, 0.0f,
                           patch.lfo_target == APP_SYNTH_LFO_TARGET_PITCH ? 12.0f : 1.0f);
    return patch;
}

app_synth_patch_t app_synth_patch_for_preset(app_synth_patch_id_t id)
{
    app_synth_patch_t patch = {
        .waveform = APP_SYNTH_WAVE_SAW, .wavetable_position = 0.0f,
        .detune_cents = 0.0f, .gain = 0.8f, .pan = 0.0f,
        .filter_cutoff_hz = 6000.0f, .filter_resonance = 0.15f,
        .attack_s = 0.008f, .decay_s = 0.15f, .sustain = 0.7f, .release_s = 0.18f,
        .lfo_hz = 4.5f, .lfo_depth = 0.0f, .lfo_target = APP_SYNTH_LFO_TARGET_PITCH,
    };
    switch (id) {
    case APP_SYNTH_PATCH_BASS:
        patch.waveform = APP_SYNTH_WAVE_SQUARE;
        patch.filter_cutoff_hz = 650.0f;
        patch.filter_resonance = 0.35f;
        patch.attack_s = 0.003f;
        patch.decay_s = 0.18f;
        patch.sustain = 0.55f;
        patch.release_s = 0.09f;
        patch.gain = 0.95f;
        break;
    case APP_SYNTH_PATCH_LEAD:
        patch.filter_cutoff_hz = 4200.0f;
        patch.filter_resonance = 0.28f;
        patch.attack_s = 0.012f;
        patch.sustain = 0.85f;
        patch.release_s = 0.15f;
        patch.lfo_hz = 5.3f;
        patch.lfo_depth = 0.15f;
        break;
    case APP_SYNTH_PATCH_PAD:
        patch.waveform = APP_SYNTH_WAVE_WAVETABLE;
        patch.wavetable_position = 0.3f;
        patch.filter_cutoff_hz = 2300.0f;
        patch.filter_resonance = 0.1f;
        patch.attack_s = 0.5f;
        patch.decay_s = 0.6f;
        patch.sustain = 0.75f;
        patch.release_s = 0.9f;
        patch.lfo_hz = 0.7f;
        patch.lfo_depth = 0.22f;
        patch.lfo_target = APP_SYNTH_LFO_TARGET_CUTOFF;
        patch.gain = 0.72f;
        break;
    default:
        break;
    }
    return patch;
}

const char *app_synth_preset_name(app_synth_patch_id_t id)
{
    static const char *const names[] = {"INIT", "BASS", "LEAD", "PAD"};
    return id >= APP_SYNTH_PATCH_INIT && id < APP_SYNTH_PATCH_COUNT ? names[id] : "CUSTOM";
}

static void store_patch_snapshot(app_synth_t *synth, const app_synth_patch_t *patch)
{
    atomic_fetch_add_explicit(&synth->patch_sequence, 1U, memory_order_acq_rel);
    atomic_store_explicit(&synth->patch_words[0], (unsigned)patch->waveform, memory_order_relaxed);
    atomic_store_explicit(&synth->patch_words[1], (unsigned)patch->lfo_target, memory_order_relaxed);
    const float values[SYNTH_PATCH_FLOATS] = {
        patch->wavetable_position, patch->detune_cents, patch->gain, patch->pan,
        patch->filter_cutoff_hz, patch->filter_resonance, patch->attack_s,
        patch->decay_s, patch->sustain, patch->release_s, patch->lfo_hz, patch->lfo_depth,
    };
    for (size_t i = 0; i < SYNTH_PATCH_FLOATS; ++i) {
        atomic_store_explicit(&synth->patch_words[i + 2U], float_word(values[i]), memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&synth->patch_sequence, 1U, memory_order_release);
}

bool app_synth_get_patch(const app_synth_t *synth, app_synth_patch_t *out_patch)
{
    if (synth == NULL || out_patch == NULL) {
        return false;
    }
    for (unsigned attempt = 0; attempt < 4U; ++attempt) {
        unsigned before = atomic_load_explicit(&synth->patch_sequence, memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        out_patch->waveform = (app_synth_waveform_t)atomic_load_explicit(&synth->patch_words[0], memory_order_relaxed);
        out_patch->lfo_target = (app_synth_lfo_target_t)atomic_load_explicit(&synth->patch_words[1], memory_order_relaxed);
        float values[SYNTH_PATCH_FLOATS];
        for (size_t i = 0; i < SYNTH_PATCH_FLOATS; ++i) {
            values[i] = word_float(atomic_load_explicit(&synth->patch_words[i + 2U], memory_order_relaxed));
        }
        if (before == atomic_load_explicit(&synth->patch_sequence, memory_order_acquire)) {
            out_patch->wavetable_position = values[0];
            out_patch->detune_cents = values[1];
            out_patch->gain = values[2];
            out_patch->pan = values[3];
            out_patch->filter_cutoff_hz = values[4];
            out_patch->filter_resonance = values[5];
            out_patch->attack_s = values[6];
            out_patch->decay_s = values[7];
            out_patch->sustain = values[8];
            out_patch->release_s = values[9];
            out_patch->lfo_hz = values[10];
            out_patch->lfo_depth = values[11];
            return true;
        }
    }
    return false;
}

static bool enqueue(app_synth_t *synth, const synth_command_t *command)
{
    if (atomic_flag_test_and_set_explicit(&synth->queue_lock, memory_order_acquire)) {
        atomic_fetch_add_explicit(&synth->dropped, 1U, memory_order_relaxed);
        return false;
    }
    bool result = synth->count < APP_SYNTH_COMMAND_CAPACITY;
    if (result) {
        synth->commands[synth->head] = *command;
        synth->head = (synth->head + 1U) % APP_SYNTH_COMMAND_CAPACITY;
        ++synth->count;
        atomic_store_explicit(&synth->queue_depth, synth->count, memory_order_relaxed);
        if (command->type == CMD_PATCH) {
            store_patch_snapshot(synth, &command->patch);
        }
    } else {
        atomic_fetch_add_explicit(&synth->dropped, 1U, memory_order_relaxed);
    }
    atomic_flag_clear_explicit(&synth->queue_lock, memory_order_release);
    return result;
}

bool app_synth_set_patch(app_synth_t *synth, const app_synth_patch_t *patch)
{
    if (synth == NULL || patch == NULL) {
        return false;
    }
    synth_command_t command = {.type = CMD_PATCH, .patch_id = APP_SYNTH_PATCH_COUNT};
    command.patch = sanitize_patch(patch);
    return enqueue(synth, &command);
}

bool app_synth_set_patch_id(app_synth_t *synth, app_synth_patch_id_t id)
{
    if (synth == NULL || id < APP_SYNTH_PATCH_INIT || id >= APP_SYNTH_PATCH_COUNT) {
        return false;
    }
    synth_command_t command = {.type = CMD_PATCH, .patch_id = id};
    command.patch = app_synth_patch_for_preset(id);
    return enqueue(synth, &command);
}

bool app_synth_note_on(app_synth_t *synth, int note, float velocity)
{
    if (synth == NULL || note < APP_SYNTH_MIN_NOTE || note > APP_SYNTH_MAX_NOTE || !isfinite(velocity)) {
        return false;
    }
    if (velocity <= 0.0f) {
        return app_synth_note_off(synth, note);
    }
    synth_command_t command = {.type = CMD_NOTE_ON, .note = note,
                               .velocity = clampf(velocity, 0.0f, 1.0f)};
    return enqueue(synth, &command);
}

bool app_synth_note_off(app_synth_t *synth, int note)
{
    if (synth == NULL || note < APP_SYNTH_MIN_NOTE || note > APP_SYNTH_MAX_NOTE) {
        return false;
    }
    synth_command_t command = {.type = CMD_NOTE_OFF, .note = note};
    if (!enqueue(synth, &command)) {
        atomic_fetch_or_explicit(&synth->release_mask[(unsigned)note / 32U],
                                 1U << ((unsigned)note % 32U), memory_order_release);
    }
    return true;
}

void app_synth_all_notes_off(app_synth_t *synth)
{
    if (synth != NULL) {
        synth_command_t command = {.type = CMD_ALL_OFF};
        if (!enqueue(synth, &command)) {
            atomic_store_explicit(&synth->emergency_all_off, true, memory_order_release);
        }
    }
}

void app_synth_panic(app_synth_t *synth)
{
    if (synth != NULL) {
        atomic_store_explicit(&synth->emergency_panic, true, memory_order_release);
    }
}

static void build_tables(app_synth_t *synth)
{
    for (unsigned table = 0; table < SYNTH_TABLE_COUNT; ++table) {
        float peak = 0.0f;
        for (unsigned i = 0; i < APP_SYNTH_WAVETABLE_SIZE; ++i) {
            float phase = SYNTH_TWO_PI * (float)i / (float)APP_SYNTH_WAVETABLE_SIZE;
            float value = sinf(phase);
            if (table == 1U) {
                value += sinf(phase * 3.0f) / 9.0f + sinf(phase * 5.0f) / 25.0f;
            } else if (table == 2U) {
                value = 0.0f;
                for (unsigned harmonic = 1; harmonic <= 8U; ++harmonic) {
                    value += sinf(phase * (float)harmonic) / (float)harmonic;
                }
            } else if (table == 3U) {
                value = 0.0f;
                for (unsigned harmonic = 1; harmonic <= 15U; harmonic += 2U) {
                    value += sinf(phase * (float)harmonic) / (float)harmonic;
                }
            }
            synth->tables[table][i] = value;
            peak = fmaxf(peak, fabsf(value));
        }
        for (unsigned i = 0; i < APP_SYNTH_WAVETABLE_SIZE; ++i) {
            synth->tables[table][i] /= peak;
        }
        synth->tables[table][APP_SYNTH_WAVETABLE_SIZE] = synth->tables[table][0];
    }
}

app_synth_t *app_synth_create(float sample_rate)
{
    if (!isfinite(sample_rate) || sample_rate < SYNTH_MIN_RATE || sample_rate > SYNTH_MAX_RATE) {
        return NULL;
    }
    app_synth_t *synth = calloc(1, sizeof(*synth));
    if (synth == NULL) {
        return NULL;
    }
    synth->sample_rate = sample_rate;
    synth->smooth_coefficient = 1.0f - expf(-1.0f / (SYNTH_SMOOTH_SECONDS * sample_rate));
    synth->target = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
    synth->current = synth->target;
    synth->preset_id = APP_SYNTH_PATCH_INIT;
    synth->wave_mix[APP_SYNTH_WAVE_SAW] = 1.0f;
    atomic_flag_clear(&synth->queue_lock);
    atomic_init(&synth->queue_depth, 0U);
    atomic_init(&synth->dropped, 0U);
    atomic_init(&synth->emergency_all_off, false);
    atomic_init(&synth->emergency_panic, false);
    atomic_init(&synth->patch_sequence, 0U);
    atomic_init(&synth->status_sequence, 0U);
    atomic_init(&synth->status_frames_low, 0U);
    atomic_init(&synth->status_frames_high, 0U);
    atomic_init(&synth->status_active, 0U);
    atomic_init(&synth->status_stolen, 0U);
    atomic_init(&synth->status_emergencies, 0U);
    atomic_init(&synth->status_panics, 0U);
    atomic_init(&synth->status_preset, APP_SYNTH_PATCH_INIT);
    atomic_init(&synth->status_peak, 0U);
    atomic_init(&synth->status_clipped, false);
    for (unsigned i = 0; i < 4U; ++i) {
        atomic_init(&synth->release_mask[i], 0U);
    }
    for (unsigned i = 0; i < SYNTH_PATCH_FLOATS + 2U; ++i) {
        atomic_init(&synth->patch_words[i], 0U);
    }
    store_patch_snapshot(synth, &synth->target);
    build_tables(synth);
    return synth;
}

void app_synth_destroy(app_synth_t *synth)
{
    free(synth);
}

static void release_voice(app_synth_t *synth, synth_voice_t *voice)
{
    if (!voice->active || !voice->held) {
        return;
    }
    voice->held = false;
    voice->stage = ENV_RELEASE;
    voice->release_step = voice->envelope /
                          fmaxf(1.0f, synth->current.release_s * synth->sample_rate);
}

static void release_note(app_synth_t *synth, int note)
{
    for (unsigned i = 0; i < APP_SYNTH_VOICE_COUNT; ++i) {
        if (note < 0 || synth->voices[i].note == note) {
            release_voice(synth, &synth->voices[i]);
        }
    }
}

static void start_note(app_synth_t *synth, int note, float velocity)
{
    synth_voice_t *voice = NULL;
    for (unsigned i = 0; i < APP_SYNTH_VOICE_COUNT; ++i) {
        synth_voice_t *candidate = &synth->voices[i];
        if (candidate->active && candidate->note == note) {
            voice = candidate;
            break;
        }
        if (!candidate->active && voice == NULL) {
            voice = candidate;
        }
    }
    if (voice == NULL) {
        voice = &synth->voices[0];
        for (unsigned i = 1; i < APP_SYNTH_VOICE_COUNT; ++i) {
            synth_voice_t *candidate = &synth->voices[i];
            /* Prefer released/quiet voices, then the oldest held voice. */
            if ((!candidate->held && voice->held) ||
                    (candidate->held == voice->held &&
                     ((!candidate->held && candidate->envelope < voice->envelope) ||
                      (candidate->held && candidate->age < voice->age)))) {
                voice = candidate;
            }
        }
        ++synth->stolen;
    }
    memset(voice, 0, sizeof(*voice));
    voice->active = true;
    voice->held = true;
    voice->note = (uint8_t)note;
    voice->velocity = velocity;
    voice->age = ++synth->next_age;
    voice->stage = ENV_ATTACK;
    voice->amp_mod = 1.0f;
    synth->control_countdown = 0U;
}

static void consume_commands(app_synth_t *synth)
{
    if (atomic_flag_test_and_set_explicit(&synth->queue_lock, memory_order_acquire)) {
        /* A producer holds the ring briefly. Keep emergency flags pending until
         * the older queued note-ons can be consumed, so they cannot resurrect a
         * released note in the next block. Panic silences this block immediately. */
        if (atomic_load_explicit(&synth->emergency_panic, memory_order_acquire)) {
            memset(synth->voices, 0, sizeof(synth->voices));
        }
        return;
    }
    for (unsigned i = 0; i < APP_SYNTH_COMMAND_CAPACITY && synth->count != 0U; ++i) {
        const synth_command_t *command = &synth->commands[synth->tail];
        switch (command->type) {
        case CMD_PATCH:
            synth->target = command->patch;
            synth->preset_id = command->patch_id;
            break;
        case CMD_NOTE_ON:
            start_note(synth, command->note, command->velocity);
            break;
        case CMD_NOTE_OFF:
            release_note(synth, command->note);
            break;
        case CMD_ALL_OFF:
            release_note(synth, -1);
            break;
        case CMD_PANIC:
            break;
        }
        synth->tail = (synth->tail + 1U) % APP_SYNTH_COMMAND_CAPACITY;
        --synth->count;
    }
    atomic_store_explicit(&synth->queue_depth, synth->count, memory_order_relaxed);
    atomic_flag_clear_explicit(&synth->queue_lock, memory_order_release);
    for (unsigned word = 0; word < 4U; ++word) {
        uint32_t mask = atomic_exchange_explicit(&synth->release_mask[word], 0U, memory_order_acq_rel);
        if (mask != 0U) {
            ++synth->emergency_count;
            for (unsigned bit = 0; bit < 32U; ++bit) {
                if ((mask & (1U << bit)) != 0U) {
                    release_note(synth, (int)(word * 32U + bit));
                }
            }
        }
    }
    if (atomic_exchange_explicit(&synth->emergency_all_off, false, memory_order_acq_rel)) {
        release_note(synth, -1);
        ++synth->emergency_count;
    }
    if (atomic_exchange_explicit(&synth->emergency_panic, false, memory_order_acq_rel)) {
        memset(synth->voices, 0, sizeof(synth->voices));
        ++synth->panics;
    }
}

static float table_at(const app_synth_t *synth, unsigned table, float phase)
{
    float position = phase * (float)APP_SYNTH_WAVETABLE_SIZE;
    unsigned index = (unsigned)position;
    float fraction = position - (float)index;
    return synth->tables[table][index] + fraction *
           (synth->tables[table][index + 1U] - synth->tables[table][index]);
}

static float poly_blep(float phase, float step)
{
    if (phase < step) {
        float position = phase / step;
        return position + position - position * position - 1.0f;
    }
    if (phase > 1.0f - step) {
        float position = (phase - 1.0f) / step;
        return position * position + position + position + 1.0f;
    }
    return 0.0f;
}

static float oscillator(app_synth_t *synth, synth_voice_t *voice)
{
    float phase = voice->phase;
    float step = voice->phase_step;
    float value = 0.0f;
    if (synth->wave_mix[APP_SYNTH_WAVE_SAW] > 0.00001f) {
        value += synth->wave_mix[APP_SYNTH_WAVE_SAW] * (2.0f * phase - 1.0f - poly_blep(phase, step));
    }
    if (synth->wave_mix[APP_SYNTH_WAVE_SQUARE] > 0.00001f) {
        float half_phase = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
        value += synth->wave_mix[APP_SYNTH_WAVE_SQUARE] *
                 ((phase < 0.5f ? 1.0f : -1.0f) + poly_blep(phase, step) - poly_blep(half_phase, step));
    }
    if (synth->wave_mix[APP_SYNTH_WAVE_TRIANGLE] > 0.00001f) {
        value += synth->wave_mix[APP_SYNTH_WAVE_TRIANGLE] * (1.0f - 4.0f * fabsf(phase - 0.5f));
    }
    if (synth->wave_mix[APP_SYNTH_WAVE_SINE] > 0.00001f) {
        value += synth->wave_mix[APP_SYNTH_WAVE_SINE] * table_at(synth, 0U, phase);
    }
    if (synth->wave_mix[APP_SYNTH_WAVE_WAVETABLE] > 0.00001f) {
        float position = synth->current.wavetable_position * (float)(SYNTH_TABLE_COUNT - 1U);
        unsigned table = (unsigned)position;
        unsigned next = table < SYNTH_TABLE_COUNT - 1U ? table + 1U : table;
        float fraction = position - (float)table;
        float first = table_at(synth, table, phase);
        value += synth->wave_mix[APP_SYNTH_WAVE_WAVETABLE] *
                 (first + fraction * (table_at(synth, next, phase) - first));
    }
    voice->phase += step;
    if (voice->phase >= 1.0f) {
        voice->phase -= 1.0f;
    }
    return value;
}

static void smooth_patch(app_synth_t *synth)
{
#define SMOOTH_FIELD(field) synth->current.field += synth->smooth_coefficient * \
        (synth->target.field - synth->current.field)
    SMOOTH_FIELD(wavetable_position);
    SMOOTH_FIELD(detune_cents);
    SMOOTH_FIELD(gain);
    SMOOTH_FIELD(pan);
    SMOOTH_FIELD(filter_cutoff_hz);
    SMOOTH_FIELD(filter_resonance);
    SMOOTH_FIELD(attack_s);
    SMOOTH_FIELD(decay_s);
    SMOOTH_FIELD(sustain);
    SMOOTH_FIELD(release_s);
    SMOOTH_FIELD(lfo_hz);
    SMOOTH_FIELD(lfo_depth);
#undef SMOOTH_FIELD
    for (unsigned i = 0; i < 5U; ++i) {
        float target_mix = i == (unsigned)synth->target.waveform ? 1.0f : 0.0f;
        synth->wave_mix[i] += synth->smooth_coefficient * (target_mix - synth->wave_mix[i]);
    }
    synth->current.waveform = synth->target.waveform;
    synth->current.lfo_target = synth->target.lfo_target;
}

static void update_voice_control(app_synth_t *synth, synth_voice_t *voice)
{
    float lfo = table_at(synth, 0U, voice->lfo_phase);
    float depth = clampf(synth->current.lfo_depth, 0.0f,
                         synth->current.lfo_target == APP_SYNTH_LFO_TARGET_PITCH ? 12.0f : 1.0f);
    float pitch = synth->current.detune_cents / 100.0f;
    if (synth->current.lfo_target == APP_SYNTH_LFO_TARGET_PITCH) {
        pitch += lfo * depth;
    }
    float frequency = 440.0f * exp2f(((float)voice->note - 69.0f + pitch) / 12.0f);
    voice->phase_step = clampf(frequency / synth->sample_rate, 0.000001f, 0.45f);
    float cutoff = synth->current.filter_cutoff_hz;
    if (synth->current.lfo_target == APP_SYNTH_LFO_TARGET_CUTOFF) {
        cutoff *= 1.0f + lfo * depth;
    }
    cutoff = clampf(cutoff, 20.0f, fminf(20000.0f, synth->sample_rate * 0.45f));
    float g = tanf(SYNTH_PI * cutoff / synth->sample_rate);
    voice->k = 2.0f - 1.8f * synth->current.filter_resonance;
    voice->a1 = 1.0f / (1.0f + g * (g + voice->k));
    voice->a2 = g * voice->a1;
    voice->a3 = g * voice->a2;
    voice->amp_mod = synth->current.lfo_target == APP_SYNTH_LFO_TARGET_AMP ?
                     1.0f - 0.5f * depth * (1.0f + lfo) : 1.0f;
}

static float envelope_step(app_synth_t *synth, synth_voice_t *voice)
{
    switch (voice->stage) {
    case ENV_ATTACK:
        voice->envelope += 1.0f / fmaxf(1.0f, synth->current.attack_s * synth->sample_rate);
        if (voice->envelope >= 1.0f) {
            voice->envelope = 1.0f;
            voice->stage = ENV_DECAY;
        }
        break;
    case ENV_DECAY:
        voice->envelope -= (1.0f - synth->current.sustain) /
                           fmaxf(1.0f, synth->current.decay_s * synth->sample_rate);
        if (voice->envelope <= synth->current.sustain) {
            voice->envelope = synth->current.sustain;
            voice->stage = ENV_SUSTAIN;
        }
        break;
    case ENV_SUSTAIN:
        voice->envelope += synth->smooth_coefficient * (synth->current.sustain - voice->envelope);
        break;
    case ENV_RELEASE:
        voice->envelope -= voice->release_step;
        if (voice->envelope <= SYNTH_ENV_EPSILON) {
            voice->envelope = 0.0f;
            voice->stage = ENV_OFF;
            voice->active = false;
        }
        break;
    default:
        voice->envelope = 0.0f;
        voice->active = false;
        break;
    }
    return voice->envelope;
}

static float render_voice(app_synth_t *synth, synth_voice_t *voice)
{
    float input = oscillator(synth, voice);
    /* Topology-preserving state-variable low-pass. Coefficients are bounded
     * below Nyquist and resonance never reaches self oscillation. */
    float v3 = input - voice->ic2;
    float v1 = voice->a1 * voice->ic1 + voice->a2 * v3;
    float v2 = voice->ic2 + voice->a2 * voice->ic1 + voice->a3 * v3;
    voice->ic1 = 2.0f * v1 - voice->ic1;
    voice->ic2 = 2.0f * v2 - voice->ic2;
    if (!isfinite(v2) || fabsf(voice->ic1) > 64.0f || fabsf(voice->ic2) > 64.0f) {
        voice->ic1 = 0.0f;
        voice->ic2 = 0.0f;
        v2 = 0.0f;
    }
    voice->lfo_phase += synth->current.lfo_hz / synth->sample_rate;
    if (voice->lfo_phase >= 1.0f) {
        voice->lfo_phase -= 1.0f;
    }
    return v2 * envelope_step(synth, voice) * voice->velocity * voice->amp_mod;
}

static void publish_status(app_synth_t *synth, float peak, bool clipped)
{
    unsigned active = 0;
    for (unsigned i = 0; i < APP_SYNTH_VOICE_COUNT; ++i) {
        active += synth->voices[i].active ? 1U : 0U;
    }
    atomic_fetch_add_explicit(&synth->status_sequence, 1U, memory_order_acq_rel);
    atomic_store_explicit(&synth->status_frames_low, (uint32_t)synth->frames, memory_order_relaxed);
    atomic_store_explicit(&synth->status_frames_high, (uint32_t)(synth->frames >> 32U), memory_order_relaxed);
    atomic_store_explicit(&synth->status_active, active, memory_order_relaxed);
    atomic_store_explicit(&synth->status_stolen, synth->stolen, memory_order_relaxed);
    atomic_store_explicit(&synth->status_emergencies, synth->emergency_count, memory_order_relaxed);
    atomic_store_explicit(&synth->status_panics, synth->panics, memory_order_relaxed);
    atomic_store_explicit(&synth->status_preset, (unsigned)synth->preset_id, memory_order_relaxed);
    atomic_store_explicit(&synth->status_peak, float_word(peak), memory_order_relaxed);
    atomic_store_explicit(&synth->status_clipped, clipped, memory_order_relaxed);
    atomic_fetch_add_explicit(&synth->status_sequence, 1U, memory_order_release);
}

void app_synth_render(app_synth_t *synth, float *stereo, size_t frames)
{
    if (stereo == NULL) {
        return;
    }
    if (synth == NULL) {
        memset(stereo, 0, frames * 2U * sizeof(*stereo));
        return;
    }
    consume_commands(synth);
    float peak = 0.0f;
    bool clipped = false;
    for (size_t frame = 0; frame < frames; ++frame) {
        smooth_patch(synth);
        bool update_control = synth->control_countdown == 0U;
        if (update_control) {
            synth->control_countdown = SYNTH_CONTROL_INTERVAL;
        }
        --synth->control_countdown;
        float sample = 0.0f;
        for (unsigned i = 0; i < APP_SYNTH_VOICE_COUNT; ++i) {
            synth_voice_t *voice = &synth->voices[i];
            if (!voice->active) {
                continue;
            }
            if (update_control) {
                update_voice_control(synth, voice);
            }
            sample += render_voice(synth, voice);
        }
        sample *= SYNTH_VOICE_GAIN * synth->current.gain;
        float left = sample * sqrtf(0.5f * (1.0f - synth->current.pan));
        float right = sample * sqrtf(0.5f * (1.0f + synth->current.pan));
        peak = fmaxf(peak, fmaxf(fabsf(left), fabsf(right)));
        clipped = clipped || fabsf(left) > 1.0f || fabsf(right) > 1.0f;
        /* Keep source headroom for the shared output bus. Saturating each
         * source here would distort a mix that the bus later attenuates. */
        stereo[frame * 2U] = isfinite(left) ? left : 0.0f;
        stereo[frame * 2U + 1U] = isfinite(right) ? right : 0.0f;
    }
    synth->frames += frames;
    publish_status(synth, peak, clipped);
}

void app_synth_get_status(const app_synth_t *synth, app_synth_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (synth == NULL) {
        return;
    }
    for (unsigned attempt = 0; attempt < 4U; ++attempt) {
        unsigned before = atomic_load_explicit(&synth->status_sequence, memory_order_acquire);
        if ((before & 1U) != 0U) {
            continue;
        }
        out_status->rendered_frames = atomic_load_explicit(&synth->status_frames_low, memory_order_relaxed) |
                                      ((uint64_t)atomic_load_explicit(&synth->status_frames_high, memory_order_relaxed) << 32U);
        out_status->active_voices = atomic_load_explicit(&synth->status_active, memory_order_relaxed);
        out_status->stolen_voices = atomic_load_explicit(&synth->status_stolen, memory_order_relaxed);
        out_status->emergency_releases = atomic_load_explicit(&synth->status_emergencies, memory_order_relaxed);
        out_status->panic_count = atomic_load_explicit(&synth->status_panics, memory_order_relaxed);
        out_status->patch_id = (app_synth_patch_id_t)atomic_load_explicit(&synth->status_preset, memory_order_relaxed);
        out_status->peak = word_float(atomic_load_explicit(&synth->status_peak, memory_order_relaxed));
        out_status->clipped = atomic_load_explicit(&synth->status_clipped, memory_order_relaxed);
        if (before == atomic_load_explicit(&synth->status_sequence, memory_order_acquire)) {
            break;
        }
    }
    out_status->queue_depth = atomic_load_explicit(&synth->queue_depth, memory_order_relaxed);
    out_status->dropped_commands = atomic_load_explicit(&synth->dropped, memory_order_relaxed);
}
