// SPDX-License-Identifier: Apache-2.0
/* Public tracker lifecycle API and S3M event sequencer. Module decoding and
 * sample mixing are intentionally kept in separate implementation units. */

#include "tracker_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
static int engine_lock_test_and_set(tracker_engine_t *engine)
{
    if (engine->state_lock) {
        return 1;
    }
    engine->state_lock = 1;
    return 0;
}
static void engine_lock_clear(tracker_engine_t *engine) { engine->state_lock = 0; }
static unsigned engine_seq_read(tracker_engine_t *engine) { return engine->status_seq; }
static void engine_seq_begin(tracker_engine_t *engine) { ++engine->status_seq; }
static void engine_set_playing(tracker_engine_t *engine, bool playing)
{
    engine->playing = playing;
}
static bool engine_get_playing(tracker_engine_t *engine) { return engine->playing; }
#else
static int engine_lock_test_and_set(tracker_engine_t *engine)
{
    return __atomic_exchange_n(&engine->state_lock, 1, __ATOMIC_ACQUIRE) != 0;
}
static void engine_lock_clear(tracker_engine_t *engine)
{
    __atomic_store_n(&engine->state_lock, 0, __ATOMIC_RELEASE);
}
static unsigned engine_seq_read(tracker_engine_t *engine)
{
    return __atomic_load_n(&engine->status_seq, __ATOMIC_ACQUIRE);
}
static void engine_seq_begin(tracker_engine_t *engine)
{
    __atomic_add_fetch(&engine->status_seq, 1, __ATOMIC_RELEASE);
}
static void engine_set_playing(tracker_engine_t *engine, bool playing)
{
    __atomic_store_n(&engine->playing, playing, __ATOMIC_RELEASE);
}
static bool engine_get_playing(tracker_engine_t *engine)
{
    return __atomic_load_n(&engine->playing, __ATOMIC_ACQUIRE);
}
#endif

static void engine_lock(tracker_engine_t *engine)
{
    while (engine_lock_test_and_set(engine)) {
    }
}

static void engine_unlock(tracker_engine_t *engine)
{
    engine_lock_clear(engine);
}

static void publish_status(tracker_engine_t *engine)
{
    tracker_status_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.order_index = engine->order_index;
    snapshot.pattern = engine->module.orders[engine->order_index];
    snapshot.row = engine->row;
    snapshot.row_count = TRACKER_PATTERN_ROWS;
    snapshot.patterns = engine->module.pattern_count;
    snapshot.orders = engine->module.order_count;
    snapshot.speed = engine->speed;
    snapshot.tempo = engine->tempo;
    snapshot.channels = engine->module.channel_count;
    snapshot.playing = engine->playing;
    snapshot.frames = engine->frames;
    memcpy(snapshot.title, engine->module.title, sizeof(snapshot.title));

    engine_seq_begin(engine);
    engine->snapshot = snapshot;
    engine_seq_begin(engine);
}

static double note_period(int note)
{
    const int octave = (note >> 4) & 0x0F;
    const int semitone = note & 0x0F;
    return 1712.0 * pow(2.0, (48.0 - (octave * 12 + semitone)) / 12.0);
}

static void update_step(tracker_voice_t *voice)
{
    if (voice->sample != NULL && voice->period > 0.0) {
        voice->step = (float)((double)voice->sample->c2_speed * 1712.0 /
                              (voice->period * TRACKER_SAMPLE_RATE));
    }
}

static int first_playable_order(const tracker_engine_t *engine)
{
    for (int index = 0; index < engine->module.order_count; ++index) {
        const uint8_t pattern = engine->module.orders[index];
        if (pattern != 0xFE && pattern != 0xFF &&
            pattern < engine->module.pattern_count) {
            return index;
        }
    }
    return 0;
}

static void advance_order_locked(tracker_engine_t *engine)
{
    for (int guard = 0; guard < TRACKER_MAX_ORDERS; ++guard) {
        ++engine->order_index;
        if (engine->order_index >= engine->module.order_count) {
            if (!engine->loop) {
                engine->order_index = first_playable_order(engine);
                engine->row = 0;
                engine->tick = 0;
                break;
            }
            engine->order_index = 0;
        }

        const uint8_t pattern = engine->module.orders[engine->order_index];
        if (pattern != 0xFE && pattern != 0xFF &&
            pattern < engine->module.pattern_count) {
            return;
        }
    }

    /* A malformed order list must stop playback, not index outside cells. */
    engine->order_index = first_playable_order(engine);
    engine->row = 0;
    engine->tick = 0;
    engine_set_playing(engine, false);
}

static void next_row_locked(tracker_engine_t *engine)
{
    ++engine->row;
    if (engine->row >= TRACKER_PATTERN_ROWS) {
        engine->row = 0;
        advance_order_locked(engine);
    }
}

static void apply_event(tracker_engine_t *engine, tracker_voice_t *voice,
                        const tracker_cell_data_t *cell, int *new_speed,
                        int *new_tempo)
{
    int effect = cell->effect;
    int param = cell->param;
    if (effect == 4 || effect == 5 || effect == 6 || effect == 7) {
        if (param == 0) {
            param = voice->effect_memory[effect];
        }
        if (param != 0) {
            voice->effect_memory[effect] = (uint8_t)param;
        }
    }

    if (cell->volume != TRACKER_EMPTY && cell->volume <= 64) {
        voice->volume = (float)cell->volume / 64.0f;
    }

    const tracker_sample_t *selected = voice->instrument;
    if (cell->instrument != TRACKER_EMPTY && cell->instrument > 0 &&
        cell->instrument <= engine->module.sample_count) {
        selected = &engine->module.samples[cell->instrument - 1];
        if (selected->data == NULL) {
            selected = NULL;
        }
        voice->instrument = selected;
        if (cell->note == TRACKER_EMPTY && voice->sample != NULL &&
            selected != NULL) {
            voice->sample = selected;
            update_step(voice);
        }
    }

    if (cell->note == 0xFE) {
        voice->sample = NULL;
    } else if (cell->note != TRACKER_EMPTY && selected != NULL) {
        const double target = note_period(cell->note);
        voice->sample = selected;
        if (effect == 7 && voice->step > 0.0f) {
            voice->target_period = target;
        } else {
            voice->period = target;
            voice->target_period = 0.0;
            update_step(voice);
            voice->position = effect == 15 ? (float)param * 256.0f : 0.0f;
            voice->volume = cell->volume == TRACKER_EMPTY ? selected->volume
                                                           : voice->volume;
        }
    }

    if (effect == 1 && param != 0) {
        *new_speed = param;
    } else if (effect == 4) {
        const int high = param >> 4;
        const int low = param & 0x0F;
        if (high == 0x0F) {
            voice->volume = fmaxf(0.0f, voice->volume - (float)low / 64.0f);
        } else if (low == 0x0F) {
            voice->volume = fminf(1.0f, voice->volume + (float)high / 64.0f);
        } else {
            voice->volume_slide = (float)(high - low) / 64.0f;
        }
    } else if (effect == 5) {
        if (param >= 0xF0) {
            voice->period += (param - 0xF0) * 4;
        } else if (param >= 0xE0) {
            voice->period += param - 0xE0;
        } else {
            voice->period_slide = (float)(param * 2);
        }
    } else if (effect == 6) {
        if (param >= 0xF0) {
            voice->period -= (param - 0xF0) * 4;
        } else if (param >= 0xE0) {
            voice->period -= param - 0xE0;
        } else {
            voice->period_slide = -(float)(param * 2);
        }
    } else if (effect == 7) {
        voice->portamento_speed = param;
    } else if (effect == 20 && param >= 32) {
        *new_tempo = param;
    }
}

static void advance_effects(tracker_engine_t *engine)
{
    for (int channel = 0; channel < engine->module.channel_count; ++channel) {
        tracker_voice_t *voice = &engine->mixer.voices[channel];
        voice->volume =
            fminf(1.0f, fmaxf(0.0f, voice->volume + voice->volume_slide));
        if (voice->period_slide != 0.0f) {
            voice->period = fmax(1.0, voice->period + voice->period_slide);
        }
        if (voice->target_period > 0.0 && voice->portamento_speed != 0) {
            const double change = voice->portamento_speed * 2.0;
            if (voice->period < voice->target_period) {
                voice->period =
                    fmin(voice->period + change, voice->target_period);
            } else {
                voice->period =
                    fmax(voice->period - change, voice->target_period);
            }
        }
        update_step(voice);
    }
}

static void start_tick_locked(tracker_engine_t *engine)
{
    int new_speed = 0;
    int new_tempo = 0;
    if (engine->tick == 0) {
        for (int channel = 0; channel < engine->module.channel_count;
             ++channel) {
            tracker_voice_t *voice = &engine->mixer.voices[channel];
            voice->volume_slide = 0.0f;
            voice->period_slide = 0.0f;
            voice->portamento_speed = 0;
        }

        const uint8_t pattern = engine->module.orders[engine->order_index];
        for (int channel = 0; channel < engine->module.channel_count;
             ++channel) {
            const tracker_cell_data_t *cell =
                engine->module.cells +
                ((size_t)pattern * TRACKER_PATTERN_ROWS +
                 (size_t)engine->row) *
                    TRACKER_MAX_CHANNELS +
                channel;
            if (cell->note == TRACKER_EMPTY &&
                cell->instrument == TRACKER_EMPTY &&
                cell->volume == TRACKER_EMPTY &&
                cell->effect == TRACKER_EMPTY) {
                continue;
            }
            apply_event(engine, &engine->mixer.voices[channel], cell, &new_speed,
                        &new_tempo);
        }
    } else {
        advance_effects(engine);
    }

    if (new_speed > 0) {
        engine->speed = new_speed;
    }
    if (new_tempo > 0) {
        engine->tempo = new_tempo;
    }

    const double tempo = (double)engine->tempo * engine->speed_multiplier;
    const double frames = TRACKER_TICK_SECONDS * TRACKER_SAMPLE_RATE /
                          (tempo > 0.0 ? tempo : 1.0);
    int whole = (int)(frames + 0.5);
    if (whole < 1) {
        whole = 1;
    }
    engine->tick_frames_left = whole;
}

static void finish_tick_locked(tracker_engine_t *engine)
{
    ++engine->tick;
    if (engine->tick >= engine->speed) {
        engine->tick = 0;
        next_row_locked(engine);
    }
}

static int peak_level(float peak)
{
    if (peak <= 0.0f) {
        return 0;
    }
    const int level = (int)(peak * 255.0f);
    return level > 255 ? 255 : level;
}

tracker_engine_t *tracker_engine_create(const uint8_t *s3m, size_t size)
{
    tracker_engine_t *engine = calloc(1, sizeof(*engine));
    if (engine == NULL) {
        return NULL;
    }
    if (!tracker_module_parse(&engine->module, s3m, size) ||
        !tracker_mixer_init(&engine->mixer, &engine->module)) {
        tracker_mixer_destroy(&engine->mixer);
        tracker_module_destroy(&engine->module);
        free(engine);
        return NULL;
    }

    engine->speed = engine->module.initial_speed;
    engine->tempo = engine->module.initial_tempo;
    engine->speed_multiplier = 1.0f;
    engine->loop = true;
    engine->order_index = first_playable_order(engine);
    publish_status(engine);
    return engine;
}

void tracker_engine_destroy(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }
    tracker_mixer_destroy(&engine->mixer);
    tracker_module_destroy(&engine->module);
    free(engine);
}

void tracker_engine_play(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }
    engine_lock(engine);
    engine_set_playing(engine, true);
    publish_status(engine);
    engine_unlock(engine);
}

void tracker_engine_pause(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }
    engine_lock(engine);
    engine_set_playing(engine, false);
    tracker_mixer_clear_monitoring(&engine->mixer);
    publish_status(engine);
    engine_unlock(engine);
}

void tracker_engine_restart(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return;
    }
    engine_lock(engine);
    engine->order_index = first_playable_order(engine);
    engine->row = 0;
    engine->tick = 0;
    engine->tick_frames_left = 0;
    engine->frames = 0;
    tracker_mixer_reset(&engine->mixer, &engine->module);
    publish_status(engine);
    engine_unlock(engine);
}

void tracker_engine_set_loop(tracker_engine_t *engine, bool loop)
{
    if (engine == NULL) {
        return;
    }
    engine_lock(engine);
    engine->loop = loop;
    publish_status(engine);
    engine_unlock(engine);
}

bool tracker_engine_loop_enabled(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return false;
    }
    engine_lock(engine);
    const bool loop = engine->loop;
    engine_unlock(engine);
    return loop;
}

bool tracker_engine_is_playing(tracker_engine_t *engine)
{
    return engine != NULL && engine_get_playing(engine);
}

void tracker_engine_set_speed_multiplier(tracker_engine_t *engine,
                                         float multiplier)
{
    if (engine == NULL) {
        return;
    }
    if (multiplier < 0.5f) {
        multiplier = 0.5f;
    } else if (multiplier > 2.0f) {
        multiplier = 2.0f;
    }
    engine_lock(engine);
    engine->speed_multiplier = multiplier;
    engine_unlock(engine);
}

float tracker_engine_speed_multiplier(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return 1.0f;
    }
    engine_lock(engine);
    const float multiplier = engine->speed_multiplier;
    engine_unlock(engine);
    return multiplier;
}

void tracker_engine_render(tracker_engine_t *engine, int16_t *stereo, int frames)
{
    if (engine == NULL || stereo == NULL || frames <= 0) {
        return;
    }

    engine_lock(engine);
    if (!engine_get_playing(engine)) {
        memset(stereo, 0, (size_t)frames * 2 * sizeof(int16_t));
        engine_unlock(engine);
        return;
    }

    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        engine->mixer.channel_peak[channel] = 0.0f;
    }
    engine->mixer.master_peak = 0.0f;

    int produced = 0;
    while (produced < frames) {
        if (engine->tick_frames_left <= 0) {
            start_tick_locked(engine);
            if (engine->tick_frames_left <= 0) {
                break;
            }
        }
        int chunk = frames - produced;
        if (chunk > engine->tick_frames_left) {
            chunk = engine->tick_frames_left;
        }
        tracker_mixer_render(&engine->mixer, &engine->module,
                             stereo + produced * 2, chunk);
        produced += chunk;
        engine->tick_frames_left -= chunk;
        if (engine->tick_frames_left <= 0) {
            finish_tick_locked(engine);
        }
    }
    engine->frames += (uint64_t)frames;
    if (!engine_get_playing(engine)) {
        /* The order list ended without looping: clear monitoring under the lock
         * so a concurrent play() is never overwritten by a late pause. */
        tracker_mixer_clear_monitoring(&engine->mixer);
    }
    publish_status(engine);
    engine_unlock(engine);
}

void tracker_engine_get_status(tracker_engine_t *engine, tracker_status_t *status)
{
    if (engine == NULL || status == NULL) {
        return;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const unsigned before = engine_seq_read(engine);
        if ((before & 1U) != 0) {
            continue;
        }
        *status = engine->snapshot;
        if (engine_seq_read(engine) == before) {
            return;
        }
    }
    *status = engine->snapshot;
}

int tracker_engine_patterns(tracker_engine_t *engine)
{
    return engine != NULL ? engine->module.pattern_count : 0;
}

int tracker_engine_orders(tracker_engine_t *engine)
{
    return engine != NULL ? engine->module.order_count : 0;
}

int tracker_engine_pattern_at(tracker_engine_t *engine, int order_index)
{
    if (engine == NULL || order_index < 0 ||
        order_index >= engine->module.order_count) {
        return 0;
    }
    engine_lock(engine);
    const int pattern = engine->module.orders[order_index];
    engine_unlock(engine);
    return pattern;
}

void tracker_engine_set_pattern_at(tracker_engine_t *engine, int order_index,
                                   int pattern)
{
    if (engine == NULL || engine->module.order_count == 0 ||
        engine->module.pattern_count == 0) {
        return;
    }
    if (order_index < 0) {
        order_index = 0;
    } else if (order_index >= engine->module.order_count) {
        order_index = engine->module.order_count - 1;
    }
    if (pattern < 0) {
        pattern = 0;
    } else if (pattern >= engine->module.pattern_count) {
        pattern = engine->module.pattern_count - 1;
    }

    engine_lock(engine);
    engine->module.orders[order_index] = (uint8_t)pattern;
    publish_status(engine);
    engine_unlock(engine);
}

int tracker_engine_song_rows(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return -1;
    }
    engine_lock(engine);
    int total = 0;
    for (int index = 0; index < engine->module.order_count; ++index) {
        if (engine->module.orders[index] < engine->module.pattern_count) {
            total += TRACKER_PATTERN_ROWS;
        }
    }
    engine_unlock(engine);
    return total;
}

int tracker_engine_current_song_row(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return -1;
    }
    engine_lock(engine);
    int base = 0;
    for (int index = 0; index < engine->order_index; ++index) {
        if (engine->module.orders[index] < engine->module.pattern_count) {
            base += TRACKER_PATTERN_ROWS;
        }
    }
    const int current = base + engine->row;
    engine_unlock(engine);
    return current;
}

bool tracker_engine_song_row_at(tracker_engine_t *engine, int song_row,
                                int *pattern, int *row)
{
    if (engine == NULL || song_row < 0) {
        return false;
    }
    engine_lock(engine);
    bool found = false;
    for (int index = 0; index < engine->module.order_count; ++index) {
        const uint8_t candidate = engine->module.orders[index];
        if (candidate >= engine->module.pattern_count) {
            continue;
        }
        if (song_row < TRACKER_PATTERN_ROWS) {
            if (pattern != NULL) {
                *pattern = candidate;
            }
            if (row != NULL) {
                *row = song_row;
            }
            found = true;
            break;
        }
        song_row -= TRACKER_PATTERN_ROWS;
    }
    engine_unlock(engine);
    return found;
}

void tracker_engine_cell(tracker_engine_t *engine, int pattern, int row,
                         int channel, tracker_cell_t *cell)
{
    if (cell == NULL) {
        return;
    }
    cell->note = -1;
    cell->instrument = 0;
    cell->volume = -1;
    cell->effect = -1;
    cell->param = 0;
    if (engine == NULL || pattern < 0 ||
        pattern >= engine->module.pattern_count || row < 0 ||
        row >= TRACKER_PATTERN_ROWS || channel < 0 ||
        channel >= TRACKER_MAX_CHANNELS ||
        channel >= engine->module.channel_count) {
        return;
    }

    const tracker_cell_data_t *source =
        engine->module.cells +
        ((size_t)pattern * TRACKER_PATTERN_ROWS + (size_t)row) *
            TRACKER_MAX_CHANNELS +
        channel;
    if (source->note != TRACKER_EMPTY) {
        cell->note = source->note;
    }
    if (source->instrument != TRACKER_EMPTY) {
        cell->instrument = source->instrument;
    }
    if (source->volume != TRACKER_EMPTY) {
        cell->volume = source->volume;
    }
    if (source->effect != TRACKER_EMPTY) {
        cell->effect = source->effect;
        cell->param = source->param;
    }
}

int tracker_engine_waveform(tracker_engine_t *engine, int channel,
                            int16_t *out, int count)
{
    if (out == NULL || count <= 0) {
        return 0;
    }
    memset(out, 0, (size_t)count * sizeof(int16_t));
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS ||
        engine->mixer.wave == NULL) {
        return 0;
    }

    const int available =
        count < TRACKER_WAVE_SAMPLES ? count : TRACKER_WAVE_SAMPLES;
    engine_lock(engine);
    const uint16_t head = engine->mixer.wave_head[channel];
    for (int index = 0; index < available; ++index) {
        out[index] = engine->mixer.wave[channel * TRACKER_WAVE_SAMPLES +
                                        (uint16_t)((head + index) %
                                                   TRACKER_WAVE_SAMPLES)];
    }
    engine_unlock(engine);
    return available;
}

float tracker_engine_channel_cycle_frames(tracker_engine_t *engine, int channel)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS) {
        return 0.0f;
    }
    engine_lock(engine);
    const tracker_voice_t *voice = &engine->mixer.voices[channel];
    const tracker_sample_t *sample = voice->sample;
    float cycles = 0.0f;
    if (sample != NULL && voice->step > 0.0f) {
        const uint32_t span =
            sample->loops ? sample->loop_end - sample->loop_start
                          : sample->length;
        if (span > 0) {
            cycles = (float)span / voice->step;
        }
    }
    engine_unlock(engine);
    return cycles;
}

void tracker_engine_set_channel_gain(tracker_engine_t *engine, int channel,
                                     float gain)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS ||
        channel >= engine->module.channel_count) {
        return;
    }
    if (gain < 0.0f) {
        gain = 0.0f;
    } else if (gain > 2.0f) {
        gain = 2.0f;
    }
    engine_lock(engine);
    engine->mixer.channel_gain[channel] = gain;
    engine_unlock(engine);
}

float tracker_engine_channel_gain(tracker_engine_t *engine, int channel)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS) {
        return 0.0f;
    }
    engine_lock(engine);
    const float gain = engine->mixer.channel_gain[channel];
    engine_unlock(engine);
    return gain;
}

void tracker_engine_set_channel_mute(tracker_engine_t *engine, int channel,
                                     bool muted)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS ||
        channel >= engine->module.channel_count) {
        return;
    }
    engine_lock(engine);
    engine->mixer.channel_mute[channel] = muted;
    engine_unlock(engine);
}

bool tracker_engine_channel_mute(tracker_engine_t *engine, int channel)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS) {
        return false;
    }
    engine_lock(engine);
    const bool muted = engine->mixer.channel_mute[channel];
    engine_unlock(engine);
    return muted;
}

void tracker_engine_set_channel_solo(tracker_engine_t *engine, int channel,
                                     bool solo)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS ||
        channel >= engine->module.channel_count) {
        return;
    }
    engine_lock(engine);
    if (engine->mixer.channel_solo[channel] != solo) {
        engine->mixer.channel_solo[channel] = solo;
        engine->mixer.solo_count += solo ? 1 : -1;
    }
    engine_unlock(engine);
}

bool tracker_engine_channel_solo(tracker_engine_t *engine, int channel)
{
    if (engine == NULL || channel < 0 || channel >= TRACKER_MAX_CHANNELS) {
        return false;
    }
    engine_lock(engine);
    const bool solo = engine->mixer.channel_solo[channel];
    engine_unlock(engine);
    return solo;
}

bool tracker_engine_master_clipped(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return false;
    }
    engine_lock(engine);
    const bool clipped = engine->mixer.master_clipped;
    engine_unlock(engine);
    return clipped;
}

bool tracker_engine_consume_master_clip(tracker_engine_t *engine)
{
    if (engine == NULL) {
        return false;
    }
    engine_lock(engine);
    const bool clipped = engine->mixer.master_clipped;
    engine->mixer.master_clipped = false;
    engine_unlock(engine);
    return clipped;
}

int tracker_engine_master_level(tracker_engine_t *engine)
{
    if (engine == NULL || !engine_get_playing(engine)) {
        return 0;
    }
    engine_lock(engine);
    const int level = peak_level(engine->mixer.master_peak);
    engine_unlock(engine);
    return level;
}

int tracker_engine_channel_level(tracker_engine_t *engine, int channel)
{
    if (engine == NULL || !engine_get_playing(engine) || channel < 0 ||
        channel >= TRACKER_MAX_CHANNELS) {
        return 0;
    }
    engine_lock(engine);
    const int level = peak_level(engine->mixer.channel_peak[channel]);
    engine_unlock(engine);
    return level;
}
