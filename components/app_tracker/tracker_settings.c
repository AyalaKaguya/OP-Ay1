// SPDX-License-Identifier: Apache-2.0
#include "tracker_settings.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tracker_graphics.h"

#define MENU_BOTTOM 412
#define MENU_TOP_GESTURE 32
#define MENU_OPEN_TRAVEL 42
#define MENU_TAP_SLOP 12

static bool inside(int x, int y, int left, int top, int w, int h)
{
    return x >= left && x < left + w && y >= top && y < top + h;
}

void tracker_settings_init(tracker_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    app_settings_snapshot_defaults(&s->display);
    s->requested_brightness = s->display.brightness;
    s->requested_rotation = s->display.rotation;
    s->requested_locked = s->display.rotation_locked;
    s->usb.state = APP_USB_STATE_VENDOR;
}

void tracker_settings_receive_display(tracker_settings_t *s, const app_settings_snapshot_t *next)
{
    s->display = *next;
    if (s->awaiting_display && !next->busy &&
            next->brightness == s->requested_brightness &&
            next->rotation == s->requested_rotation &&
            next->rotation_locked == s->requested_locked) {
        s->awaiting_display = false;
    }
    if (!s->awaiting_display && !s->display_request && !s->brightness_drag) {
        s->requested_brightness = next->brightness;
        s->requested_rotation = next->rotation;
        s->requested_locked = next->rotation_locked;
    }
}

static void brightness(tracker_settings_t *s, int x)
{
    int percent = 5 + (x - 32) * 95 / 415;
    if (percent < 5) {
        percent = 5;
    }
    if (percent > 100) {
        percent = 100;
    }
    s->requested_brightness = percent;
    s->display_request = true;
}

static void tap(tracker_settings_t *s, int x, int y)
{
    if (s->usb_confirm) {
        if (inside(x, y, 30, 246, 206, 48)) {
            s->usb_confirm = false;
        } else if (inside(x, y, 244, 246, 206, 48)) {
            s->requested_usb = APP_USB_MODE_MIDI_HOST;
            s->usb_request = true;
            s->usb_confirm = false;
        }
        return;
    }
    if (inside(x, y, 420, 0, 60, 44) || y >= MENU_BOTTOM) {
        s->open = false;
    } else if (inside(x, y, 278, 155, 172, 44)) {
        s->requested_locked = !s->requested_locked;
        s->display_request = true;
    } else if (inside(x, y, 30, 211, 420, 44)) {
        const int column = (x - 30) / 107;
        if ((x - 30) % 107 < 99 && column < 4) {
            s->requested_rotation = column * 90;
            s->requested_locked = true;
            s->display_request = true;
        }
    } else if (inside(x, y, 30, 319, 206, 44)) {
        if (s->usb.actual_mode != APP_USB_MODE_MIDI_HOST) {
            s->usb_confirm = true;
        }
    } else if (inside(x, y, 244, 319, 206, 44)) {
        s->requested_usb = APP_USB_MODE_VENDOR;
        s->usb_request = true;
    }
}

bool tracker_settings_pointer(tracker_settings_t *s, int x, int y,
                              bool pressed, bool *out_opened)
{
    if (out_opened) {
        *out_opened = false;
    }
    /* A drag that started on a page stays with that page through release,
     * even if it subsequently crosses the menu's top activation strip. */
    if (s->page_pointer_down) {
        if (!pressed) {
            s->page_pointer_down = false;
        }
        return false;
    }
    if (pressed && !s->pointer_down) {
        if (!s->open && (y < 0 || y >= MENU_TOP_GESTURE)) {
            s->page_pointer_down = true;
            return false;
        }
        s->pointer_down = true;
        s->top_candidate = !s->open;
        s->start_x = x;
        s->start_y = y;
        s->moved = false;
        s->brightness_drag = s->open && !s->usb_confirm && inside(x, y, 25, 112, 430, 36);
        if (s->brightness_drag) {
            brightness(s, x);
        }
        return true;
    }
    if (!s->pointer_down) {
        return s->open;
    }
    const int dx = x - s->start_x;
    const int dy = y - s->start_y;
    if (abs(dx) > MENU_TAP_SLOP || abs(dy) > MENU_TAP_SLOP) {
        s->moved = true;
    }
    if (s->top_candidate && dy >= MENU_OPEN_TRAVEL && dy > abs(dx) * 2) {
        s->top_candidate = false;
        s->open = true;
        if (out_opened) {
            *out_opened = true;
        }
    }
    if (s->brightness_drag) {
        brightness(s, x);
    }
    if (!pressed) {
        const bool top = s->top_candidate;
        const bool slider = s->brightness_drag;
        s->pointer_down = false;
        s->top_candidate = false;
        s->brightness_drag = false;
        if (!top && !slider && s->open) {
            if (!s->usb_confirm && dy < -48 && abs(dy) > abs(dx) * 2) {
                s->open = false;
            } else if (!s->moved) {
                tap(s, x, y);
            }
        }
    }
    return true;
}

static const char *charge_name(app_settings_charge_t charge)
{
    switch (charge) {
    case APP_SETTINGS_CHARGE_CHARGING: return "CHARGING";
    case APP_SETTINGS_CHARGE_DISCHARGING: return "DISCHARGING";
    case APP_SETTINGS_CHARGE_IDLE: return "IDLE";
    case APP_SETTINGS_CHARGE_FULL: return "FULL";
    default: return "UNKNOWN";
    }
}

static void button(tracker_graphics_t *g, int x, int y, int w, int h,
                   const char *label, bool active, uint16_t color)
{
    tracker_gfx_button(g, x, y, w, h, label, active ? color : C_MUTED, active);
}

void tracker_settings_render(uint16_t *frame, const tracker_settings_t *s)
{
    if (!s->open) {
        return;
    }
    /* Dim the live page, then draw actual controls rather than a concept bitmap. */
    for (unsigned i = 0; i < 480U * 480U; ++i) {
        const unsigned p = frame[i];
        frame[i] = (uint16_t)(((p & 0xF800U) >> 2 & 0xF800U) |
                              ((p & 0x07E0U) >> 2 & 0x07E0U) |
                              ((p & 0x001FU) >> 2));
    }
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    tracker_gfx_clip(&g, 0, 0, 480, 480);
    tracker_gfx_rect(&g, 0, 0, 480, MENU_BOTTOM, C_BG);
    tracker_gfx_rect(&g, 0, 0, 480, 40, C_PANEL);
    tracker_gfx_text(&g, 30, 14, 2, C_TEXT, "QUICK SETTINGS");
    tracker_gfx_line(&g, 435, 14, 445, 24, C_MUTED);
    tracker_gfx_line(&g, 445, 14, 435, 24, C_MUTED);
    tracker_gfx_rect(&g, 16, 40, 448, 1, C_BORDER);
    const bool battery = s->display.battery_valid && !s->display.battery_stale;
    const uint16_t battery_color = battery && s->display.battery_percent <= 15 ? C_ACCENT :
                                   battery && s->display.charge == APP_SETTINGS_CHARGE_CHARGING ? C_GREEN : C_MUTED;
    tracker_gfx_outline(&g, 30, 54, 24, 12, battery_color);
    tracker_gfx_rect(&g, 54, 57, 3, 6, battery_color);
    if (battery) {
        tracker_gfx_rect(&g, 33, 57, 18 * s->display.battery_percent / 100, 6, battery_color);
    }
    char text[32];
    if (battery) {
        snprintf(text, sizeof(text), "%d%%", s->display.battery_percent);
    } else {
        snprintf(text, sizeof(text), "--%%");
    }
    tracker_gfx_text(&g, 70, 51, 2, C_TEXT, text);
    tracker_gfx_text(&g, 184, 56, 1, battery_color, battery ? charge_name(s->display.charge) : "UNAVAILABLE");
    if (battery) {
        snprintf(text, sizeof(text), "%.2fV", s->display.voltage_mv / 1000.0);
    } else {
        snprintf(text, sizeof(text), "--.--V");
    }
    tracker_gfx_text_right(&g, 450, 56, 1, C_MUTED, text);
    tracker_gfx_rect(&g, 16, 84, 448, 1, C_BORDER);
    tracker_gfx_text(&g, 30, 101, 1, C_MUTED, "BRIGHTNESS");
    const int shown_brightness = s->display_request || s->awaiting_display || s->brightness_drag
                                 ? s->requested_brightness : s->display.brightness;
    snprintf(text, sizeof(text), "%d%%", shown_brightness);
    tracker_gfx_text_right(&g, 450, 97, 2, C_TEXT, text);
    tracker_gfx_slider(&g, 32, 127, 416, (shown_brightness - 5) / 95.0f, C_CYAN, false);
    tracker_gfx_rect(&g, 16, 153, 448, 1, C_BORDER);
    tracker_gfx_text(&g, 30, 173, 1, C_MUTED, "ROTATION");
    button(&g, 278, 161, 172, 38, s->display.rotation_locked ? "LOCKED" : "AUTO", true,
           s->display.rotation_locked ? C_ACCENT : C_CYAN);
    static const char *const angles[] = {"0", "90", "180", "270"};
    for (int i = 0; i < 4; ++i) {
        button(&g, 30 + 107 * i, 211, 99, 44, angles[i], s->display.rotation == i * 90, C_CYAN);
    }
    tracker_gfx_text(&g, 30, 267, 1, C_MUTED, s->display.last_error ? "DISPLAY / SENSOR ERROR" :
                     s->display.busy || s->awaiting_display || s->display_request ? "APPLYING..." : "TAP ANGLE TO LOCK / AUTO TO FOLLOW");
    tracker_gfx_rect(&g, 16, 285, 448, 1, C_BORDER);
    tracker_gfx_text(&g, 30, 301, 1, C_MUTED, "USB MODE");
    button(&g, 30, 319, 206, 44, "MIDI IO", s->usb.actual_mode == APP_USB_MODE_MIDI_HOST, C_CYAN);
    button(&g, 244, 319, 206, 44, "VENDOR", s->usb.actual_mode == APP_USB_MODE_VENDOR, C_CYAN);
    const char *usb_text = "DEVICE / ESP-IRIS MANAGEMENT";
    if (s->usb.state == APP_USB_STATE_SWITCHING) {
        usb_text = "SWITCHING USB...";
    } else if (s->usb.state == APP_USB_STATE_HOST_WAITING) {
        usb_text = "HOST / WAITING FOR KEYBOARD";
    } else if (s->usb.state == APP_USB_STATE_HOST_CONNECTED) {
        usb_text = "HOST / MIDI KEYBOARD CONNECTED";
    } else if (s->usb.state == APP_USB_STATE_ERROR) {
        usb_text = "USB ERROR / TRY VENDOR";
    }
    tracker_gfx_text(&g, 30, 376, 1, s->usb.state == APP_USB_STATE_ERROR ? C_ACCENT : C_MUTED, usb_text);
    tracker_gfx_rect(&g, 16, 395, 448, 1, C_BORDER);
    tracker_gfx_rect(&g, 202, 404, 76, 3, C_MUTED);
    if (s->usb_confirm) {
        tracker_gfx_rect(&g, 16, 130, 448, 180, C_PANEL);
        tracker_gfx_outline(&g, 16, 130, 448, 180, C_CYAN);
        tracker_gfx_text(&g, 30, 149, 2, C_TEXT, "USE USB MIDI HOST?");
        tracker_gfx_text(&g, 30, 182, 1, C_MUTED, "PC USB MANAGEMENT WILL DISCONNECT.");
        tracker_gfx_text(&g, 30, 204, 1, C_MUTED, "CONNECT A USB MIDI KEYBOARD.");
        tracker_gfx_text(&g, 30, 222, 1, C_MUTED, "CHOOSE VENDOR HERE TO RECONNECT.");
        button(&g, 30, 246, 206, 48, "CANCEL", false, C_TEXT);
        button(&g, 244, 246, 206, 48, "MIDI HOST", true, C_CYAN);
    }
}
