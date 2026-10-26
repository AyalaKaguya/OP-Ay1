// SPDX-License-Identifier: Apache-2.0
/* Native preview has no USB controller; never simulate a connected keyboard. */
#include "app_usb_midi.h"

#include <stdlib.h>
#include <string.h>

#define USB_NATIVE_NOT_SUPPORTED 0x106

struct app_usb_midi {
    app_usb_midi_status_t status;
};

bool app_usb_midi_create(app_note_router_t *router, app_usb_midi_t **out_handle)
{
    if (out_handle == NULL) {
        return false;
    }
    *out_handle = NULL;
    if (router == NULL) {
        return false;
    }
    app_usb_midi_t *usb = calloc(1, sizeof(*usb));
    if (usb == NULL) {
        return false;
    }
    usb->status.requested_mode = APP_USB_MODE_VENDOR;
    usb->status.actual_mode = APP_USB_MODE_VENDOR;
    usb->status.state = APP_USB_STATE_VENDOR;
    *out_handle = usb;
    return true;
}

bool app_usb_midi_request_mode(app_usb_midi_t *usb, app_usb_mode_t mode)
{
    if (usb == NULL || (mode != APP_USB_MODE_VENDOR && mode != APP_USB_MODE_MIDI_HOST)) {
        return false;
    }
    usb->status.requested_mode = mode;
    usb->status.last_error = mode == APP_USB_MODE_VENDOR ? 0 : USB_NATIVE_NOT_SUPPORTED;
    usb->status.state = mode == APP_USB_MODE_VENDOR ? APP_USB_STATE_VENDOR : APP_USB_STATE_ERROR;
    return mode == APP_USB_MODE_VENDOR;
}

void app_usb_midi_get_status(app_usb_midi_t *usb, app_usb_midi_status_t *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
        if (usb != NULL) {
            *status = usb->status;
        }
    }
}

bool app_usb_midi_shutdown(app_usb_midi_t **handle, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (handle != NULL) {
        free(*handle);
        *handle = NULL;
    }
    return true;
}
