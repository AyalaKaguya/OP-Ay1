// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_gsp.h"
#include "app_audio_bus.h"
#include "app_synth.h"
#include "app_project.h"
#include "app_sampler.h"
#include "app_settings.h"
#include "app_usb_midi.h"
#include "app_note_router.h"
#include "tracker_engine.h"

typedef struct app_tracker app_tracker_t;

typedef struct {
    void *context;
    void *(*allocate_frame)(void *context, size_t bytes);
    void (*free_frame)(void *context, void *frame);
    uint64_t (*time_ms)(void *context);
    void (*set_volume)(void *context, int percent);
    void (*heartbeat)(void *context);
    void (*shutdown)(void *context);
    bool (*read_settings)(void *context, app_settings_snapshot_t *snapshot);
    bool (*request_settings)(void *context, int brightness, int rotation, bool locked);
    bool (*request_idle)(void *context, uint32_t dim_after_s, uint32_t off_after_s);
    /* Record real input. True consumes a screen-off wake gesture through up. */
    bool (*record_activity)(void *context);
    void (*interaction_busy)(void *context, bool busy);
} app_tracker_hooks_t;

typedef struct {
    tracker_engine_t *tracker;
    app_synth_t *synth;
    app_audio_bus_t *audio_bus;
    app_note_router_t *note_router;
    app_usb_midi_t *usb_midi;
    app_project_t *project;
    app_sampler_t *sampler;
    bool visualize_display_power; /* Native preview only; device uses panel power. */
    app_tracker_hooks_t hooks;
} app_tracker_config_t;

typedef struct {
    int page;
    int volume;
    int mix_scroll;
    int held_note;
    int synth_tab;
    int preset;
    int gain_steps[4];
    bool muted[4];
    bool soloed[4];
    bool playing;
    int launched_pattern;
    int pending_pattern;
    bool stop_pending;
    uint32_t active_voices;
    float synth_peak;
    uint64_t rendered_frames;
    uint16_t transition_progress;
    bool page_animating;
    bool menu_animating;
    int workspace_screen;
    int home_page;
    bool project_opened;
    char project_name[APP_PROJECT_NAME_BYTES];
    bool project_busy;
    bool project_dirty;
    uint64_t project_generation;
    int sampler_tab;
    unsigned sample_count;
    unsigned sample_index;
    bool sample_edit_active;
    bool sample_edit_dirty;
    uint32_t sample_trim_start;
    uint32_t sample_trim_end;
    uint32_t sample_loop_start;
    uint32_t sample_loop_end;
    uint32_t sample_c2;
    float sample_gain;
    bool sample_loop_enabled;
    int record_state;
    uint32_t record_frames;
    esp_gsp_err_t last_error;
} app_tracker_snapshot_t;

typedef struct {
    app_settings_snapshot_t display;
    app_usb_midi_status_t usb;
    bool menu_open;
    bool confirmation_open;
    bool timeouts_open;
} app_tracker_settings_snapshot_t;

/* Sources are borrowed. Callbacks and read belong to the UI thread.
 * Hooks and sources outlive the owner. On rare rollback fence failure, a
 * non-NULL output retains ownership for stop() retry even when create fails. */
esp_gsp_err_t app_tracker_create(esp_gsp_handle_t ui,
                                 const app_tracker_config_t *config,
                                 app_tracker_t **out_handle);
esp_gsp_err_t app_tracker_read(app_tracker_t *state,
                               app_tracker_snapshot_t *snapshot);
esp_gsp_err_t app_tracker_read_settings(app_tracker_t *state,
                                        app_tracker_settings_snapshot_t *snapshot);
/* Cross-task hardware requests are drained by the UI callback thread. */
void app_tracker_request_transport(app_tracker_t *state, bool restart);
/* UI-thread entry for a platform input queue; uses the same pointer path as
 * mapped GSP input. Device RPC tasks must enqueue rather than call directly. */
void app_tracker_feed_pointer(app_tracker_t *state, int x, int y, bool pressed);
/* UI-thread cancellation: releases held input without committing taps/swipes. */
void app_tracker_cancel_pointer(app_tracker_t *state);

/* Device adapter: PSRAM frame, button requests and independent watchdog. */
esp_err_t app_tracker_start_bus(esp_gsp_handle_t ui, tracker_engine_t *engine,
                                app_synth_t *synth, app_audio_bus_t *bus,
                                app_tracker_t **out_handle);
/* Settings/USB services are borrowed, source-owned and process lifetime. */
struct app_settings;
esp_err_t app_tracker_start_workstation(esp_gsp_handle_t ui, tracker_engine_t *engine,
                                        app_synth_t *synth, app_audio_bus_t *bus,
                                        app_note_router_t *router, app_usb_midi_t *usb,
                                        struct app_settings *settings,
                                        app_project_t *project, app_sampler_t *sampler,
                                        app_tracker_t **out_handle);
/* Compatibility device entry point with tracker only. */
esp_err_t app_tracker_start(esp_gsp_handle_t ui, tracker_engine_t *engine,
                            app_tracker_t **out_handle);

/* Host: serialized with callbacks. Device: external control task, never a GSP
 * callback; detaches and fences selected callbacks. A failed fence retains the
 * handle and source lifetimes for a later retry. */
void app_tracker_stop(app_tracker_t **handle);
