// SPDX-License-Identifier: Apache-2.0
#include "tracker_transition.h"

#include <stddef.h>
#include <string.h>

void tracker_transition_init(tracker_transition_t *transition, uint16_t value)
{
    if (transition != NULL) {
        memset(transition, 0, sizeof(*transition));
        transition->from = value;
        transition->to = value;
        transition->current = value;
    }
}

void tracker_transition_begin(tracker_transition_t *transition,
                              uint16_t from, uint16_t to,
                              uint64_t now_ms, uint32_t duration_ms)
{
    if (transition == NULL) {
        return;
    }
    transition->start_ms = now_ms;
    transition->last_ms = now_ms;
    transition->duration_ms = duration_ms > TRACKER_TRANSITION_MAX_MS
                              ? TRACKER_TRANSITION_MAX_MS : duration_ms;
    transition->from = from;
    transition->to = to;
    transition->active = from != to && duration_ms != 0;
    transition->current = transition->active ? from : to;
}

uint16_t tracker_transition_sample(tracker_transition_t *transition,
                                   uint64_t now_ms)
{
    if (transition == NULL) {
        return 0;
    }
    if (now_ms > transition->last_ms) {
        transition->last_ms = now_ms;
    }
    if (!transition->active) {
        return transition->current;
    }
    const uint64_t elapsed = transition->last_ms - transition->start_ms;
    if (elapsed >= transition->duration_ms) {
        transition->current = transition->to;
        transition->active = false;
        return transition->current;
    }
    const uint64_t full = TRACKER_TRANSITION_FULL;
    const uint64_t linear = elapsed * full / transition->duration_ms;
    const uint64_t remaining = full - linear;
    const uint64_t denominator = full * full;
    const uint64_t eased = full -
                           (remaining * remaining * remaining + denominator / 2U) / denominator;
    const int delta = (int)transition->to - (int)transition->from;
    const unsigned distance = (unsigned)(delta < 0 ? -delta : delta);
    const unsigned moved = (unsigned)(((uint64_t)distance * eased + full / 2U) / full);
    transition->current = (uint16_t)(delta < 0
                                     ? (unsigned)transition->from - moved : (unsigned)transition->from + moved);
    return transition->current;
}

void tracker_transition_retarget(tracker_transition_t *transition, uint16_t to,
                                 uint64_t now_ms, uint32_t duration_ms)
{
    if (transition == NULL) {
        return;
    }
    const uint16_t current = tracker_transition_sample(transition, now_ms);
    /* last_ms clamps a regressed clock, so reversal cannot restart in the past. */
    tracker_transition_begin(transition, current, to, transition->last_ms, duration_ms);
}

int tracker_transition_pixels(uint16_t value, int extent)
{
    if (extent <= 0) {
        return 0;
    }
    return (int)(((uint64_t)value * (unsigned)extent + TRACKER_TRANSITION_FULL / 2U) /
                 TRACKER_TRANSITION_FULL);
}

static bool valid_frame(int width, int frame_height, int viewport_height)
{
    return width > 0 && frame_height > 0 && viewport_height >= 0 &&
           viewport_height <= frame_height &&
           (size_t)width <= SIZE_MAX / sizeof(uint16_t) / (size_t)frame_height;
}

bool tracker_transition_slide_rgb565(uint16_t *dest,
                                     const uint16_t *old_frame,
                                     const uint16_t *new_frame,
                                     int width, int frame_height,
                                     int viewport_height, uint16_t progress,
                                     int direction)
{
    if (dest == NULL || old_frame == NULL || new_frame == NULL ||
            dest == old_frame || direction == 0 ||
            !valid_frame(width, frame_height, viewport_height)) {
        return false;
    }
    const size_t offset = (size_t)tracker_transition_pixels(progress, width);
    const size_t retained = (size_t)width - offset;
    const size_t pixel_bytes = sizeof(uint16_t);
    for (int y = 0; y < viewport_height; ++y) {
        uint16_t *out = dest + (size_t)y * (size_t)width;
        const uint16_t *old_row = old_frame + (size_t)y * (size_t)width;
        const uint16_t *new_row = new_frame + (size_t)y * (size_t)width;
        /* Move the new-page part first: when dest==new_frame, copying old
         * first would overwrite pixels the entering page still needs. */
        if (direction > 0) {
            memmove(out + retained, new_row, offset * pixel_bytes);
            memcpy(out, old_row + offset, retained * pixel_bytes);
        } else {
            memmove(out, new_row + retained, offset * pixel_bytes);
            memcpy(out + offset, old_row, retained * pixel_bytes);
        }
    }
    return true;
}

bool tracker_transition_slide_up_rgb565(uint16_t *dest,
                                        const uint16_t *old_frame,
                                        const uint16_t *new_frame,
                                        int width, int frame_height,
                                        int viewport_height, uint16_t progress)
{
    if (dest == NULL || old_frame == NULL || new_frame == NULL ||
            dest == old_frame || !valid_frame(width, frame_height, viewport_height)) {
        return false;
    }
    const size_t rows = (size_t)tracker_transition_pixels(progress, viewport_height);
    const size_t retained = (size_t)viewport_height - rows;
    const size_t line = (size_t)width;
    /* Copy the entering image before replacing any of its source pixels.
     * memmove permits dest==new_frame without a third animation buffer. */
    memmove(dest + retained * line, new_frame, rows * line * sizeof(*dest));
    memcpy(dest, old_frame + rows * line, retained * line * sizeof(*dest));
    return true;
}

bool tracker_transition_expand_center_rgb565(uint16_t *dest,
                                             const uint16_t *old_frame,
                                             const uint16_t *new_frame,
                                             int width, int frame_height,
                                             int viewport_height, uint16_t progress)
{
    if (dest == NULL || old_frame == NULL || new_frame == NULL ||
            dest == old_frame || dest == new_frame || old_frame == new_frame ||
            !valid_frame(width, frame_height, viewport_height)) {
        return false;
    }
    const size_t line = (size_t)width;
    const size_t viewport_bytes = line * (size_t)viewport_height * sizeof(*dest);
    if (progress == TRACKER_TRANSITION_FULL) {
        memcpy(dest, new_frame, viewport_bytes);
        return true;
    }
    memcpy(dest, old_frame, viewport_bytes);
    const int scaled_width = tracker_transition_pixels(progress, width);
    const int scaled_height = tracker_transition_pixels(progress, viewport_height);
    if (scaled_width == 0 || scaled_height == 0) {
        return true;
    }
    const int left = (width - scaled_width) / 2;
    const int top = (viewport_height - scaled_height) / 2;
    for (int y = 0; y < scaled_height; ++y) {
        const size_t source_y = (size_t)((uint64_t)(unsigned)y * (unsigned)viewport_height /
                                         (unsigned)scaled_height);
        const uint16_t *source = new_frame + source_y * line;
        uint16_t *out = dest + (size_t)(top + y) * line + (unsigned)left;
        for (int x = 0; x < scaled_width; ++x) {
            const size_t source_x = (size_t)((uint64_t)(unsigned)x * (unsigned)width /
                                             (unsigned)scaled_width);
            out[x] = source[source_x];
        }
    }
    return true;
}

static uint16_t dim_pixel(uint16_t pixel, unsigned keep)
{
    /* Scale the three channels independently; multiplying packed RGB565 would
     * leak channel carries and change hues. keep is bounded to 1..256. */
    const unsigned red = ((unsigned)(pixel >> 11) * keep) >> 8;
    const unsigned green = (((unsigned)pixel >> 5 & 63U) * keep) >> 8;
    const unsigned blue = (((unsigned)pixel & 31U) * keep) >> 8;
    return (uint16_t)(red << 11 | green << 5 | blue);
}

bool tracker_transition_reveal_top_rgb565(uint16_t *dest,
                                          const uint16_t *base_frame,
                                          const uint16_t *full_menu,
                                          int width, int frame_height,
                                          int viewport_height, int panel_height,
                                          uint16_t open_amount,
                                          uint8_t max_dim)
{
    if (dest == NULL || base_frame == NULL || full_menu == NULL ||
            dest == base_frame || !valid_frame(width, frame_height, viewport_height) ||
            panel_height < 0 || panel_height > viewport_height) {
        return false;
    }
    const size_t shown = (size_t)tracker_transition_pixels(open_amount, panel_height);
    const size_t line_pixels = (size_t)width;
    const size_t shown_pixels = shown * line_pixels;
    memmove(dest, full_menu + ((size_t)panel_height - shown) * line_pixels,
            shown_pixels * sizeof(uint16_t));
    const unsigned dark = (unsigned)(((uint64_t)open_amount * max_dim +
                                      TRACKER_TRANSITION_FULL / 2U) / TRACKER_TRANSITION_FULL);
    const size_t tail_pixels = ((size_t)viewport_height - shown) * line_pixels;
    if (dark == 0) {
        memcpy(dest + shown_pixels, base_frame + shown_pixels,
               tail_pixels * sizeof(uint16_t));
    } else {
        const unsigned keep = 256U - dark;
        for (size_t index = shown_pixels;
                index < (size_t)viewport_height * line_pixels; ++index) {
            dest[index] = dim_pixel(base_frame[index], keep);
        }
    }
    return true;
}
