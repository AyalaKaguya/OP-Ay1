// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_note_router.h"

typedef struct app_usb_midi app_usb_midi_t;
typedef enum {
    APP_USB_MODE_NONE = -1, /* Neither driver owns the port after a failure. */
    APP_USB_MODE_VENDOR = 0, /* ESP-Iris CDC management device. */
    APP_USB_MODE_MIDI_HOST = 1,
} app_usb_mode_t;

typedef enum {
    APP_USB_STATE_VENDOR,
    APP_USB_STATE_SWITCHING,
    APP_USB_STATE_HOST_WAITING,
    APP_USB_STATE_HOST_CONNECTED,
    APP_USB_STATE_ERROR,
} app_usb_state_t;

typedef struct {
    app_usb_mode_t requested_mode;
    app_usb_mode_t actual_mode;
    app_usb_state_t state;
    bool connected;
    bool midi_in;
    bool midi_out;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t received_packets;
    uint32_t sent_packets;
    uint32_t dropped_packets;
    int32_t last_error; /* ESP error code on device; zero on success. */
} app_usb_midi_status_t;

/* Borrows router. Vendor is the default on each boot. Device startup creates
 * one USB owner worker; no transition runs on a UI or Iris callback thread.
 * Returns false on allocation/task creation failure; out_handle is then NULL. */
bool app_usb_midi_create(app_note_router_t *router, app_usb_midi_t **out_handle);
bool app_usb_midi_request_mode(app_usb_midi_t *usb, app_usb_mode_t mode);
void app_usb_midi_get_status(app_usb_midi_t *usb, app_usb_midi_status_t *status);
/* Requests Vendor, waits for complete host teardown and Iris USB resume.
 * On false the non-NULL handle and all borrowed resources must remain alive.
 * Called by a control task, never a UI/USB/Iris callback. */
bool app_usb_midi_shutdown(app_usb_midi_t **handle, uint32_t timeout_ms);
