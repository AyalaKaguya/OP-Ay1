// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_synth.h"

typedef struct app_note_router app_note_router_t;
typedef bool (*app_note_router_tx_fn)(void *context, const uint8_t packet[4]);

typedef struct {
    unsigned touch_notes;
    unsigned midi_notes;
    unsigned sustained_notes;
    uint32_t invalid_packets;
    uint32_t rejected_notes;
    uint32_t dropped_tx;
} app_note_router_status_t;

/* Borrows synth. The router serializes control producers; it never renders.
 * Destroy only after every UI/USB caller has stopped. */
app_note_router_t *app_note_router_create(app_synth_t *synth);
void app_note_router_destroy(app_note_router_t *router);
bool app_note_router_touch_note_on(app_note_router_t *router, int note, float velocity);
void app_note_router_touch_note_off(app_note_router_t *router, int note);
void app_note_router_release_touch(app_note_router_t *router);
/* Accepts one USB-MIDI 1.0 event packet, virtual cable zero. Incoming messages
 * are never echoed. Note On/Off and CC64/120/121/123 affect channel ownership. */
bool app_note_router_midi_packet(app_note_router_t *router, const uint8_t packet[4]);
void app_note_router_midi_disconnect(app_note_router_t *router);
/* Clears all sources; hardware and software PANIC share this entry point. */
void app_note_router_panic(app_note_router_t *router);
void app_note_router_get_status(app_note_router_t *router, app_note_router_status_t *status);
/* One borrowed, nonblocking TX sink. Called under the router control lock;
 * it must not call this router. Set NULL before destroying the sink context. */
void app_note_router_set_midi_tx(app_note_router_t *router,
                                 app_note_router_tx_fn callback, void *context);
