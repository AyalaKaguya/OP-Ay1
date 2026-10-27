// SPDX-License-Identifier: Apache-2.0
/* Production sample-library input/rendering with a real sampler bank. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_workspace.h"
#include "tracker_graphics.h"

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "sampler_library_ui:%d: %s\n", __LINE__, #value); exit(1); \
} } while (0)

/* No project I/O is needed for selecting or scrolling a loaded bank. */
bool tracker_workspace_busy(const tracker_workspace_t *w)
{
    return w->job_waiting;
}
bool tracker_workspace_begin_source_edit(tracker_workspace_t *w)
{
    (void)w;
    return true;
}
void tracker_workspace_end_source_edit(tracker_workspace_t *w, bool changed)
{
    (void)w; (void)changed;
}
void tracker_workspace_request(tracker_workspace_t *w, app_project_action_t action)
{
    (void)w; (void)action;
    CHECK(false);
}
void tracker_workspace_open_import(tracker_workspace_t *w, bool wav)
{
    w->import_return = w->screen;
    w->import_wav = wav;
    w->screen = TRACKER_WORKSPACE_IMPORT;
}
void tracker_workspace_message(tracker_workspace_t *w, const char *message)
{
    snprintf(w->message, sizeof(w->message), "%s", message);
    w->dirty = true;
}
void tracker_workspace_pixel_name(char *out, size_t capacity, const char *input,
                                  unsigned maximum_characters)
{
    size_t count = strlen(input);
    if (count > maximum_characters) {
        count = maximum_characters;
    }
    if (count >= capacity) {
        count = capacity - 1U;
    }
    memcpy(out, input, count);
    out[count] = '\0';
}

static void tap(tracker_workspace_t *w, int x, int y)
{
    tracker_sampler_ui_pointer(w, NULL, x, y, true);
    tracker_sampler_ui_pointer(w, NULL, x, y, false);
}

static void drag(tracker_workspace_t *w, int x1, int y1, int x2, int y2)
{
    tracker_sampler_ui_pointer(w, NULL, x1, y1, true);
    tracker_sampler_ui_pointer(w, NULL, x2, y2, true);
    tracker_sampler_ui_pointer(w, NULL, x2, y2, false);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb"); CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file); CHECK(length > 0);
    rewind(file);
    uint8_t *bytes = malloc((size_t)length); CHECK(bytes != NULL);
    CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    app_sampler_result_t result;
    app_sampler_bank_t *bank = app_sampler_bank_from_s3m(bytes, (size_t)length, &result);
    CHECK(bank != NULL && result == APP_SAMPLER_OK);
    app_sampler_t *sampler = app_sampler_create(); CHECK(sampler != NULL);
    app_sampler_bank_commit(sampler, bank);
    tracker_workspace_t *w = calloc(1, sizeof(*w)); CHECK(w != NULL);
    w->sampler = sampler; w->screen = TRACKER_WORKSPACE_SAMPLER;
    tracker_sampler_ui_poll(w);
    CHECK(w->sample_count == 9 && w->sample == 0 && w->library_scroll == 0);
    CHECK(TRACKER_SAMPLE_LIBRARY_ROW_HEIGHT * 2 >= 45 * 3);
    int16_t expected[TRACKER_SAMPLE_LIBRARY_WAVE_SAMPLES];
    const unsigned points = app_sampler_waveform(sampler, 0, expected, TRACKER_SAMPLE_LIBRARY_WAVE_SAMPLES);
    CHECK(points != 0 && w->library_waveform_counts[0] == points);
    CHECK(memcmp(expected, w->library_waveforms[0], points * sizeof(*expected)) == 0);

    drag(w, 100, 330, 100, 230);
    CHECK(w->library_scroll == 72 && w->sample_first == 1 && w->sample == 0);
    tap(w, 100, TRACKER_SAMPLE_LIBRARY_TOP + 20);
    CHECK(w->sample == 1);
    const unsigned selected = w->sample;
    drag(w, 456, 330, 456, -2000);
    CHECK(w->library_scroll == 9 * 72 - TRACKER_SAMPLE_LIBRARY_HEIGHT);
    CHECK(w->sample == selected && !w->sampler_pointer && !w->library_scrolling);
    CHECK(w->sample_first == 5 && w->library_valid[3] && w->library_rows[3].frames == 0);
    CHECK(w->library_waveform_counts[3] == 0 && !w->library_valid[4]);
    for (unsigned i = 0; i < TRACKER_SAMPLE_LIBRARY_WAVE_SAMPLES; ++i) {
        CHECK(w->library_waveforms[3][i] == 0 && w->library_waveforms[4][i] == 0);
    }
    tap(w, 100, 325);
    CHECK(w->sample == 8 && w->sample_info.frames == 0);
    drag(w, 100, 180, 100, 3000);
    CHECK(w->library_scroll == 0 && w->sample == 8);
    tap(w, 100, TRACKER_SAMPLE_LIBRARY_TOP + 69); /* Four-pixel row spacing is inert. */
    CHECK(w->sample == 8);
    tap(w, 100, TRACKER_SAMPLE_LIBRARY_TOP + 10);
    CHECK(w->sample == 0);
    drag(w, 100, 170, 400, 175);
    CHECK(w->library_scroll == 0 && w->sample == 0);

    /* A partly clipped bottom card keeps its own real waveform, rather than
     * borrowing the row that was visible before the drag crossed its boundary. */
    tracker_sampler_ui_pointer(w, NULL, 100, 300, true);
    tracker_sampler_ui_pointer(w, NULL, 100, 240, true);
    CHECK(w->library_scroll == 60 && w->library_valid[4]);
    const unsigned tail = app_sampler_waveform(sampler, 4, expected, TRACKER_SAMPLE_LIBRARY_WAVE_SAMPLES);
    CHECK(w->library_waveform_counts[4] == tail);
    CHECK(memcmp(expected, w->library_waveforms[4], tail * sizeof(*expected)) == 0);
    tracker_sampler_ui_cancel(w);
    CHECK(!w->sampler_pointer && !w->library_list_pointer && !w->library_scrolling);

    uint16_t *frame = malloc(480U * 480U * sizeof(*frame)); CHECK(frame != NULL);
    for (unsigned i = 0; i < 480U * 480U; ++i) {
        frame[i] = C_BG;
    }
    tracker_sampler_ui_render(frame, w);
    unsigned right_wave_pixels = 0;
    for (int y = TRACKER_SAMPLE_LIBRARY_TOP; y < TRACKER_SAMPLE_LIBRARY_TOP + TRACKER_SAMPLE_LIBRARY_HEIGHT; ++y)
        for (int x = 300; x < 444; ++x)
            if (frame[y * 480 + x] == C_CYAN || frame[y * 480 + x] == C_MUTED) {
                ++right_wave_pixels;
            }
    CHECK(right_wave_pixels != 0);
    /* The former independent lower waveform cannot survive outside list clips. */
    for (int y = 374; y < 382; ++y)
        for (int x = 24; x < 456; ++x) {
            CHECK(frame[y * 480 + x] == C_BG);
        }

    w->sample = 8;
    tracker_sampler_ui_reveal_sample(w);
    tracker_sampler_ui_poll(w);
    CHECK(w->library_scroll == 414 && w->sample_first == 5);
    app_sampler_t *blank = app_sampler_create(); CHECK(blank != NULL);
    app_sampler_bank_t *empty = app_sampler_bank_snapshot(blank); CHECK(empty != NULL);
    app_sampler_destroy(blank);
    app_sampler_bank_commit(sampler, empty);
    tracker_sampler_ui_poll(w);
    CHECK(w->sample_count == 0 && w->library_scroll == 0 && w->sample == 0);
    for (unsigned row = 0; row < TRACKER_SAMPLE_LIBRARY_ROWS; ++row) {
        CHECK(!w->library_valid[row] && w->library_waveform_counts[row] == 0);
    }
    free(frame); free(w); app_sampler_destroy(sampler); free(bytes);
    puts("sampler library: scroll bounds/snap, tap ownership, empty slots, real per-row PCM and stale-cache cleanup PASS");
    return 0;
}
