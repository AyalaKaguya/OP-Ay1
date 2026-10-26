// SPDX-License-Identifier: Apache-2.0
/* Host renderer regression tests. Build/run through tools/test_ui_renderer.py.
 * argv[1] is the unchanged main/assets/aryx.s3m fixture; no device is accessed.
 * Initial pattern/wave hashes cover RGB565 content in canonical little-endian
 * byte order, independent of native endianness. They are not updated by tests. */

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_graphics.h"
#include "tracker_pages.h"
#include "tracker_render.h"

#define FRAME_PIXELS (480U * 480U)
#define CONTENT_PIXELS (480U * 432U)
#define GUARD_WORDS 32U
#define CANARY UINT16_C(0xdead)
#define FNV_OFFSET UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)
#define EXPECTED_FIXTURE_HASH UINT64_C(0x0211d94f9f6c3f9a)
#define EXPECTED_PATTERN_HASH UINT64_C(0xc4d6c605b39adba6)
#define EXPECTED_WAVES_HASH UINT64_C(0xa524290e1931dbe4)

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "ui_renderer: FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static uint64_t hash_bytes(const uint8_t *data, size_t size)
{
    uint64_t hash = FNV_OFFSET;
    for (size_t index = 0; index < size; ++index) {
        hash = (hash ^ data[index]) * FNV_PRIME;
    }
    return hash;
}

static uint64_t hash_frame(const uint16_t *frame)
{
    uint64_t hash = FNV_OFFSET;
    for (size_t index = 0; index < CONTENT_PIXELS; ++index) {
        hash = (hash ^ (frame[index] & 0xffU)) * FNV_PRIME;
        hash = (hash ^ (frame[index] >> 8)) * FNV_PRIME;
    }
    return hash;
}

static void require_hash(uint64_t actual, uint64_t expected, const char *name)
{
    if (actual != expected) {
        fprintf(stderr, "ui_renderer: %s hash 0x%016" PRIx64
                "; expected 0x%016" PRIx64 "\n", name, actual, expected);
        require(false, "fixture or approved initial rendering changed; review the snapshot");
    }
}

static void reset_frame(uint16_t *allocation)
{
    for (size_t index = 0; index < FRAME_PIXELS + 2U * GUARD_WORDS; ++index) {
        allocation[index] = CANARY;
    }
}

static void require_guards(const uint16_t *allocation)
{
    const uint16_t *frame = allocation + GUARD_WORDS;
    for (size_t index = 0; index < GUARD_WORDS; ++index) {
        require(allocation[index] == CANARY, "write before framebuffer");
        require(frame[FRAME_PIXELS + index] == CANARY, "write after framebuffer");
    }
    for (size_t index = CONTENT_PIXELS; index < FRAME_PIXELS; ++index) {
        require(frame[index] == CANARY, "renderer overwrote navigation footer");
    }
}

static void require_complete(const uint16_t *frame)
{
    for (size_t index = 0; index < CONTENT_PIXELS; ++index) {
        require(frame[index] != CANARY, "renderer left content pixels uninitialized");
    }
}

static void controls_bounds(uint16_t *allocation, tracker_controls_view_t *view,
                             uint16_t *reference)
{
    uint16_t *frame = allocation + GUARD_WORDS;
    view->status = (tracker_status_t) {
        .pattern = 5, .patterns = 12, .order_index = 2, .orders = 44,
        .row = 24, .row_count = 64, .channels = 12, .frames = 624000,
        .speed = 2, .tempo = 95, .playing = true,
    };
    view->volume = 100;
    view->speed_multiplier = 1.0f;
    view->channel_count = 12;
    view->master_level = 72;
    view->pad_playing = 5;
    view->pad_next = 6;
    view->pad_blink = true;
    for (int channel = 0; channel < TRACKER_MAX_CHANNELS; ++channel) {
        view->gain_steps[channel] = 100;
        view->levels[channel] = (uint8_t)(200 - channel * 10);
    }
    for (int page = TRACKER_PAGE_PLAYBACK; page <= TRACKER_PAGE_PADS; ++page) {
        view->page = page;
        reset_frame(allocation);
        tracker_render_controls(frame, view);
        require_guards(allocation);
        require_complete(frame);
    }

    view->page = TRACKER_PAGE_MIXER;
    view->mix_scroll = 0;
    tracker_render_controls(frame, view);
    memcpy(reference, frame, CONTENT_PIXELS * sizeof(*frame));
    const int max_scroll = TRACKER_MIX_STRIPS * TRACKER_MIX_STRIP_H - TRACKER_MIX_VIEW_H;
    for (int scroll = 0; scroll <= max_scroll; ++scroll) {
        view->mix_scroll = scroll;
        reset_frame(allocation);
        tracker_render_controls(frame, view);
        require_guards(allocation);
        require_complete(frame);
        /* Only the visible-channel label changes above the fixed mixer panel.
         * Row glyphs/buttons at a partial offset must not paint over it. */
        require(!memcmp(reference, frame, 33U * 480U * sizeof(*frame)),
                "scrolling rows overwrote the shared status bar");
        require(!memcmp(reference + 69U * 480U, frame + 69U * 480U,
                        (TRACKER_MIX_VIEW_Y - 69U) * 480U * sizeof(*frame)),
                "scrolling rows overwrote the fixed mixer panel");
    }
    printf("ui_renderer: all %d mixer scroll offsets preserve header/footer and guards\n",
           max_scroll + 1);

    /* Invalid offsets must render exactly like their clamped boundary. */
    view->mix_scroll = max_scroll;
    tracker_render_controls(frame, view);
    memcpy(reference, frame, CONTENT_PIXELS * sizeof(*frame));
    view->mix_scroll = INT_MAX;
    tracker_render_controls(frame, view);
    require(!memcmp(reference, frame, CONTENT_PIXELS * sizeof(*frame)),
            "large scroll offset did not clamp to the bottom");
    view->mix_scroll = 0;
    tracker_render_controls(frame, view);
    memcpy(reference, frame, CONTENT_PIXELS * sizeof(*frame));
    view->mix_scroll = INT_MIN;
    tracker_render_controls(frame, view);
    require(!memcmp(reference, frame, CONTENT_PIXELS * sizeof(*frame)),
            "negative scroll offset did not clamp to the top");

    view->page = TRACKER_PAGE_PADS;
    view->pad_stop = true;
    view->pad_next = -1;
    for (int blink = 0; blink <= 1; ++blink) {
        view->pad_blink = blink != 0;
        reset_frame(allocation);
        tracker_render_controls(frame, view);
        require_guards(allocation);
    }
    view->page = TRACKER_PAGE_PLAYBACK;
    view->status.playing = false;
    reset_frame(allocation);
    tracker_render_controls(frame, view);
    require_guards(allocation);
}

static void graphics_bounds(uint16_t *allocation)
{
    uint16_t *frame = allocation + GUARD_WORDS;
    reset_frame(allocation);
    tracker_graphics_t gfx;
    tracker_gfx_init(&gfx, frame);
    tracker_gfx_clip(&gfx, 10, 20, 5, 5);
    tracker_gfx_rect(&gfx, INT_MIN, INT_MIN, INT_MAX, INT_MAX, UINT16_C(0x1234));
    tracker_gfx_rect(&gfx, -100, -100, 10000, 10000, UINT16_C(0x1234));
    for (int y = 0; y < 480; ++y) {
        for (int x = 0; x < 480; ++x) {
            const uint16_t expected = x >= 10 && x < 15 && y >= 20 && y < 25
                                          ? UINT16_C(0x1234) : CANARY;
            require(frame[(size_t)y * 480U + (size_t)x] == expected,
                    "rectangle escaped its explicit clip");
        }
    }
    tracker_gfx_text(&gfx, 7, 16, 2, C_TEXT, "CLIP");
    for (int y = 0; y < 480; ++y) {
        for (int x = 0; x < 480; ++x) {
            if (x < 10 || x >= 15 || y < 20 || y >= 25) {
                require(frame[(size_t)y * 480U + (size_t)x] == CANARY,
                        "partially visible text escaped its clip");
            }
        }
    }
    require_guards(allocation);
    puts("ui_renderer: rectangle and partial-glyph clipping OK");
}

static uint8_t *read_fixture(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    require(file != NULL, "cannot open aryx.s3m fixture (pass its path as argv[1])");
    require(fseek(file, 0, SEEK_END) == 0, "cannot seek fixture");
    const long length = ftell(file);
    require(length > 0, "fixture is empty or unreadable");
    require(fseek(file, 0, SEEK_SET) == 0, "cannot rewind fixture");
    *size = (size_t)length;
    uint8_t *data = malloc(*size);
    require(data != NULL, "fixture allocation failed");
    require(fread(data, 1, *size, file) == *size, "fixture read was incomplete");
    require(fclose(file) == 0, "fixture close failed");
    return data;
}

static void legacy_snapshots(uint16_t *allocation, const char *fixture_path)
{
    uint16_t *frame = allocation + GUARD_WORDS;
    size_t size = 0;
    uint8_t *blob = read_fixture(fixture_path, &size);
    require_hash(hash_bytes(blob, size), EXPECTED_FIXTURE_HASH, "aryx.s3m fixture");
    tracker_engine_t *engine = tracker_engine_create(blob, size);
    require(engine != NULL, "fixture did not create a tracker engine");
    int16_t *scratch = malloc((TRACKER_WAVEFORM_SAMPLES + 2U * GUARD_WORDS) * sizeof(int16_t));
    int16_t *audio = malloc(8192U * 2U * sizeof(int16_t));
    require(scratch != NULL && audio != NULL, "scope/audio allocation failed");

    reset_frame(allocation);
    tracker_render_pattern(frame, engine);
    require_guards(allocation);
    require_complete(frame);
    require_hash(hash_frame(frame), EXPECTED_PATTERN_HASH, "initial pattern");
    reset_frame(allocation);
    tracker_render_waves(frame, engine, scratch + GUARD_WORDS, TRACKER_WAVEFORM_SAMPLES);
    require_guards(allocation);
    require_complete(frame);
    require_hash(hash_frame(frame), EXPECTED_WAVES_HASH, "initial waves");

    /* Exercise live waveform indexing as well as a paused golden frame. */
    tracker_engine_play(engine);
    tracker_engine_render(engine, audio, 8192);
    for (size_t index = 0; index < TRACKER_WAVEFORM_SAMPLES + 2U * GUARD_WORDS; ++index) {
        scratch[index] = (int16_t)0x5a5a;
    }
    reset_frame(allocation);
    tracker_render_waves(frame, engine, scratch + GUARD_WORDS, TRACKER_WAVEFORM_SAMPLES);
    require_guards(allocation);
    for (size_t index = 0; index < GUARD_WORDS; ++index) {
        require(scratch[index] == (int16_t)0x5a5a, "scope wrote before its scratch");
        require(scratch[GUARD_WORDS + TRACKER_WAVEFORM_SAMPLES + index] == (int16_t)0x5a5a,
                "scope wrote after its scratch");
    }
    for (size_t index = 0; index < TRACKER_WAVEFORM_SAMPLES + 2U * GUARD_WORDS; ++index) {
        scratch[index] = (int16_t)0x5a5a;
    }
    tracker_render_waves(frame, engine, scratch + GUARD_WORDS, 2);
    for (size_t index = 0; index < GUARD_WORDS; ++index) {
        require(scratch[index] == (int16_t)0x5a5a, "small scope wrote before its scratch");
        require(scratch[GUARD_WORDS + 2U + index] == (int16_t)0x5a5a,
                "small scope exceeded the supplied two-sample capacity");
    }
    tracker_render_waves(frame, engine, NULL, 0);
    require_guards(allocation);
    free(audio);
    free(scratch);
    tracker_engine_destroy(engine);
    free(blob);
    puts("ui_renderer: initial pattern/waves goldens and live scope bounds OK");
}

int main(int argc, char **argv)
{
    require(argc == 2, "usage: ui_renderer <path/to/aryx.s3m>");
    uint16_t *allocation = malloc((FRAME_PIXELS + 2U * GUARD_WORDS) * sizeof(uint16_t));
    uint16_t *reference = malloc(CONTENT_PIXELS * sizeof(uint16_t));
    tracker_controls_view_t *view = calloc(1, sizeof(*view));
    require(allocation != NULL && reference != NULL && view != NULL, "test allocation failed");
    controls_bounds(allocation, view, reference);
    graphics_bounds(allocation);
    legacy_snapshots(allocation, argv[1]);
    free(view);
    free(reference);
    free(allocation);
    puts("ui_renderer: PASS");
    return EXIT_SUCCESS;
}
