// SPDX-License-Identifier: Apache-2.0
#include "app_usb_midi_descriptor.h"

#include <string.h>

#define DESC_CONFIGURATION 2U
#define DESC_INTERFACE 4U
#define DESC_ENDPOINT 5U
#define DESC_CS_INTERFACE 0x24U
#define CLASS_AUDIO 1U
#define SUBCLASS_MIDI_STREAMING 3U
#define MIDI_STREAMING_HEADER 1U
#define TRANSFER_BULK 2U

static unsigned read_u16(const uint8_t *bytes)
{
    return (unsigned)bytes[0] | (unsigned)bytes[1] << 8;
}

static bool packet_size_valid(unsigned size)
{
    return size == 8 || size == 16 || size == 32 || size == 64 || size == 512;
}

static bool finish_interface(const app_usb_midi_interface_t *candidate,
                             bool header_seen, unsigned endpoints_expected,
                             unsigned endpoints_seen)
{
    return header_seen && endpoints_expected == endpoints_seen &&
           endpoints_seen > 0 && endpoints_seen <= 2 &&
           (candidate->endpoint_in || candidate->endpoint_out);
}

bool app_usb_midi_parse_configuration(const uint8_t *bytes, size_t size,
                                      app_usb_midi_interface_t *interface_out)
{
    if (bytes == NULL || interface_out == NULL || size < 9 ||
            size > APP_USB_MIDI_MAX_CONFIG_BYTES || bytes[0] < 9 ||
            bytes[1] != DESC_CONFIGURATION || bytes[4] == 0) {
        return false;
    }
    const size_t total = read_u16(bytes + 2);
    if (total < bytes[0] || total > size) {
        return false;
    }
    app_usb_midi_interface_t candidate = {0};
    app_usb_midi_interface_t selected = {0};
    bool found = false;
    bool midi = false;
    bool header_seen = false;
    unsigned endpoints_expected = 0;
    unsigned endpoints_seen = 0;
    for (size_t offset = bytes[0]; offset < total;) {
        if (total - offset < 2 || bytes[offset] < 2 || bytes[offset] > total - offset) {
            return false;
        }
        const uint8_t *descriptor = bytes + offset;
        const unsigned length = descriptor[0];
        const unsigned type = descriptor[1];
        if (type == DESC_INTERFACE) {
            if (length < 9 || descriptor[2] >= bytes[4]) {
                return false;
            }
            if (midi && !finish_interface(&candidate, header_seen,
                                          endpoints_expected, endpoints_seen)) {
                return false;
            }
            if (midi && !found) {
                selected = candidate;
                found = true;
            }
            midi = descriptor[5] == CLASS_AUDIO &&
                   descriptor[6] == SUBCLASS_MIDI_STREAMING &&
                   descriptor[7] == 0 && descriptor[3] == 0;
            memset(&candidate, 0, sizeof(candidate));
            candidate.interface_number = descriptor[2];
            candidate.alternate_setting = descriptor[3];
            endpoints_expected = descriptor[4];
            endpoints_seen = 0;
            header_seen = false;
            if (midi && endpoints_expected > 2) {
                return false;
            }
        } else if (midi && type == DESC_CS_INTERFACE) {
            if (length < 3) {
                return false;
            }
            if (descriptor[2] == MIDI_STREAMING_HEADER) {
                if (length < 7 || read_u16(descriptor + 3) != 0x0100U) {
                    return false;
                }
                header_seen = true;
            }
        } else if (midi && type == DESC_ENDPOINT) {
            if (length < 7 || (descriptor[3] & 3U) != TRANSFER_BULK ||
                    (descriptor[2] & 0x70U) != 0 || (descriptor[2] & 0x0fU) == 0) {
                return false;
            }
            const unsigned packet_size = read_u16(descriptor + 4);
            if (!packet_size_valid(packet_size)) {
                return false;
            }
            if (descriptor[2] & 0x80U) {
                if (candidate.endpoint_in) {
                    return false;
                }
                candidate.endpoint_in = descriptor[2];
                candidate.in_packet_bytes = (uint16_t)packet_size;
            } else {
                if (candidate.endpoint_out) {
                    return false;
                }
                candidate.endpoint_out = descriptor[2];
                candidate.out_packet_bytes = (uint16_t)packet_size;
            }
            ++endpoints_seen;
        }
        offset += length;
    }
    if (midi && !finish_interface(&candidate, header_seen,
                                  endpoints_expected, endpoints_seen)) {
        return false;
    }
    if (midi && !found) {
        selected = candidate;
        found = true;
    }
    if (found) {
        *interface_out = selected;
    }
    return found;
}
