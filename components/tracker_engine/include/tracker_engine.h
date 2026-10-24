// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRACKER_MAX_CHANNELS 16
#define TRACKER_PATTERN_ROWS 64
#define TRACKER_TITLE_BYTES 29
#define TRACKER_WAVEFORM_SAMPLES 2048

typedef struct tracker_engine tracker_engine_t;

typedef struct {
    int16_t note;        /* S3M raw note byte (octave<<4 | semitone); -1 = empty */
    int16_t instrument;  /* 1-based sample number; 0 = empty */
    int16_t volume;      /* 0..64; -1 = empty */
    int16_t effect;      /* S3M effect command; -1 = empty */
    int16_t param;       /* effect parameter 0..255 */
} tracker_cell_t;

typedef struct {
    int order_index;  /* current order-list position */
    int pattern;      /* pattern playing at that order position */
    int row;          /* current row inside the pattern */
    int row_count;    /* rows per pattern (64 for S3M) */
    int patterns;     /* total pattern slots in the module */
    int orders;       /* length of the order list */
    int speed;        /* current S3M speed (ticks per row) */
    int tempo;        /* current S3M tempo (BPM) */
    int channels;     /* active channels, clamped to TRACKER_MAX_CHANNELS */
    bool playing;
    uint64_t frames;  /* rendered frames since create */
    char title[TRACKER_TITLE_BYTES];
} tracker_status_t;

/* Parses the module. Returns NULL when the blob is not a supported S3M. */
tracker_engine_t *tracker_engine_create(const uint8_t *s3m, size_t size);
void tracker_engine_destroy(tracker_engine_t *engine);

void tracker_engine_play(tracker_engine_t *engine);
void tracker_engine_pause(tracker_engine_t *engine);
void tracker_engine_restart(tracker_engine_t *engine);
bool tracker_engine_is_playing(tracker_engine_t *engine);

/* Single-song loop. When disabled, playback stops at the end of the order list
 * and rewinds to the first order so the next play starts from the beginning. */
void tracker_engine_set_loop(tracker_engine_t *engine, bool loop);
bool tracker_engine_loop_enabled(tracker_engine_t *engine);

/* 0.5 .. 2.0; scales the tick duration (tempo * multiplier). */
void tracker_engine_set_speed_multiplier(tracker_engine_t *engine, float multiplier);
float tracker_engine_speed_multiplier(tracker_engine_t *engine);

/* Fills interleaved stereo int16 samples; silence while paused. */
void tracker_engine_render(tracker_engine_t *engine, int16_t *stereo, int frames);

/* Thread-safe snapshot for the UI (writer = audio task). */
void tracker_engine_get_status(tracker_engine_t *engine, tracker_status_t *status);

int tracker_engine_patterns(tracker_engine_t *engine);
int tracker_engine_orders(tracker_engine_t *engine);
int tracker_engine_pattern_at(tracker_engine_t *engine, int order_index);
/* Sequencer edit; order_index and pattern are clamped to valid ranges. */
void tracker_engine_set_pattern_at(tracker_engine_t *engine, int order_index,
                                   int pattern);

/* Pattern pads (clip launcher). The first launch takes over immediately from
 * that pattern's first row and loops it; launching another pattern while one is
 * playing queues the switch for the end of the current pattern (bar aligned).
 * Launching the playing pattern clears a queued switch. stop_pattern_launch()
 * returns to order-list playback; restart() also clears the pad mode.
 * launched_pattern()/pending_pattern() return -1 when inactive. */
void tracker_engine_launch_pattern(tracker_engine_t *engine, int pattern);
void tracker_engine_stop_pattern_launch(tracker_engine_t *engine);
int tracker_engine_launched_pattern(tracker_engine_t *engine);
int tracker_engine_pending_pattern(tracker_engine_t *engine);

void tracker_engine_cell(tracker_engine_t *engine, int pattern, int row,
                         int channel, tracker_cell_t *cell);

/* Song-row helpers: the pattern view scrolls over one continuous row stream so
 * the playback pointer can stay centred across pattern boundaries. A global
 * row counts every playable order entry in sequence. */
int tracker_engine_song_rows(tracker_engine_t *engine);
int tracker_engine_current_song_row(tracker_engine_t *engine);
bool tracker_engine_song_row_at(tracker_engine_t *engine, int song_row,
                                int *pattern, int *row);

/* Recent per-channel output, uniformly decimated, oldest first. */
/* Copies the most recent raw samples (oldest first) and returns the count. */
int tracker_engine_waveform(tracker_engine_t *engine, int channel, int16_t *out,
                            int count);

/* Output frames per sample-loop cycle for a channel, or 0 when silent. */
float tracker_engine_channel_cycle_frames(tracker_engine_t *engine, int channel);

/* Per-channel peak 0..255 for level meters. */
int tracker_engine_channel_level(tracker_engine_t *engine, int channel);

/* Per-track mixer controls. gain is clamped to 0..2.0 (1.0 = unity, 2.0 = the
 * UI fader's +6 dB top); mute and solo affect only the mixed output (the
 * transport keeps running). */
void tracker_engine_set_channel_gain(tracker_engine_t *engine, int channel,
                                     float gain);
float tracker_engine_channel_gain(tracker_engine_t *engine, int channel);
void tracker_engine_set_channel_mute(tracker_engine_t *engine, int channel,
                                     bool muted);
bool tracker_engine_channel_mute(tracker_engine_t *engine, int channel);
void tracker_engine_set_channel_solo(tracker_engine_t *engine, int channel,
                                     bool solo);
bool tracker_engine_channel_solo(tracker_engine_t *engine, int channel);
/* Peak of the final stereo mix of the last render chunk, 0..255. */
int tracker_engine_master_level(tracker_engine_t *engine);
bool tracker_engine_master_clipped(tracker_engine_t *engine);
/* Atomically consumes a clip event from the latest render chunks. */
bool tracker_engine_consume_master_clip(tracker_engine_t *engine);


