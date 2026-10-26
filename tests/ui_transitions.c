// SPDX-License-Identifier: Apache-2.0
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_transition.h"

#define GUARD_PIXELS 16U
#define GUARD_VALUE 0xa55aU
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ui_transitions:%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef struct {
    int width;
    int height;
    size_t count;
    uint16_t *old_storage;
    uint16_t *new_storage;
    uint16_t *out_storage;
    uint16_t *snapshot;
    uint16_t *old;
    uint16_t *next;
    uint16_t *out;
} images_t;

static uint16_t old_pixel(size_t index)
{
    return (uint16_t)((index * 73U + 0x1234U) & UINT16_MAX);
}

static uint16_t new_pixel(size_t index)
{
    return (uint16_t)((index * 151U + 0x9876U) & UINT16_MAX);
}

static images_t *images_create(int width, int height)
{
    images_t *images = calloc(1, sizeof(*images));
    CHECK(images != NULL);
    images->width = width;
    images->height = height;
    images->count = (size_t)width * (size_t)height;
    const size_t bytes = (images->count + GUARD_PIXELS * 2U) * sizeof(uint16_t);
    images->old_storage = malloc(bytes);
    images->new_storage = malloc(bytes);
    images->out_storage = malloc(bytes);
    images->snapshot = malloc(images->count * sizeof(uint16_t));
    CHECK(images->old_storage && images->new_storage && images->out_storage && images->snapshot);
    images->old = images->old_storage + GUARD_PIXELS;
    images->next = images->new_storage + GUARD_PIXELS;
    images->out = images->out_storage + GUARD_PIXELS;
    for (size_t index = 0; index < images->count + GUARD_PIXELS * 2U; ++index) {
        images->old_storage[index] = GUARD_VALUE;
        images->new_storage[index] = GUARD_VALUE;
        images->out_storage[index] = GUARD_VALUE;
    }
    for (size_t index = 0; index < images->count; ++index) {
        images->old[index] = old_pixel(index);
        images->next[index] = new_pixel(index);
        images->out[index] = GUARD_VALUE;
    }
    return images;
}

static void guards(const images_t *images)
{
    for (size_t i = 0; i < GUARD_PIXELS; ++i) {
        CHECK(images->old_storage[i] == GUARD_VALUE);
        CHECK(images->new_storage[i] == GUARD_VALUE);
        CHECK(images->out_storage[i] == GUARD_VALUE);
        CHECK(images->old_storage[GUARD_PIXELS + images->count + i] == GUARD_VALUE);
        CHECK(images->new_storage[GUARD_PIXELS + images->count + i] == GUARD_VALUE);
        CHECK(images->out_storage[GUARD_PIXELS + images->count + i] == GUARD_VALUE);
    }
    for (size_t i = 0; i < images->count; ++i) {
        CHECK(images->old[i] == old_pixel(i));
    }
}

static void images_destroy(images_t *images)
{
    guards(images);
    free(images->old_storage);
    free(images->new_storage);
    free(images->out_storage);
    free(images->snapshot);
    free(images);
}

static void test_timing(void)
{
    tracker_transition_t motion;
    tracker_transition_init(&motion, 0);
    CHECK(!motion.active && tracker_transition_sample(&motion, 100) == 0);
    tracker_transition_begin(&motion, 0, TRACKER_TRANSITION_FULL, 1000, 200);
    CHECK(motion.active && tracker_transition_sample(&motion, 1000) == 0);
    uint16_t previous = 0;
    for (unsigned elapsed = 0; elapsed <= 200; ++elapsed) {
        const uint16_t value = tracker_transition_sample(&motion, 1000U + elapsed);
        CHECK(value >= previous);
        if (elapsed == 100) {
            CHECK(value >= 57340 && value <= 57344); /* Cubic ease-out at half time: 87.5%. */
        }
        previous = value;
    }
    CHECK(!motion.active && previous == TRACKER_TRANSITION_FULL);
    CHECK(tracker_transition_sample(&motion, UINT64_MAX) == TRACKER_TRANSITION_FULL);
    tracker_transition_begin(&motion, TRACKER_TRANSITION_FULL, 0, 1000, 180);
    previous = TRACKER_TRANSITION_FULL;
    for (unsigned elapsed = 0; elapsed <= 180; ++elapsed) {
        const uint16_t value = tracker_transition_sample(&motion, 1000U + elapsed);
        CHECK(value <= previous);
        previous = value;
    }
    CHECK(previous == 0 && !motion.active);
    tracker_transition_begin(&motion, 0, TRACKER_TRANSITION_FULL, 1000, 200);
    const uint16_t before_back = tracker_transition_sample(&motion, 1080);
    CHECK(tracker_transition_sample(&motion, 1050) == before_back);
    tracker_transition_retarget(&motion, 0, 1040, 180);
    CHECK(motion.current == before_back && motion.start_ms == 1080);
    CHECK(tracker_transition_sample(&motion, 1080) == before_back);
    CHECK(tracker_transition_sample(&motion, 1260) == 0 && !motion.active);
    tracker_transition_begin(&motion, 100, 500, UINT64_MAX - 40U, 40);
    CHECK(tracker_transition_sample(&motion, UINT64_MAX) == 500);
    tracker_transition_begin(&motion, 100, 500, 1000, 0);
    CHECK(!motion.active && motion.current == 500);
    tracker_transition_begin(&motion, 42, 42, 1000, 180);
    CHECK(!motion.active && motion.current == 42);
    tracker_transition_begin(&motion, 0, 1000, 1000, UINT32_MAX);
    CHECK(motion.duration_ms == TRACKER_TRANSITION_MAX_MS);
    CHECK(tracker_transition_pixels(0, 480) == 0);
    CHECK(tracker_transition_pixels(TRACKER_TRANSITION_FULL, 480) == 480);
    CHECK(tracker_transition_pixels(TRACKER_TRANSITION_FULL, INT_MAX) == INT_MAX);
    CHECK(tracker_transition_pixels(100, -1) == 0);
    tracker_transition_init(NULL, 0);
    tracker_transition_begin(NULL, 0, 1, 0, 1);
    tracker_transition_retarget(NULL, 1, 0, 1);
    CHECK(tracker_transition_sample(NULL, 0) == 0);
    puts("ui transitions: endpoints, cubic easing, reversal, clock rollback/overflow PASS");
}

static void slide_case(images_t *images, int viewport, uint16_t progress,
                        int direction, bool alias)
{
    const size_t width = (size_t)images->width;
    const size_t shift = (size_t)tracker_transition_pixels(progress, images->width);
    for (size_t index = 0; index < images->count; ++index) {
        images->next[index] = new_pixel(index);
        images->out[index] = GUARD_VALUE;
    }
    uint16_t *dest = alias ? images->next : images->out;
    CHECK(tracker_transition_slide_rgb565(dest, images->old, images->next,
                                            images->width, images->height,
                                            viewport, progress, direction));
    for (int y = 0; y < images->height; ++y) {
        for (int x = 0; x < images->width; ++x) {
            const size_t index = (size_t)y * width + (size_t)x;
            uint16_t expected;
            if (y >= viewport) {
                expected = alias ? new_pixel(index) : GUARD_VALUE;
            } else if (direction > 0) {
                const size_t source_x = (size_t)x + shift;
                expected = source_x < width ? old_pixel((size_t)y * width + source_x)
                    : new_pixel((size_t)y * width + source_x - width);
            } else {
                expected = (size_t)x < shift
                    ? new_pixel((size_t)y * width + width - shift + (size_t)x)
                    : old_pixel((size_t)y * width + (size_t)x - shift);
            }
            CHECK(dest[index] == expected);
            if (!alias) {
                CHECK(images->next[index] == new_pixel(index));
            }
        }
    }
    guards(images);
}

static void test_slides(void)
{
    images_t *small = images_create(17, 11);
    for (unsigned progress = 0; progress <= UINT16_MAX; progress += 127U) {
        for (int direction = -1; direction <= 1; direction += 2) {
            slide_case(small, 9, (uint16_t)progress, direction, false);
            slide_case(small, 9, (uint16_t)progress, direction, true);
        }
    }
    slide_case(small, 9, TRACKER_TRANSITION_FULL, 1, true);
    images_destroy(small);
    images_t *full = images_create(480, 480);
    const uint16_t positions[] = {0, 1, 32767, 57343, 65534, TRACKER_TRANSITION_FULL};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        slide_case(full, 432, positions[i], 1, false);
        slide_case(full, 432, positions[i], 1, true);
        slide_case(full, 432, positions[i], -1, false);
        slide_case(full, 432, positions[i], -1, true);
    }
    slide_case(full, 0, 32767, 1, true);
    images_destroy(full);
    puts("ui transitions: bidirectional slides, exact in-place alias, stationary footer, guards PASS");
}

static uint16_t expected_dim(uint16_t pixel, unsigned dark)
{
    const unsigned keep = 256U - dark;
    const unsigned r = (pixel / 2048U) * keep / 256U;
    const unsigned g = (pixel / 32U % 64U) * keep / 256U;
    const unsigned b = (pixel % 32U) * keep / 256U;
    return (uint16_t)(r * 2048U + g * 32U + b);
}

static void drawer_case(images_t *images, int viewport, int panel_height,
                         uint16_t progress, uint8_t dim, bool alias)
{
    const size_t width = (size_t)images->width;
    const int shown = tracker_transition_pixels(progress, panel_height);
    const unsigned dark = (unsigned)(((uint64_t)progress * dim + UINT16_MAX / 2U) / UINT16_MAX);
    for (size_t index = 0; index < images->count; ++index) {
        images->next[index] = new_pixel(index);
        images->out[index] = GUARD_VALUE;
    }
    uint16_t *dest = alias ? images->next : images->out;
    CHECK(tracker_transition_reveal_top_rgb565(dest, images->old, images->next,
                                                images->width, images->height,
                                                viewport, panel_height, progress, dim));
    for (int y = 0; y < images->height; ++y) {
        for (int x = 0; x < images->width; ++x) {
            const size_t index = (size_t)y * width + (size_t)x;
            const uint16_t expected = y >= viewport
                ? alias ? new_pixel(index) : GUARD_VALUE
                : y < shown
                    ? new_pixel(((size_t)panel_height - (size_t)shown + (size_t)y) * width + (size_t)x)
                    : expected_dim(old_pixel(index), dark);
            CHECK(dest[index] == expected);
        }
    }
    guards(images);
}

static void test_drawer(void)
{
    images_t *small = images_create(17, 11);
    for (unsigned progress = 0; progress <= UINT16_MAX; progress += 127U) {
        drawer_case(small, 9, 7, (uint16_t)progress, 192, false);
        drawer_case(small, 9, 7, (uint16_t)progress, 192, true);
    }
    drawer_case(small, 9, 0, TRACKER_TRANSITION_FULL, 255, true);
    drawer_case(small, 0, 0, TRACKER_TRANSITION_FULL, 192, true);
    images_destroy(small);
    images_t *full = images_create(480, 480);
    const uint16_t positions[] = {0, 1, 32767, 57343, 65534, TRACKER_TRANSITION_FULL};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        drawer_case(full, 480, 420, positions[i], 192, false);
        drawer_case(full, 480, 420, positions[i], 192, true);
        drawer_case(full, 432, 420, positions[i], 0, true);
    }
    images_destroy(full);
    puts("ui transitions: vertical drawer translation, reversible reveal, dim RGB isolation PASS");
}

static void test_retarget_capture_and_invalid(void)
{
    images_t *images = images_create(480, 480);
    slide_case(images, 432, 20000, 1, false);
    memcpy(images->snapshot, images->out, images->count * sizeof(uint16_t));
    /* A second navigation captures what was actually displayed, rather than
     * jumping back to either original page before its new slide begins. */
    CHECK(tracker_transition_slide_rgb565(images->next, images->snapshot,
                                            images->next, 480, 480, 432, 0, -1));
    CHECK(memcmp(images->next, images->snapshot, 480U * 432U * sizeof(uint16_t)) == 0);
    memcpy(images->snapshot, images->out, images->count * sizeof(uint16_t));
    CHECK(!tracker_transition_slide_rgb565(NULL, images->old, images->next, 480, 480, 432, 0, 1));
    CHECK(!tracker_transition_slide_rgb565(images->out, images->old, images->next, 480, 480, 481, 0, 1));
    CHECK(!tracker_transition_slide_rgb565(images->out, images->old, images->next, -1, 480, 432, 0, 1));
    CHECK(!tracker_transition_slide_rgb565(images->out, images->old, images->next, 480, 480, 432, 0, 0));
    CHECK(!tracker_transition_slide_rgb565(images->old, images->old, images->next, 480, 480, 432, 0, 1));
    CHECK(!tracker_transition_reveal_top_rgb565(images->out, images->old, images->next,
                                                 480, 480, 432, 433, 0, 192));
    CHECK(!tracker_transition_reveal_top_rgb565(images->old, images->old, images->next,
                                                 480, 480, 432, 420, 0, 192));
    CHECK(memcmp(images->out, images->snapshot, images->count * sizeof(uint16_t)) == 0);
    images_destroy(images);
    puts("ui transitions: rapid page retarget capture and invalid/no-write contracts PASS");
}

int main(void)
{
    test_timing();
    test_slides();
    test_drawer();
    test_retarget_capture_and_invalid();
    puts("portable UI transitions: PASS");
    return 0;
}
