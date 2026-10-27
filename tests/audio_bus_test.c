// SPDX-License-Identifier: Apache-2.0
/* Host-only integration test. The generated S3M fixture has no external asset
 * or copyright dependency and deliberately overloads the tracker source. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_audio_bus.h"

static unsigned char fixture[1024];
static int16_t output[4096 * 2];
static int16_t reference[4096 * 2];
static float floating[4096 * 2];
static int16_t history[APP_AUDIO_SCOPE_SAMPLES];
static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void make_fixture(int channels)
{
    memset(fixture, 0, sizeof(fixture));
    memcpy(fixture + 0x2c, "SCRM", 4);
    fixture[0x20] = 1;
    fixture[0x22] = 1;
    fixture[0x24] = 1;
    fixture[0x30] = 64;
    fixture[0x31] = 6;
    fixture[0x32] = 125;
    memset(fixture + 0x40, 0xff, 32);
    for (int channel = 0; channel < channels; ++channel) {
        fixture[0x40 + channel] = (unsigned char)channel;
    }
    fixture[0x61] = 0x10; /* Instrument header at 0x100. */
    fixture[0x63] = 0x18; /* Pattern at 0x180. */
    fixture[0x100] = 1;
    fixture[0x10e] = 0x30; /* Signed constant sample at 0x300. */
    fixture[0x110] = 16;
    fixture[0x118] = 16;
    fixture[0x11c] = 64;
    fixture[0x11f] = 1;
    fixture[0x120] = 0x80; /* c2 speed = 0xbb80 = 48000. */
    fixture[0x121] = 0xbb;
    memset(fixture + 0x300, 127, 16);
    size_t cursor = 0x182;
    for (int channel = 0; channel < channels; ++channel) {
        fixture[cursor++] = (unsigned char)(0x20 | channel);
        fixture[cursor++] = 0x40; /* C4. */
        fixture[cursor++] = 1;
    }
    const unsigned packed_bytes = (unsigned)(cursor - 0x180U) + 64U;
    fixture[0x180] = (unsigned char)packed_bytes;
    fixture[0x181] = (unsigned char)(packed_bytes >> 8);
}

static void float_and_saturation(void)
{
    make_fixture(16);
    tracker_engine_t *tracker = tracker_engine_create(fixture, sizeof(fixture));
    CHECK(tracker != NULL);
    if (tracker == NULL) {
        return;
    }
    tracker_engine_play(tracker);
    tracker_engine_render_float(tracker, floating, 512);
    CHECK(floating[0] > 1.5f); /* This source must reach the bus unclipped. */
    tracker_engine_restart(tracker);
    app_audio_bus_t *bus = app_audio_bus_create(tracker, NULL);
    CHECK(bus != NULL);
    if (bus == NULL) {
        tracker_engine_destroy(tracker);
        return;
    }
    app_audio_bus_render(bus, output, 512);
    CHECK(output[0] == INT16_MAX);
    app_audio_bus_status_t status;
    app_audio_bus_get_status(bus, &status);
    CHECK(status.master_peak > 1.5f && status.master_level == 255);
    CHECK(status.frames == 512 && status.clipped);
    CHECK(app_audio_bus_consume_clip(bus));
    CHECK(!app_audio_bus_consume_clip(bus));
    CHECK(app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_TRACKER, 0.25f));
    app_audio_bus_render(bus, output, 512);
    CHECK(output[0] > 12000); /* Would be <=8192 after incorrect pre-clipping. */
    CHECK(abs((int)output[0] - (int)(floating[0] * 0.25f * 32767.0f)) <= 1);
    app_audio_bus_get_status(bus, &status);
    CHECK(status.master_peak < 0.6f && !status.clipped);
    CHECK(!app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_TRACKER, NAN));
    CHECK(!app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_COUNT, 1.0f));
    CHECK(!app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_SYNTH, -1.0f));
    CHECK(!app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_SYNTH, 2.01f));
    CHECK(app_audio_bus_waveform(bus, history, 32) == 32);
    CHECK(history[31] == (int16_t)(((int32_t)output[1022] + output[1023]) / 2));
    CHECK(app_audio_bus_set_source_gain(bus, APP_AUDIO_SOURCE_TRACKER, 0.0f));
    app_audio_bus_render(bus, output, 4096);
    CHECK(app_audio_bus_waveform(bus, history, APP_AUDIO_SCOPE_SAMPLES) ==
          APP_AUDIO_SCOPE_SAMPLES);
    for (int index = 0; index < APP_AUDIO_SCOPE_SAMPLES; ++index) {
        CHECK(history[index] == 0);
    }
    app_audio_bus_destroy(bus);
    tracker_engine_destroy(tracker);
}

static void legacy_and_chunk_equivalence(void)
{
    make_fixture(1);
    tracker_engine_t *a = tracker_engine_create(fixture, sizeof(fixture));
    tracker_engine_t *b = tracker_engine_create(fixture, sizeof(fixture));
    CHECK(a != NULL && b != NULL);
    if (a == NULL || b == NULL) {
        tracker_engine_destroy(a);
        tracker_engine_destroy(b);
        return;
    }
    tracker_engine_play(a);
    tracker_engine_play(b);
    tracker_engine_render(a, reference, 4096);
    tracker_engine_render_float(b, floating, 4096);
    for (int index = 0; index < 4096 * 2; ++index) {
        CHECK(abs((int)reference[index] - (int)(floating[index] * 32767.0f)) <= 1);
    }
    tracker_engine_restart(a);
    tracker_engine_restart(b);
    app_audio_bus_t *bus_a = app_audio_bus_create(a, NULL);
    app_audio_bus_t *bus_b = app_audio_bus_create(b, NULL);
    CHECK(bus_a != NULL && bus_b != NULL);
    if (bus_a != NULL && bus_b != NULL) {
        app_audio_bus_render(bus_a, reference, 1024);
        const int chunks[] = {17, 113, 389, 505};
        int offset = 0;
        for (unsigned index = 0; index < sizeof(chunks) / sizeof(chunks[0]); ++index) {
            app_audio_bus_render(bus_b, output + offset * 2, chunks[index]);
            offset += chunks[index];
        }
        CHECK(memcmp(reference, output, 1024 * 2 * sizeof(int16_t)) == 0);
    }
    app_audio_bus_destroy(bus_a);
    app_audio_bus_destroy(bus_b);
    tracker_engine_destroy(a);
    tracker_engine_destroy(b);
}

static void independent_synth(void)
{
    make_fixture(1);
    tracker_engine_t *tracker = tracker_engine_create(fixture, sizeof(fixture));
    app_synth_t *synth = app_synth_create(APP_AUDIO_SAMPLE_RATE);
    CHECK(tracker != NULL && synth != NULL);
    if (tracker == NULL || synth == NULL) {
        tracker_engine_destroy(tracker);
        app_synth_destroy(synth);
        return;
    }
    app_audio_bus_t *bus = app_audio_bus_create(tracker, synth);
    CHECK(bus != NULL);
    CHECK(app_synth_note_on(synth, 69, 1.0f));
    if (bus != NULL) {
        /* The tracker remains paused; synth must still sound and reach meters. */
        app_audio_bus_render(bus, output, 4096);
        int nonzero = 0;
        for (int index = 0; index < 8192; ++index) {
            nonzero += output[index] != 0;
        }
        CHECK(nonzero > 1000);
        app_audio_bus_status_t status;
        app_audio_bus_get_status(bus, &status);
        CHECK(status.master_peak > 0.01f && status.master_level > 0);
        CHECK(!tracker_engine_is_playing(tracker));
        app_synth_panic(synth);
        app_audio_bus_render(bus, output, 4096);
        for (int index = 0; index < 8192; ++index) {
            CHECK(output[index] == 0);
        }
    }
    app_audio_bus_destroy(bus);
    app_synth_destroy(synth);
    tracker_engine_destroy(tracker);
}

static void tracker_transient_meter(void)
{
    make_fixture(1);
    fixture[0x11f] = 0; /* Sixteen positive samples, then silence (no loop). */
    tracker_engine_t *tracker = tracker_engine_create(fixture, sizeof(fixture));
    CHECK(tracker != NULL);
    if (tracker == NULL) {
        return;
    }
    app_audio_bus_t *bus = app_audio_bus_create(tracker, NULL);
    CHECK(bus != NULL);
    if (bus != NULL) {
        tracker_engine_play(tracker);
        app_audio_bus_render(bus, output, 512);
        CHECK(output[0] > 0 && output[1022] == 0 && output[1023] == 0);
        CHECK(tracker_engine_channel_level(tracker, 0) > 0);
        CHECK(tracker_engine_master_level(tracker) > 0);
    }
    app_audio_bus_destroy(bus);
    tracker_engine_destroy(tracker);
}

static void legacy_gain_edge_monitoring(void)
{
    make_fixture(16);
    tracker_engine_t *tracker = tracker_engine_create(fixture, sizeof(fixture));
    CHECK(tracker != NULL);
    if (tracker == NULL) {
        return;
    }
    tracker_engine_play(tracker);
    tracker_engine_set_channel_gain(tracker, 0, INFINITY);
    CHECK(tracker_engine_channel_gain(tracker, 0) == 2.0f);
    tracker_engine_set_channel_gain(tracker, 0, -INFINITY);
    CHECK(tracker_engine_channel_gain(tracker, 0) == 0.0f);
    for (int channel = 0; channel < 16; ++channel) {
        tracker_engine_set_channel_gain(tracker, channel, NAN);
    }
    CHECK(isnan(tracker_engine_channel_gain(tracker, 0)));
    tracker_engine_render_float(tracker, floating, 512);
    CHECK(isnan(floating[0]) && isnan(floating[1]));
    CHECK(tracker_engine_master_level(tracker) == 0);
    CHECK(tracker_engine_channel_level(tracker, 0) == 0);
    CHECK(!tracker_engine_master_clipped(tracker));
    CHECK(tracker_engine_waveform(tracker, 0, history, APP_AUDIO_SCOPE_SAMPLES) ==
          APP_AUDIO_SCOPE_SAMPLES);
    CHECK(history[APP_AUDIO_SCOPE_SAMPLES - 1] == -32767);
    app_audio_bus_t *bus = app_audio_bus_create(tracker, NULL);
    CHECK(bus != NULL);
    if (bus != NULL) {
        app_audio_bus_render(bus, output, 512);
        CHECK(output[0] == 0 && output[1023] == 0);
        CHECK(app_audio_bus_consume_clip(bus));
    }
    app_audio_bus_destroy(bus);
    tracker_engine_destroy(tracker);
}

int main(void)
{
    CHECK(app_audio_bus_create(NULL, NULL) == NULL);
    memset(output, 0x7f, sizeof(output));
    app_audio_bus_render(NULL, output, 512);
    CHECK(output[0] == 0 && output[1023] == 0);
    float_and_saturation();
    legacy_and_chunk_equivalence();
    independent_synth();
    tracker_transient_meter();
    legacy_gain_edge_monitoring();
    printf("audio bus: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
