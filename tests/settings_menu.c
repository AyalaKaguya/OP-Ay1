// SPDX-License-Identifier: Apache-2.0
#include "tracker_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void tap(tracker_settings_t *s, int x, int y)
{
    bool opened;
    assert(tracker_settings_pointer(s, x, y, true, &opened));
    assert(tracker_settings_pointer(s, x, y, false, &opened));
}

int main(void)
{
    tracker_settings_t s;
    tracker_settings_init(&s);
    bool opened = false;
    assert(!tracker_settings_pointer(&s, 200, 200, true, &opened));
    assert(!s.open);
    assert(!tracker_settings_pointer(&s, 220, 12, true, &opened));
    assert(!tracker_settings_pointer(&s, 220, 12, false, &opened));
    assert(!s.open && !s.page_pointer_down);
    assert(tracker_settings_pointer(&s, 220, 12, true, &opened));
    assert(!s.open);
    assert(tracker_settings_pointer(&s, 225, 70, true, &opened));
    assert(opened && s.open);
    assert(tracker_settings_pointer(&s, 225, 70, false, &opened));
    tap(&s, 32, 127);
    assert(s.display_request && s.requested_brightness == 5);
    s.display_request = false;
    tap(&s, 447, 127);
    assert(s.requested_brightness == 100);
    tap(&s, 180, 233);
    assert(s.requested_rotation == 90 && s.requested_locked);
    /* Requests are queued independently of the display worker's publication. */
    s.display_request = false;
    s.awaiting_display = true;
    tracker_settings_receive_display(&s, &s.display);
    assert(s.awaiting_display && s.requested_rotation == 90);
    tap(&s, 300, 127);
    assert(s.requested_rotation == 90 && s.requested_locked);
    tap(&s, 370, 180);
    assert(!s.requested_locked);
    tap(&s, 370, 180);
    assert(s.requested_locked && s.requested_rotation == 90);
    s.display_request = false;
    app_settings_snapshot_t applied = s.display;
    applied.brightness = s.requested_brightness;
    applied.rotation = s.requested_rotation;
    applied.rotation_locked = s.requested_locked;
    tracker_settings_receive_display(&s, &applied);
    assert(!s.awaiting_display && s.requested_rotation == 90);
    tap(&s, 130, 340);
    assert(s.usb_confirm && !s.usb_request);
    tap(&s, 130, 270);
    assert(!s.usb_confirm && !s.usb_request);
    tap(&s, 130, 340);
    tap(&s, 330, 270);
    assert(s.usb_request && s.requested_usb == APP_USB_MODE_MIDI_HOST);
    s.usb_request = false;
    tap(&s, 350, 340);
    assert(s.usb_request && s.requested_usb == APP_USB_MODE_VENDOR);
    uint16_t *frame = calloc(480U * 480U + 2U, sizeof(*frame));
    assert(frame);
    frame[0] = 0xDEAD;
    frame[480U * 480U + 1U] = 0xBEEF;
    tracker_settings_render(frame + 1, &s);
    assert(frame[0] == 0xDEAD && frame[480U * 480U + 1U] == 0xBEEF);
    tap(&s, 440, 20);
    assert(!s.open);
    assert(!tracker_settings_pointer(&s, 50, 200, true, &opened));
    free(frame);
    puts("settings menu: gesture capture, controls, USB confirmation and framebuffer guards PASS");
    return 0;
}
