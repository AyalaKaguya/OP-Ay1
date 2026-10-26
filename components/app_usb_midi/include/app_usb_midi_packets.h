// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_note_router.h"

typedef struct {
    uint32_t accepted;
    uint32_t dropped;
} app_usb_midi_packet_counts_t;

/* Processes at most one 512-byte bulk completion. Four-zero padding is ignored.
 * Invalid/short packets or rejected note-ons release only the MIDI source,
 * preventing a partial gesture or a full synth queue from leaving a held note.
 * Returns true only when no malformed/rejected event was encountered. */
bool app_usb_midi_receive_packets(app_note_router_t *router, const uint8_t *bytes,
                                   size_t size, app_usb_midi_packet_counts_t *counts);
