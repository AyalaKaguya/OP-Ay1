// SPDX-License-Identifier: Apache-2.0
#include "app_usb_midi_packets.h"
#include "app_usb_midi_descriptor.h"

#include <string.h>

bool app_usb_midi_receive_packets(app_note_router_t *router, const uint8_t *bytes,
                                   size_t size, app_usb_midi_packet_counts_t *counts)
{
    app_usb_midi_packet_counts_t result = {0};
    if (router == NULL || (bytes == NULL && size != 0) ||
            size > APP_USB_MIDI_MAX_PACKET_BYTES) {
        result.dropped = 1;
    } else {
        for (size_t offset = 0; offset + 4 <= size; offset += 4) {
            const uint8_t *packet = bytes + offset;
            if (packet[0] == 0 && packet[1] == 0 && packet[2] == 0 && packet[3] == 0) {
                continue;
            }
            if (app_note_router_midi_packet(router, packet)) {
                ++result.accepted;
            } else {
                ++result.dropped;
            }
        }
        if (size % 4) {
            ++result.dropped;
        }
    }
    if (result.dropped && router != NULL) {
        app_note_router_midi_disconnect(router);
    }
    if (counts != NULL) {
        *counts = result;
    }
    return result.dropped == 0;
}
