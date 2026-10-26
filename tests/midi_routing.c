// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_note_router.h"
#include "app_usb_midi.h"
#include "app_usb_midi_descriptor.h"
#include "app_usb_midi_packets.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "midi_routing:%d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef struct {
    app_synth_t *synth;
    app_note_router_t *router;
    float samples[1024];
    uint8_t transmitted[64][4];
    unsigned transmitted_count;
    bool reject_transmit;
} fixture_t;

static void render(fixture_t *f, unsigned frames)
{
    while (frames) {
        const unsigned count = frames < 512 ? frames : 512;
        app_synth_render(f->synth, f->samples, count);
        frames -= count;
    }
}

static fixture_t *fixture_create(void)
{
    fixture_t *f = calloc(1, sizeof(*f));
    CHECK(f != NULL);
    f->synth = app_synth_create(48000);
    f->router = app_note_router_create(f->synth);
    CHECK(f->synth != NULL && f->router != NULL);
    app_synth_patch_t patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
    patch.waveform = APP_SYNTH_WAVE_SINE;
    patch.attack_s = 0;
    patch.decay_s = 0;
    patch.sustain = 1;
    patch.release_s = 0.001f;
    CHECK(app_synth_set_patch(f->synth, &patch));
    render(f, 48000);
    return f;
}

static void fixture_destroy(fixture_t *f)
{
    app_note_router_destroy(f->router);
    app_synth_destroy(f->synth);
    free(f);
}

static bool midi(fixture_t *f, unsigned channel, unsigned type,
                 unsigned data1, unsigned data2)
{
    const uint8_t packet[4] = {(uint8_t)type, (uint8_t)(type * 16 + channel),
                               (uint8_t)data1, (uint8_t)data2
                              };
    return app_note_router_midi_packet(f->router, packet);
}

static unsigned active(fixture_t *f)
{
    app_synth_status_t status = {0};
    render(f, 512);
    CHECK(app_synth_get_status(f->synth, &status));
    return status.active_voices;
}

static app_note_router_status_t ownership(fixture_t *f)
{
    app_note_router_status_t status;
    app_note_router_get_status(f->router, &status);
    return status;
}

static void test_sources(void)
{
    fixture_t *f = fixture_create();
    CHECK(app_note_router_touch_note_on(f->router, 60, 0.8f));
    CHECK(midi(f, 0, 9, 60, 100));
    CHECK(active(f) == 1);
    app_note_router_release_touch(f->router);
    CHECK(ownership(f).touch_notes == 0 && ownership(f).midi_notes == 1);
    CHECK(active(f) == 1);
    CHECK(midi(f, 0, 8, 60, 0));
    CHECK(active(f) == 0);

    CHECK(midi(f, 2, 9, 64, 100));
    CHECK(app_note_router_touch_note_on(f->router, 64, 0.8f));
    app_note_router_midi_disconnect(f->router);
    CHECK(active(f) == 1 && ownership(f).touch_notes == 1);
    app_note_router_touch_note_off(f->router, 64);
    CHECK(active(f) == 0);

    CHECK(midi(f, 0, 9, 67, 100));
    CHECK(midi(f, 15, 9, 67, 100));
    CHECK(midi(f, 0, 8, 67, 0));
    CHECK(active(f) == 1);
    CHECK(midi(f, 15, 9, 67, 0)); /* Note On velocity zero means off. */
    CHECK(active(f) == 0);
    CHECK(!app_note_router_touch_note_on(f->router, -1, 0.8f));
    CHECK(!app_note_router_touch_note_on(f->router, 128, 0.8f));
    CHECK(!app_note_router_touch_note_on(f->router, 60, NAN));
    fixture_destroy(f);
    puts("midi: touch/channel ownership and selective disconnect PASS");
}

static void test_sustain_and_channel_controls(void)
{
    fixture_t *f = fixture_create();
    CHECK(midi(f, 0, 9, 60, 100));
    CHECK(midi(f, 0, 11, 64, 127));
    CHECK(midi(f, 0, 8, 60, 0));
    CHECK(active(f) == 1);
    CHECK(ownership(f).midi_notes == 0 && ownership(f).sustained_notes == 1);
    CHECK(midi(f, 1, 11, 64, 0));
    CHECK(active(f) == 1); /* Other channel's pedal cannot release this one. */
    CHECK(midi(f, 0, 11, 64, 0));
    CHECK(active(f) == 0);

    CHECK(midi(f, 0, 9, 60, 100));
    CHECK(midi(f, 0, 11, 64, 127));
    CHECK(midi(f, 0, 11, 123, 0));
    CHECK(active(f) == 1 && ownership(f).sustained_notes == 1);
    CHECK(midi(f, 0, 11, 121, 0));
    CHECK(active(f) == 0);

    CHECK(midi(f, 0, 9, 60, 100));
    CHECK(midi(f, 1, 9, 64, 100));
    CHECK(app_note_router_touch_note_on(f->router, 67, 0.8f));
    CHECK(midi(f, 0, 11, 120, 0));
    CHECK(active(f) == 2); /* Channel clear leaves channel 1 and touch alive. */
    app_note_router_panic(f->router);
    CHECK(active(f) == 0 && ownership(f).touch_notes == 0 && ownership(f).midi_notes == 0);
    fixture_destroy(f);
    puts("midi: per-channel sustain and all-off controls PASS");
}

static bool capture_transmit(void *context, const uint8_t packet[4])
{
    fixture_t *f = context;
    if (f->reject_transmit) {
        return false;
    }
    CHECK(f->transmitted_count < 64);
    memcpy(f->transmitted[f->transmitted_count++], packet, 4);
    return true;
}

static void test_transmit_and_queue_withdrawal(void)
{
    fixture_t *f = fixture_create();
    app_note_router_set_midi_tx(f->router, capture_transmit, f);
    CHECK(app_note_router_touch_note_on(f->router, 60, 0.8f));
    CHECK(f->transmitted_count == 1 && f->transmitted[0][1] == 0x90);
    app_note_router_touch_note_off(f->router, 60);
    CHECK(f->transmitted_count == 2 && f->transmitted[1][1] == 0x80);
    CHECK(midi(f, 0, 9, 64, 100));
    CHECK(midi(f, 0, 8, 64, 0));
    CHECK(f->transmitted_count == 2); /* Incoming data is not echoed. */
    CHECK(app_note_router_touch_note_on(f->router, 60, 0.0001f));
    CHECK(f->transmitted[f->transmitted_count - 1][3] == 1);
    app_note_router_touch_note_off(f->router, 60);
    f->reject_transmit = true;
    CHECK(app_note_router_touch_note_on(f->router, 67, 0.8f));
    app_note_router_release_touch(f->router);
    CHECK(ownership(f).dropped_tx == 2);
    render(f, 512);

    for (unsigned i = 0; i < APP_SYNTH_COMMAND_CAPACITY; ++i) {
        CHECK(app_synth_set_patch_id(f->synth, APP_SYNTH_PATCH_INIT));
    }
    CHECK(!app_note_router_touch_note_on(f->router, 72, 0.8f));
    CHECK(ownership(f).touch_notes == 0);
    const uint8_t on[4] = {9, 0x90, 76, 100};
    app_usb_midi_packet_counts_t counts;
    CHECK(!app_usb_midi_receive_packets(f->router, on, sizeof(on), &counts));
    CHECK(counts.dropped == 1 && ownership(f).midi_notes == 0);
    CHECK(active(f) == 0);
    app_note_router_set_midi_tx(f->router, NULL, NULL);
    fixture_destroy(f);
    puts("midi: no echo, TX rejection, full synth queue withdrawal PASS");
}

static void test_packet_validation(void)
{
    fixture_t *f = fixture_create();
    const uint8_t running_status[4] = {9, 60, 100, 0};
    const uint8_t wrong_cin[4] = {8, 0x90, 60, 100};
    const uint8_t bad_data[4] = {9, 0x90, 60, 128};
    const uint8_t other_cable[4] = {0x19, 0x90, 60, 100};
    CHECK(!app_note_router_midi_packet(f->router, running_status));
    CHECK(!app_note_router_midi_packet(f->router, wrong_cin));
    CHECK(!app_note_router_midi_packet(f->router, bad_data));
    CHECK(!app_note_router_midi_packet(f->router, other_cable));
    const uint8_t clock[4] = {0x0f, 0xf8, 0, 0};
    const uint8_t program[4] = {0x0c, 0xc0, 4, 0};
    CHECK(app_note_router_midi_packet(f->router, clock));
    CHECK(app_note_router_midi_packet(f->router, program));
    CHECK(ownership(f).midi_notes == 0);
    CHECK(midi(f, 0, 9, 60, 100));
    CHECK(app_note_router_touch_note_on(f->router, 67, 0.8f));
    const uint8_t short_sysex[8] = {6, 0xf0, 0xf7, 0, 7, 0xf0, 1, 0xf7};
    app_usb_midi_packet_counts_t counts;
    CHECK(app_usb_midi_receive_packets(f->router, short_sysex, sizeof(short_sysex), &counts));
    CHECK(counts.accepted == 2 && ownership(f).midi_notes == 1);
    const uint8_t short_completion[7] = {0x0f, 0xf8, 0, 0, 0x09, 0x90, 60};
    CHECK(!app_usb_midi_receive_packets(f->router, short_completion, sizeof(short_completion), &counts));
    CHECK(counts.accepted == 1 && counts.dropped == 1);
    CHECK(ownership(f).midi_notes == 0 && ownership(f).touch_notes == 1);
    CHECK(active(f) == 1);
    const uint8_t padding[8] = {0};
    CHECK(app_usb_midi_receive_packets(f->router, padding, sizeof(padding), &counts));
    CHECK(counts.accepted == 0 && counts.dropped == 0);
    fixture_destroy(f);
    puts("midi: CIN, running-status rejection, short completion, realtime/padding PASS");
}

static void test_descriptors(void)
{
    static const uint8_t valid[] = {
        9, 2, 49, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 1, 3, 0, 0,
        7, 0x24, 1, 0, 1, 37, 0,
        7, 5, 0x81, 2, 64, 0, 0,
        5, 0x25, 1, 1, 1,
        7, 5, 0x01, 2, 64, 0, 0,
        5, 0x25, 1, 1, 3,
    };
    app_usb_midi_interface_t result;
    CHECK(app_usb_midi_parse_configuration(valid, sizeof(valid), &result));
    CHECK(result.interface_number == 0 && result.endpoint_in == 0x81 &&
          result.endpoint_out == 1 && result.in_packet_bytes == 64);
    uint8_t broken[sizeof(valid)];
    for (unsigned size = 0; size < sizeof(valid); ++size) {
        CHECK(!app_usb_midi_parse_configuration(valid, size, &result));
    }
    memcpy(broken, valid, sizeof(broken));
    broken[9] = 0;
    CHECK(!app_usb_midi_parse_configuration(broken, sizeof(broken), &result));
    memcpy(broken, valid, sizeof(broken));
    broken[39] = 0x82; /* Two IN directions in one streaming interface. */
    CHECK(!app_usb_midi_parse_configuration(broken, sizeof(broken), &result));
    memcpy(broken, valid, sizeof(broken));
    broken[29] = 0;
    CHECK(!app_usb_midi_parse_configuration(broken, sizeof(broken), &result));
    memcpy(broken, valid, sizeof(broken));
    broken[29] = 0; broken[30] = 2; broken[41] = 0; broken[42] = 2;
    CHECK(app_usb_midi_parse_configuration(broken, sizeof(broken), &result));
    CHECK(result.in_packet_bytes == 512 && result.out_packet_bytes == 512);
    memcpy(broken, valid, sizeof(broken));
    broken[15] = 2; /* Non-MIDI Audio subclass. */
    CHECK(!app_usb_midi_parse_configuration(broken, sizeof(broken), &result));
    puts("midi: standard MIDI1 descriptor bounds and FS/HS endpoints PASS");
}

static void test_native_mode(void)
{
    fixture_t *f = fixture_create();
    app_usb_midi_t *usb = NULL;
    CHECK(app_usb_midi_create(f->router, &usb));
    app_usb_midi_status_t status;
    app_usb_midi_get_status(usb, &status);
    CHECK(status.actual_mode == APP_USB_MODE_VENDOR && !status.connected);
    CHECK(!app_usb_midi_request_mode(usb, APP_USB_MODE_MIDI_HOST));
    app_usb_midi_get_status(usb, &status);
    CHECK(status.state == APP_USB_STATE_ERROR && status.actual_mode == APP_USB_MODE_VENDOR);
    CHECK(app_usb_midi_request_mode(usb, APP_USB_MODE_VENDOR));
    CHECK(app_usb_midi_shutdown(&usb, 100) && usb == NULL);
    fixture_destroy(f);
    puts("midi: native preview never claims a physical keyboard PASS");
}

int main(void)
{
    test_descriptors();
    test_sources();
    test_sustain_and_channel_controls();
    test_transmit_and_queue_withdrawal();
    test_packet_validation();
    test_native_mode();
    puts("midi native routing/descriptor tests: PASS");
    return 0;
}
