// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app_audio_bus.h"
#include "app_synth.h"
#include "app_note_router.h"
#include "tracker_pages.h"

enum {
    TRACKER_PAGE_WAVES = 0,
    TRACKER_PAGE_PATTERN = 1,
    TRACKER_PAGE_SYNTH = 5,
    TRACKER_PAGE_COUNT = 6,
};

typedef enum {
    TRACKER_INPUT_NONE,
    TRACKER_INPUT_NAV,
    TRACKER_INPUT_MIX_PENDING,
    TRACKER_INPUT_MIX_SCROLL,
    TRACKER_INPUT_GAIN,
    TRACKER_INPUT_MASTER,
    TRACKER_INPUT_SYNTH_PARAM,
    TRACKER_INPUT_KEYBOARD,
} tracker_input_kind_t;

typedef struct {
    app_synth_patch_t patch;
    app_synth_status_t status;
    int preset;
    int tab;
    int octave;
    int note;
    bool command_failed;
} tracker_synth_view_t;

/* Mutable UI state has one owner: the GSP callback thread on device and host.
 * Only the borrowed source APIs and bus cross the audio/UI task boundary. */
typedef struct {
    tracker_engine_t *engine;
    app_synth_t *synth;
    app_audio_bus_t *bus;
    app_note_router_t *router;
    tracker_controls_view_t view;
    tracker_synth_view_t synth_view;
    tracker_input_kind_t input;
    bool pointer_down;
    bool moved;
    bool dirty;
    bool patch_pending;
    int pending_preset;
    int start_x;
    int start_y;
    int last_y;
    int drag_channel;
    int drag_parameter;
    int scroll_start;
    float fling;
    uint64_t input_time;
    uint64_t last_pointer_time;
    uint64_t now_ms;
    unsigned clip_ticks;
    uint32_t edit_revision;
    bool user_mute[TRACKER_MAX_CHANNELS];
    bool auto_mute[TRACKER_MAX_CHANNELS];
} tracker_model_t;

void tracker_model_init(tracker_model_t *model, tracker_engine_t *engine,
                        app_synth_t *synth, app_audio_bus_t *bus);
void tracker_model_tick(tracker_model_t *model, uint64_t now_ms);
void tracker_model_pointer(tracker_model_t *model, int x, int y,
                           bool pressed, uint64_t now_ms);
void tracker_model_set_page(tracker_model_t *model, int page);
void tracker_model_cancel_input(tracker_model_t *model);
/* Refresh views after the project worker commits a complete source session.
 * Reads public source controls; does not reset gains or mutate loaded content. */
void tracker_model_sync_sources(tracker_model_t *model);
void tracker_model_transport(tracker_model_t *model, bool restart,
                             bool song_mode);

/* Synth controls and rendering share the same keyboard/parameter geometry. */
int tracker_synth_note_at(int x, int y, int octave);
int tracker_synth_parameter_at(int tab, int x, int y);
void tracker_synth_set_parameter(tracker_synth_view_t *view, int parameter,
                                 int x);
void tracker_render_synth(uint16_t *frame, const tracker_synth_view_t *view);
