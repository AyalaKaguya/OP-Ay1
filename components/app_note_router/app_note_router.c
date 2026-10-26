// SPDX-License-Identifier: Apache-2.0
#include "app_note_router.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#define MIDI_CHANNELS 16
#define MIDI_NOTES 128

struct app_note_router {
    app_synth_t *synth;
    bool touch[MIDI_NOTES];
    uint16_t keys[MIDI_NOTES];
    uint16_t latched[MIDI_NOTES];
    uint16_t sustain;
    app_note_router_tx_fn transmit;
    void *transmit_context;
    app_note_router_status_t counters;
#if defined(ESP_PLATFORM)
    SemaphoreHandle_t lock;
#else
    atomic_bool lock;
#endif
};

static void control_lock(app_note_router_t *router)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreTake(router->lock, portMAX_DELAY);
#else
    while (atomic_exchange_explicit(&router->lock, true, memory_order_acquire)) {
    }
#endif
}

static void control_unlock(app_note_router_t *router)
{
#if defined(ESP_PLATFORM)
    (void)xSemaphoreGive(router->lock);
#else
    atomic_store_explicit(&router->lock, false, memory_order_release);
#endif
}

static bool valid_note(int note)
{
    return note >= 0 && note < MIDI_NOTES;
}

static bool held(const app_note_router_t *router, int note)
{
    return router->touch[note] || router->keys[note] || router->latched[note];
}

static bool start_note(app_note_router_t *router, int note, float velocity,
                        bool retrigger)
{
    if ((!held(router, note) || retrigger) &&
            !app_synth_note_on(router->synth, note, velocity)) {
        ++router->counters.rejected_notes;
        return false;
    }
    return true;
}

static void release_if_idle(app_note_router_t *router, int note)
{
    if (!held(router, note)) {
        (void)app_synth_note_off(router->synth, note);
    }
}

static void transmit(app_note_router_t *router, const uint8_t packet[4])
{
    if (router->transmit &&
            !router->transmit(router->transmit_context, packet)) {
        ++router->counters.dropped_tx;
    }
}

app_note_router_t *app_note_router_create(app_synth_t *synth)
{
    if (synth == NULL) {
        return NULL;
    }
    app_note_router_t *router = calloc(1, sizeof(*router));
    if (router == NULL) {
        return NULL;
    }
    router->synth = synth;
#if defined(ESP_PLATFORM)
    router->lock = xSemaphoreCreateMutex();
    if (router->lock == NULL) {
        free(router);
        return NULL;
    }
#else
    atomic_init(&router->lock, false);
#endif
    return router;
}

void app_note_router_destroy(app_note_router_t *router)
{
    if (router == NULL) {
        return;
    }
    app_note_router_panic(router);
#if defined(ESP_PLATFORM)
    vSemaphoreDelete(router->lock);
#endif
    free(router);
}

bool app_note_router_touch_note_on(app_note_router_t *router, int note,
                                   float velocity)
{
    if (router == NULL || !valid_note(note) || !isfinite(velocity)) {
        return false;
    }
    if (velocity <= 0.0f) {
        app_note_router_touch_note_off(router, note);
        return true;
    }
    velocity = velocity > 1.0f ? 1.0f : velocity;
    control_lock(router);
    const bool accepted = start_note(router, note, velocity, router->touch[note]);
    if (accepted) {
        router->touch[note] = true;
        const uint8_t packet[4] = {0x09, 0x90, (uint8_t)note,
                                   (uint8_t)(velocity * 127.0f + 0.5f)};
        transmit(router, packet);
    }
    control_unlock(router);
    return accepted;
}

static void release_touch_note(app_note_router_t *router, int note)
{
    if (router->touch[note]) {
        router->touch[note] = false;
        release_if_idle(router, note);
        const uint8_t packet[4] = {0x08, 0x80, (uint8_t)note, 0};
        transmit(router, packet);
    }
}

void app_note_router_touch_note_off(app_note_router_t *router, int note)
{
    if (router == NULL || !valid_note(note)) {
        return;
    }
    control_lock(router);
    release_touch_note(router, note);
    control_unlock(router);
}

void app_note_router_release_touch(app_note_router_t *router)
{
    if (router == NULL) {
        return;
    }
    control_lock(router);
    for (int note = 0; note < MIDI_NOTES; ++note) {
        release_touch_note(router, note);
    }
    control_unlock(router);
}

static void channel_all_off(app_note_router_t *router, uint16_t channel,
                            bool honor_sustain)
{
    for (int note = 0; note < MIDI_NOTES; ++note) {
        const bool existed = (router->keys[note] | router->latched[note]) & channel;
        if (honor_sustain && (router->sustain & channel)) {
            router->latched[note] |= router->keys[note] & channel;
        } else {
            router->latched[note] &= (uint16_t)~channel;
        }
        router->keys[note] &= (uint16_t)~channel;
        if (existed) {
            release_if_idle(router, note);
        }
    }
}

static void sustain_release(app_note_router_t *router, uint16_t channel)
{
    router->sustain &= (uint16_t)~channel;
    for (int note = 0; note < MIDI_NOTES; ++note) {
        if (router->latched[note] & channel) {
            router->latched[note] &= (uint16_t)~channel;
            release_if_idle(router, note);
        }
    }
}

static bool packet_valid(const uint8_t packet[4])
{
    /* Running status is not allowed in a USB MIDI 1.0 Event Packet. The CIN
     * must match the embedded status, and all MIDI data bytes are seven bit. */
    if (packet == NULL || (packet[0] >> 4) != 0) {
        return false;
    }
    const unsigned cin = packet[0] & 0x0f;
    const unsigned type = packet[1] >> 4;
    if (cin == 0x0f) {
        return packet[1] >= 0xf8 && packet[2] == 0 && packet[3] == 0;
    }
    if (cin == 2) {
        return (packet[1] == 0xf1 || packet[1] == 0xf3) &&
               packet[2] < 128 && packet[3] == 0;
    }
    if (cin == 3) {
        return packet[1] == 0xf2 && packet[2] < 128 && packet[3] < 128;
    }
    if (cin == 4) {
        return (packet[1] == 0xf0 || packet[1] < 128) &&
               packet[2] < 128 && packet[3] < 128;
    }
    if (cin == 5) {
        return (packet[1] == 0xf7 || packet[1] == 0xf6) &&
               packet[2] == 0 && packet[3] == 0;
    }
    if (cin == 6) {
        return packet[1] < 128 && packet[2] == 0xf7 && packet[3] == 0;
    }
    if (cin == 7) {
        return packet[1] < 128 && packet[2] < 128 && packet[3] == 0xf7;
    }
    if (type < 8 || type > 14 || cin != type || packet[2] >= 128) {
        return false;
    }
    return (type == 12 || type == 13) ? packet[3] == 0 : packet[3] < 128;
}

bool app_note_router_midi_packet(app_note_router_t *router,
                                 const uint8_t packet[4])
{
    if (router == NULL) {
        return false;
    }
    control_lock(router);
    if (!packet_valid(packet)) {
        ++router->counters.invalid_packets;
        control_unlock(router);
        return false;
    }
    const unsigned type = packet[1] >> 4;
    const uint16_t channel = (uint16_t)(1U << (packet[1] & 0x0f));
    const int note = packet[2];
    bool accepted = true;
    if (type == 9 && packet[3] != 0) {
        const bool retrigger = ((router->keys[note] | router->latched[note]) & channel) != 0;
        accepted = start_note(router, note, packet[3] / 127.0f, retrigger);
        if (accepted) {
            router->keys[note] |= channel;
            router->latched[note] &= (uint16_t)~channel;
        }
    } else if (type == 8 || type == 9) {
        const bool existed = (router->keys[note] & channel) != 0;
        router->keys[note] &= (uint16_t)~channel;
        if (existed && (router->sustain & channel)) {
            router->latched[note] |= channel;
        } else if (existed) {
            router->latched[note] &= (uint16_t)~channel;
        }
        if (existed) {
            release_if_idle(router, note);
        }
    } else if (type == 11) {
        if (packet[2] == 64) {
            if (packet[3] >= 64) {
                router->sustain |= channel;
            } else {
                sustain_release(router, channel);
            }
        } else if (packet[2] == 120) {
            channel_all_off(router, channel, false);
        } else if (packet[2] == 123) {
            channel_all_off(router, channel, true);
        } else if (packet[2] == 121) {
            sustain_release(router, channel);
        }
    }
    control_unlock(router);
    return accepted;
}

void app_note_router_midi_disconnect(app_note_router_t *router)
{
    if (router == NULL) {
        return;
    }
    control_lock(router);
    router->sustain = 0;
    channel_all_off(router, UINT16_MAX, false);
    control_unlock(router);
}

void app_note_router_panic(app_note_router_t *router)
{
    if (router == NULL) {
        return;
    }
    control_lock(router);
    memset(router->touch, 0, sizeof(router->touch));
    memset(router->keys, 0, sizeof(router->keys));
    memset(router->latched, 0, sizeof(router->latched));
    router->sustain = 0;
    app_synth_panic(router->synth);
    const uint8_t packet[4] = {0x0b, 0xb0, 120, 0};
    transmit(router, packet);
    control_unlock(router);
}

void app_note_router_get_status(app_note_router_t *router,
                                app_note_router_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (router == NULL) {
        return;
    }
    control_lock(router);
    *status = router->counters;
    for (int note = 0; note < MIDI_NOTES; ++note) {
        status->touch_notes += router->touch[note] ? 1U : 0U;
        status->midi_notes += router->keys[note] ? 1U : 0U;
        status->sustained_notes += router->latched[note] ? 1U : 0U;
    }
    control_unlock(router);
}

void app_note_router_set_midi_tx(app_note_router_t *router,
                                 app_note_router_tx_fn callback, void *context)
{
    if (router == NULL) {
        return;
    }
    control_lock(router);
    router->transmit = callback;
    router->transmit_context = callback ? context : NULL;
    control_unlock(router);
}
