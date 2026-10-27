// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TRACKER_TRANSITION_FULL UINT16_MAX
#define TRACKER_TRANSITION_PAGE_MS 200U
#define TRACKER_TRANSITION_DRAWER_MS 180U
#define TRACKER_TRANSITION_MAX_MS 60000U

/* Owned by the UI thread. Values are normalized 0..65535; sampling advances
 * current monotonically toward to, including when the wall clock moves back.
 * This state owns no image buffers and performs no allocations. */
typedef struct {
    uint64_t start_ms;
    uint64_t last_ms;
    uint32_t duration_ms;
    uint16_t from;
    uint16_t to;
    uint16_t current;
    bool active;
} tracker_transition_t;

void tracker_transition_init(tracker_transition_t *transition, uint16_t value);
void tracker_transition_begin(tracker_transition_t *transition,
                              uint16_t from, uint16_t to,
                              uint64_t now_ms, uint32_t duration_ms);
uint16_t tracker_transition_sample(tracker_transition_t *transition,
                                   uint64_t now_ms);
/* Samples the old motion first, then starts from that exact current value.
 * A page change also needs the caller to capture the displayed composite as
 * its old image, then begin a fresh 0..FULL slide. */
void tracker_transition_retarget(tracker_transition_t *transition, uint16_t to,
                                 uint64_t now_ms, uint32_t duration_ms);
/* Nearest integer pixels for normalized progress; extent<=0 returns zero. */
int tracker_transition_pixels(uint16_t value, int extent);

/* Every image holds width*frame_height RGB565 pixels. Only rows
 * [0,viewport_height) are written; footer rows and source images are untouched.
 * direction>0 slides old left/new in from right; direction<0 reverses it.
 * dest==new_frame is supported, old_frame must be a distinct complete snapshot.
 * Other partial image overlaps are not supported. Invalid arguments return
 * false without writes. Copies use row memmove/memcpy, no pixel blending. */
bool tracker_transition_slide_rgb565(uint16_t *dest,
                                     const uint16_t *old_frame,
                                     const uint16_t *new_frame,
                                     int width, int frame_height,
                                     int viewport_height, uint16_t progress,
                                     int direction);

/* The current page moves up and the destination enters from below. Uses the
 * same frame/alias contract as slide_rgb565; only the viewport rows move. */
bool tracker_transition_slide_up_rgb565(uint16_t *dest,
                                        const uint16_t *old_frame,
                                        const uint16_t *new_frame,
                                        int width, int frame_height,
                                        int viewport_height, uint16_t progress);

/* Slides the menu panel down from y=-panel_height to y=0. full_menu contains
 * the completely rendered panel in its top panel_height rows. base_frame is
 * the independent background snapshot. dest==full_menu is supported: the
 * panel moves first, then uncovered rows are restored from base and dimmed.
 * max_dim is 0..255 of black /256 (192 is 75%); dim grows with open_amount.
 * Rows >=viewport_height stay untouched. Zero panel height is valid (dim only).
 * Only exact dest/full_menu aliasing is supported; dest/base must be distinct. */
bool tracker_transition_reveal_top_rgb565(uint16_t *dest,
                                          const uint16_t *base_frame,
                                          const uint16_t *full_menu,
                                          int width, int frame_height,
                                          int viewport_height, int panel_height,
                                          uint16_t open_amount,
                                          uint8_t max_dim);
