// SPDX-License-Identifier: Apache-2.0
/* Production UI owner with real model/render/audio components and a tiny fake
 * GSP callback scheduler. No simulated duplicate of the owner's state machine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_ui.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ui_owner_lifecycle:%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

struct gsp_ui_core {
    int marker;
};
static struct gsp_ui_core fake_ui;
static esp_gsp_canvas_draw_cb_t canvas_callback;
static void *canvas_context;
static esp_gsp_pointer_observer_cb_t pointer_callback;
static void *pointer_context;
static esp_gsp_timer_cb_t timer_callback;
static void *timer_context;
static int fake_timer;

typedef struct {
    tracker_engine_t *tracker;
    app_synth_t *synth;
    app_audio_bus_t *bus;
    app_note_router_t *router;
    app_tracker_config_t config;
    app_settings_snapshot_t display;
    uint64_t clock_ms;
    unsigned allocation_calls;
    unsigned fail_allocation;
    unsigned free_calls;
    void *allocated[3];
    bool freed[3];
    bool force_wake;
    bool busy;
    unsigned activity_calls;
    unsigned fail_gsp_stage;
    bool fail_stop;
    uint16_t *capture;
    uint16_t *expected;
    int16_t audio[1024];
} fixture_t;

static fixture_t *current_fixture;

esp_gsp_err_t esp_gsp_canvas_set_draw_cb(esp_gsp_handle_t ui, uint16_t bind,
                                         esp_gsp_canvas_draw_cb_t callback, void *context)
{
    (void)bind;
    CHECK(ui == &fake_ui);
    if (current_fixture->fail_gsp_stage == 1) {
        return ESP_GSP_ERR_NO_MEM;
    }
    canvas_callback = callback;
    canvas_context = context;
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_canvas_invalidate(esp_gsp_handle_t ui, uint16_t bind)
{
    (void)bind;
    CHECK(ui == &fake_ui);
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_set_pointer_observer(esp_gsp_handle_t ui,
                                           esp_gsp_pointer_observer_cb_t callback,
                                           void *context)
{
    CHECK(ui == &fake_ui);
    if (callback && current_fixture->fail_gsp_stage == 2) {
        return ESP_GSP_ERR_NO_MEM;
    }
    pointer_callback = callback;
    pointer_context = context;
    return ESP_GSP_OK;
}

void *esp_gsp_timer_create(esp_gsp_handle_t ui, uint32_t period,
                           esp_gsp_timer_cb_t callback, void *context)
{
    CHECK(ui == &fake_ui && period == 50);
    if (current_fixture->fail_gsp_stage == 3) {
        return NULL;
    }
    timer_callback = callback;
    timer_context = context;
    return &fake_timer;
}

esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t ui, void *timer)
{
    CHECK(ui == &fake_ui && timer == &fake_timer);
    timer_callback = NULL;
    timer_context = NULL;
    return ESP_GSP_OK;
}

esp_gsp_err_t esp_gsp_canvas_stop(esp_gsp_handle_t ui, uint16_t bind)
{
    (void)bind;
    CHECK(ui == &fake_ui);
    if (current_fixture->fail_stop) {
        return ESP_GSP_ERR_INVALID_STATE;
    }
    canvas_callback = NULL;
    canvas_context = NULL;
    return ESP_GSP_OK;
}

static void *allocate_frame(void *context, size_t bytes)
{
    fixture_t *f = context;
    CHECK(bytes == 480U * 480U * sizeof(uint16_t));
    CHECK(f->allocation_calls < 3);
    const unsigned slot = f->allocation_calls++;
    if (f->allocation_calls == f->fail_allocation) {
        return NULL;
    }
    f->allocated[slot] = malloc(bytes);
    CHECK(f->allocated[slot] != NULL);
    memset(f->allocated[slot], 0xcd, bytes);
    return f->allocated[slot];
}

static void free_frame(void *context, void *frame)
{
    fixture_t *f = context;
    CHECK(frame != NULL);
    bool found = false;
    for (unsigned slot = 0; slot < 3; ++slot) {
        if (f->allocated[slot] == frame) {
            CHECK(!f->freed[slot]);
            f->freed[slot] = true;
            found = true;
            break;
        }
    }
    CHECK(found);
    free(frame);
    ++f->free_calls;
}

static uint64_t clock_ms(void *context)
{
    return ((fixture_t *)context)->clock_ms;
}
static bool read_settings(void *context, app_settings_snapshot_t *out)
{
    *out = ((fixture_t *)context)->display;
    return true;
}
static bool record_activity(void *context)
{
    fixture_t *f = context;
    ++f->activity_calls;
    const bool waking = f->force_wake || f->display.screen_off;
    f->force_wake = false;
    f->display.screen_off = false;
    f->display.effective_brightness = f->display.brightness;
    return waking;
}
static void interaction_busy(void *context, bool busy)
{
    ((fixture_t *)context)->busy = busy;
}

static fixture_t *fixture_create(const uint8_t *module, size_t size)
{
    fixture_t *f = calloc(1, sizeof(*f));
    CHECK(f != NULL);
    f->tracker = tracker_engine_create(module, size);
    f->synth = app_synth_create(48000);
    f->bus = app_audio_bus_create(f->tracker, f->synth);
    f->router = app_note_router_create(f->synth);
    f->capture = malloc(480U * 480U * sizeof(uint16_t));
    f->expected = malloc(480U * 480U * sizeof(uint16_t));
    CHECK(f->tracker && f->synth && f->bus && f->router && f->capture && f->expected);
    app_settings_snapshot_defaults(&f->display);
    f->clock_ms = 1000;
    f->config = (app_tracker_config_t) {
        .tracker = f->tracker, .synth = f->synth, .audio_bus = f->bus,
        .note_router = f->router,
        .hooks = {.context = f, .allocate_frame = allocate_frame, .free_frame = free_frame,
                  .time_ms = clock_ms, .read_settings = read_settings,
                  .record_activity = record_activity, .interaction_busy = interaction_busy
                 },
    };
    current_fixture = f;
    return f;
}

static void fixture_destroy(fixture_t *f)
{
    CHECK(canvas_callback == NULL && pointer_callback == NULL && timer_callback == NULL);
    app_audio_bus_destroy(f->bus);
    app_note_router_destroy(f->router);
    app_synth_destroy(f->synth);
    tracker_engine_destroy(f->tracker);
    free(f->capture);
    free(f->expected);
    free(f);
    current_fixture = NULL;
}

static const uint16_t *published(app_tracker_t *owner)
{
    return owner->frames[atomic_load(&owner->frame_index) & 1U];
}

static void tap(int x, int y)
{
    CHECK(pointer_callback != NULL);
    pointer_callback(&fake_ui, x, y, true, pointer_context);
    pointer_callback(&fake_ui, x, y, false, pointer_context);
}

static void drag(int x1, int y1, int x2, int y2)
{
    CHECK(pointer_callback != NULL);
    pointer_callback(&fake_ui, x1, y1, true, pointer_context);
    for (int step = 1; step <= 6; ++step) {
        pointer_callback(&fake_ui, x1 + (x2 - x1) * step / 6,
                         y1 + (y2 - y1) * step / 6, true, pointer_context);
    }
    pointer_callback(&fake_ui, x2, y2, false, pointer_context);
}

static void advance(fixture_t *f, unsigned milliseconds)
{
    f->clock_ms += milliseconds;
    CHECK(timer_callback != NULL);
    timer_callback(&fake_ui, timer_context);
}

static void open_synth_from_home(fixture_t *f, app_tracker_t *owner)
{
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_HOME);
    for (int attempt = 0; attempt < 3 && owner->workspace.home_page != 1; ++attempt) {
        drag(400, 460, 80, 460);
        advance(f, 250);
    }
    CHECK(owner->workspace.home_page == 1);
    tap(352, 176);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY && owner->model.view.page == 5);
}

static void expected_controls(fixture_t *f, app_tracker_t *owner)
{
    if (owner->model.view.page == TRACKER_PAGE_SYNTH) {
        tracker_render_synth(f->expected, &owner->model.synth_view);
    } else {
        tracker_render_controls(f->expected, &owner->model.view);
    }
    tracker_workspace_fit_legacy(f->expected);
    memcpy(f->capture, f->expected, FRAME_BYTES);
    tracker_workspace_render_header(f->expected, &owner->workspace, &owner->model);
    CHECK(memcmp(f->capture, f->expected, FRAME_BYTES) == 0);
}

static void test_allocations(const uint8_t *module, size_t size)
{
    for (unsigned failure = 1; failure <= 3; ++failure) {
        fixture_t *f = fixture_create(module, size);
        f->fail_allocation = failure;
        app_tracker_t *owner = NULL;
        CHECK(app_tracker_create(&fake_ui, &f->config, &owner) == ESP_GSP_ERR_NO_MEM);
        CHECK(owner == NULL && f->allocation_calls == 3 && f->free_calls == 2);
        fixture_destroy(f);
    }
    fixture_t *retained = fixture_create(module, size);
    app_tracker_t *retained_owner = NULL;
    CHECK(app_tracker_create(&fake_ui, &retained->config, &retained_owner) == ESP_GSP_OK);
    retained->fail_stop = true;
    app_tracker_stop(&retained_owner);
    CHECK(retained_owner != NULL && retained->free_calls == 0 && canvas_callback != NULL);
    retained->fail_stop = false;
    app_tracker_stop(&retained_owner);
    CHECK(retained_owner == NULL && retained->free_calls == 3);
    fixture_destroy(retained);
    for (unsigned failure = 1; failure <= 3; ++failure) {
        fixture_t *f = fixture_create(module, size);
        f->fail_gsp_stage = failure;
        app_tracker_t *owner = NULL;
        CHECK(app_tracker_create(&fake_ui, &f->config, &owner) == ESP_GSP_ERR_NO_MEM);
        CHECK(owner == NULL && f->free_calls == 3);
        fixture_destroy(f);
    }
    puts("UI owner: all 3 frame OOM and callback/timer rollback free non-NULL frames once PASS");
}

static void test_motion(const uint8_t *module, size_t size)
{
    fixture_t *f = fixture_create(module, size);
    app_tracker_t *owner = NULL;
    CHECK(app_tracker_create(&fake_ui, &f->config, &owner) == ESP_GSP_OK);
    open_synth_from_home(f, owner);
    CHECK(owner->model.view.page == 5 && owner->page_motion.active);
    advance(f, 50);
    CHECK(owner->page_motion.active && owner->page_motion.current > 0);
    memcpy(f->capture, published(owner), FRAME_BYTES);
    drag(240, 460, 240, 350);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_HOME && owner->page_motion.active && owner->page_vertical);
    CHECK(memcmp(published(owner), f->capture, FRAME_BYTES) == 0);
    tracker_workspace_render(f->expected, &owner->workspace, &owner->model);
    tracker_workspace_render_header(f->expected, &owner->workspace, &owner->model);
    advance(f, 50);
    const int offset = tracker_transition_pixels(owner->page_motion.current, 480);
    CHECK(offset > 0 && offset < 480);
    for (int row = 0; row < 480; ++row) {
        const uint16_t *source = row + offset < 480
                                 ? f->capture + (size_t)(row + offset) * 480U
                                 : f->expected + (size_t)(row + offset - 480) * 480U;
        CHECK(memcmp(published(owner) + (size_t)row * 480U, source, 480U * sizeof(uint16_t)) == 0);
    }
    advance(f, 200);
    CHECK(!owner->page_motion.active);
    CHECK(memcmp(published(owner), f->expected, FRAME_BYTES) == 0);
    open_synth_from_home(f, owner);
    CHECK(owner->page_motion.active && !owner->page_vertical && owner->model.view.page == 5);
    pointer_callback(&fake_ui, 56, 419, true, pointer_context);
    CHECK(owner->motion_gesture && owner->model.synth_view.note == -1);
    advance(f, 250);
    pointer_callback(&fake_ui, 109, 419, true, pointer_context);
    CHECK(owner->motion_gesture && owner->model.synth_view.note == -1);
    pointer_callback(&fake_ui, 109, 419, false, pointer_context);
    CHECK(!owner->motion_gesture && owner->model.synth_view.note == -1);
    pointer_callback(&fake_ui, 56, 419, true, pointer_context);
    CHECK(owner->model.synth_view.note == 60);
    pointer_callback(&fake_ui, 56, 419, false, pointer_context);
    CHECK(owner->model.synth_view.note == -1);
    pointer_callback(&fake_ui, 220, 12, true, pointer_context);
    pointer_callback(&fake_ui, 225, 90, true, pointer_context);
    pointer_callback(&fake_ui, 225, 90, false, pointer_context);
    CHECK(owner->settings.open && owner->drawer_motion.active && !owner->page_motion.active);
    advance(f, 90);
    memcpy(f->capture, published(owner), FRAME_BYTES);
    /* Direct UI-thread state reversal exercises drawer compositing independently
     * of the complete-gesture capture used by ordinary input. */
    owner->settings.open = false;
    repaint(owner);
    CHECK(owner->drawer_motion.active);
    CHECK(memcmp(published(owner), f->capture, FRAME_BYTES) == 0);
    advance(f, 180);
    CHECK(!owner->drawer_motion.active && owner->drawer_motion.current == 0);
    expected_controls(f, owner);
    CHECK(memcmp(published(owner), f->expected, FRAME_BYTES) == 0);
    app_tracker_stop(&owner);
    CHECK(owner == NULL && f->free_calls == 3);
    fixture_destroy(f);
    puts("UI owner: upward HOME pixels, interrupted-frame retarget and drawer snapshot PASS");
}

static void test_wake_capture(const uint8_t *module, size_t size)
{
    fixture_t *f = fixture_create(module, size);
    app_tracker_t *owner = NULL;
    CHECK(app_tracker_create(&fake_ui, &f->config, &owner) == ESP_GSP_OK);
    open_synth_from_home(f, owner);
    advance(f, 250);
    CHECK(owner->model.view.page == 5);
    f->display.screen_off = true;
    f->display.effective_brightness = 0;
    update_settings(owner);
    repaint(owner);
    pointer_callback(&fake_ui, 56, 419, true, pointer_context);
    pointer_callback(&fake_ui, 109, 419, true, pointer_context);
    CHECK(owner->wake_gesture && owner->model.synth_view.note == -1);
    pointer_callback(&fake_ui, 109, 419, false, pointer_context);
    CHECK(!owner->wake_gesture && owner->model.synth_view.note == -1);
    app_audio_bus_render(f->bus, f->audio, 512);
    app_synth_status_t status = {0};
    CHECK(app_synth_get_status(f->synth, &status) && status.active_voices == 0);
    pointer_callback(&fake_ui, 56, 419, true, pointer_context);
    CHECK(owner->model.synth_view.note == 60);
    app_tracker_cancel_pointer(owner);
    CHECK(owner->model.synth_view.note == -1 && !owner->wake_gesture &&
          !owner->motion_gesture && !owner->settings.pointer_down && !owner->model.pointer_down);
    pointer_callback(&fake_ui, 56, 419, false, pointer_context);
    CHECK(owner->model.synth_view.note == -1);
    f->force_wake = true;
    pointer_callback(&fake_ui, 220, 12, true, pointer_context);
    pointer_callback(&fake_ui, 225, 90, true, pointer_context);
    pointer_callback(&fake_ui, 225, 90, false, pointer_context);
    CHECK(!owner->settings.open && !owner->wake_gesture);
    app_tracker_stop(&owner);
    fixture_destroy(f);
    puts("UI owner: first wake consumes whole key/menu gesture and cancel releases capture PASS");
}

static void test_edge_navigation(const uint8_t *module, size_t size)
{
    fixture_t *f = fixture_create(module, size);
    app_tracker_t *owner = NULL;
    CHECK(app_tracker_create(&fake_ui, &f->config, &owner) == ESP_GSP_OK);
    CHECK(owner->workspace.home_page == 0 && owner->navigation_hint_until == 0);
    tap(436, 460);
    tap(225, 460);
    CHECK(owner->workspace.home_page == 0 && owner->navigation_hint_until == 0);

    drag(80, 460, 400, 460);
    CHECK(owner->workspace.home_page == 2 && owner->navigation_hint_until == f->clock_ms + 1200);
    advance(f, 250);
    memcpy(f->capture, published(owner), FRAME_BYTES);
    tap(241, 455); /* The visible indicator is not an interactive page target. */
    CHECK(owner->workspace.home_page == 2);
    for (int tick = 0; tick < 18; ++tick) {
        advance(f, 50);
    }
    CHECK(memcmp(published(owner) + 440U * 480U, f->capture + 440U * 480U,
                 480U * 40U * sizeof(uint16_t)) == 0);
    advance(f, 50);
    CHECK(memcmp(published(owner) + 440U * 480U, f->capture + 440U * 480U,
                 480U * 40U * sizeof(uint16_t)) != 0);
    tap(225, 455);
    CHECK(owner->workspace.home_page == 2 && owner->navigation_hint_until <= f->clock_ms);
    drag(400, 460, 80, 460);
    CHECK(owner->workspace.home_page == 0);
    advance(f, 250);
    tap(352, 176); /* SETTINGS opens through the ordinary HOME tile. */
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_SETTINGS && owner->navigation_hint_until == 0);
    advance(f, 250);
    memset(f->expected, 0, FRAME_BYTES);
    tracker_settings_render_page(f->expected, &owner->settings);
    tracker_workspace_fit_content(f->expected, &owner->workspace);
    CHECK(memcmp(published(owner), f->expected, FRAME_BYTES) == 0);
    memcpy(f->capture, f->expected, FRAME_BYTES);
    tracker_workspace_render_header(f->expected, &owner->workspace, &owner->model);
    CHECK(memcmp(f->capture, f->expected, FRAME_BYTES) == 0);
    tap(35, 12);
    tap(64, 458);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_SETTINGS);
    tap(300, 20); /* Header space outside the module's STORAGE action. */
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_SETTINGS);
    drag(250, 380, 250, 270);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_SETTINGS && !owner->settings.open);
    drag(240, 460, 240, 410);
    drag(240, 460, 300, 380);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_SETTINGS && owner->navigation_hint_until == 0);
    tap(400, 20);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_STORAGE);
    advance(f, 250);
    drag(240, 460, 240, 350);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_HOME && owner->workspace.home_page == 0);
    CHECK(owner->navigation_hint_until == 0 && !owner->navigation_pointer && !f->busy);
    advance(f, 250);

    tap(128, 334); /* PLAYBACK. */
    advance(f, 250);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY && owner->model.view.page == 2);
    const uint32_t before_shell_taps = owner->model.edit_revision;
    tap(185, 58);
    tap(414, 58);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY && owner->model.view.page == 2);
    CHECK(owner->model.edit_revision == before_shell_taps);
    tracker_engine_play(f->tracker);
    const uint8_t midi_note_on[4] = {0x09, 0x90, 60, 100};
    CHECK(app_note_router_midi_packet(f->router, midi_note_on));
    CHECK(app_note_router_touch_note_on(f->router, 64, 0.8f));
    const uint64_t generation = owner->workspace.session_generation;
    const uint32_t revision = owner->model.edit_revision;
    drag(240, 460, 240, 350);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_HOME && owner->workspace.home_page == 0);
    CHECK(tracker_engine_is_playing(f->tracker) && owner->workspace.session_generation == generation);
    CHECK(owner->model.edit_revision == revision && owner->navigation_hint_until == 0);
    app_note_router_status_t notes = {0};
    app_note_router_get_status(f->router, &notes);
    CHECK(notes.touch_notes == 0 && notes.midi_notes == 1);
    app_note_router_midi_disconnect(f->router);
    advance(f, 250);
    tap(352, 334); /* MIXER content owns an upward list drag. */
    advance(f, 250);
    drag(456, 390, 456, 190);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY && owner->model.view.page == 3);
    CHECK(owner->model.view.mix_scroll > 0 && !owner->navigation_pointer);
    drag(400, 460, 80, 460);
    drag(80, 460, 400, 460);
    drag(400, 310, 80, 310);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY && owner->model.view.page == 3);
    CHECK(owner->navigation_hint_until == 0 && !owner->page_motion.active);

    /* A bottom-edge gesture stays captured when it crosses into content. */
    pointer_callback(&fake_ui, 240, 460, true, pointer_context);
    CHECK(owner->navigation_pointer && f->busy);
    pointer_callback(&fake_ui, 240, 380, true, pointer_context);
    CHECK(owner->navigation_pointer && !owner->model.pointer_down && !owner->motion_gesture);
    app_tracker_cancel_pointer(owner);
    CHECK(!owner->navigation_pointer && !f->busy);
    pointer_callback(&fake_ui, 240, 350, false, pointer_context);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY);

    /* The drawer owns its closing gesture; it cannot fall through to HOME. */
    drag(220, 12, 225, 90);
    CHECK(owner->settings.open);
    advance(f, 250);
    drag(240, 460, 240, 350);
    CHECK(!owner->settings.open && owner->workspace.screen == TRACKER_WORKSPACE_LEGACY);
    CHECK(!owner->navigation_pointer);
    advance(f, 250);

    /* The first gesture after screen-off only wakes, even at the system edge. */
    f->display.screen_off = true;
    f->display.effective_brightness = 0;
    update_settings(owner);
    repaint(owner);
    drag(240, 460, 240, 350);
    CHECK(!owner->wake_gesture && !owner->navigation_pointer);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_LEGACY);
    drag(240, 460, 240, 350);
    CHECK(owner->workspace.screen == TRACKER_WORKSPACE_HOME && owner->workspace.home_page == 0);
    CHECK(tracker_engine_is_playing(f->tracker));
    app_tracker_stop(&owner);
    fixture_destroy(f);
    puts("UI owner: inert taps, timed swipe hints, edge HOME, drawer/wake and control capture PASS");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *file = fopen(argv[1], "rb"); CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file); CHECK(size > 0);
    rewind(file);
    uint8_t *module = malloc((size_t)size); CHECK(module != NULL);
    CHECK(fread(module, 1, (size_t)size, file) == (size_t)size);
    CHECK(fclose(file) == 0);
    test_allocations(module, (size_t)size);
    test_motion(module, (size_t)size);
    test_wake_capture(module, (size_t)size);
    test_edge_navigation(module, (size_t)size);
    free(module);
    puts("production UI owner lifecycle and motion tests: PASS");
    return 0;
}
