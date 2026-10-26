// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_USB_MIDI_MAX_CONFIG_BYTES 4096U
#define APP_USB_MIDI_MAX_PACKET_BYTES 512U

typedef struct {
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_in;   /* Keyboard/device -> OP-Ay1, zero if absent. */
    uint8_t endpoint_out;  /* OP-Ay1 -> keyboard/device, zero if absent. */
    uint16_t in_packet_bytes;
    uint16_t out_packet_bytes;
} app_usb_midi_interface_t;

/* Selects the first alternate-zero USB MIDI 1.0 Streaming interface with one
 * or both bulk endpoints. Rejects truncated/overlong/malformed descriptors,
 * duplicate directions and invalid packet sizes. Output unchanged on false. */
bool app_usb_midi_parse_configuration(const uint8_t *bytes, size_t size,
                                      app_usb_midi_interface_t *interface_out);
