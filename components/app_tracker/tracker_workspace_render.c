// SPDX-License-Identifier: Apache-2.0
#include "tracker_workspace.h"

#include <stdio.h>
#include <string.h>
#include "tracker_graphics.h"

static const char *screen_name(const tracker_workspace_t *w, const tracker_model_t *m)
{
    static const char *const pages[] = {"WAVES", "PATTERN", "PLAYBACK", "MIXER", "SECTION PADS", "SYNTH"};
    if (w->screen == TRACKER_WORKSPACE_LEGACY) {
        return m->view.page >= 0 && m->view.page < 6 ? pages[m->view.page] : "WORKSTATION";
    }
    switch (w->screen) {
    case TRACKER_WORKSPACE_HOME: return "OP-AY1";
    case TRACKER_WORKSPACE_PROJECTS: return "OPEN PROJECT";
    case TRACKER_WORKSPACE_NEW: return "NEW PROJECT";
    case TRACKER_WORKSPACE_IMPORT: return "IMPORT";
    case TRACKER_WORKSPACE_NAME: return "SAVE AS";
    case TRACKER_WORKSPACE_SAVE: return "SAVE PROJECT";
    case TRACKER_WORKSPACE_SWITCH: return "UNSAVED CHANGES";
    case TRACKER_WORKSPACE_ERROR: return "PROJECT ERROR";
    case TRACKER_WORKSPACE_STORAGE: return "STORAGE";
    case TRACKER_WORKSPACE_SETTINGS: return "SETTINGS";
    case TRACKER_WORKSPACE_SAMPLER: return "SAMPLER";
    default: return "OP-AY1";
    }
}

static void label(tracker_graphics_t *g, int x, int y, const char *text)
{
    char line[68];
    tracker_workspace_pixel_name(line, sizeof(line), text, 66);
    tracker_gfx_text(g, x, y, 1, C_MUTED, line);
}

static void button(tracker_graphics_t *g, int x, int y, int width, int height,
                   const char *text, bool active)
{
    tracker_gfx_button(g, x, y, width, height, text, active ? C_CYAN : C_MUTED, active);
}

void tracker_workspace_render_header(uint16_t *frame, const tracker_workspace_t *w,
                                     const tracker_model_t *model)
{
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    tracker_gfx_clip(&g, 0, 0, 480, 480);
    tracker_gfx_rect(&g, 0, 0, 480, 40, C_PANEL);
    tracker_gfx_text_center(&g, 240, 14, 2, C_TEXT, screen_name(w, model));
    if (w->screen == TRACKER_WORKSPACE_SETTINGS) {
        tracker_gfx_text_right(&g, 456, 16, 1, C_CYAN, "STORAGE");
    } else {
        tracker_gfx_text(&g, 444, 16, 1, C_MUTED, "...");
    }
    tracker_gfx_rect(&g, 16, 39, 448, 1, C_BORDER);
    tracker_gfx_rect(&g, 0, 40, 480, 38, C_BG);
    tracker_gfx_outline(&g, 24, 51, 18, 12, C_MUTED);
    tracker_gfx_line(&g, 24, 48, 31, 48, C_MUTED);
    tracker_gfx_line(&g, 31, 48, 35, 51, C_MUTED);
    char name[27];
    tracker_workspace_pixel_name(name, sizeof(name), w->project_view.opened ?
                                 w->project_view.name : "NO PROJECT", 23);
    const size_t length = strlen(name);
    if (w->project_view.dirty && length + 2U < sizeof(name)) {
        memcpy(name + length, " *", 3);
    }
    tracker_gfx_text(&g, 54, 51, 2, C_TEXT, name);
    tracker_gfx_text_right(&g, 456, 55, 1,
                           tracker_workspace_busy(w) ? C_FX : w->project_view.dirty ? C_CYAN : C_MUTED,
                           tracker_workspace_busy(w) ? "BUSY" : !w->project_view.opened ? "--" :
                           w->project_view.dirty ? "SAVE" : "SAVED");
    tracker_gfx_rect(&g, 16, 77, 448, 1, C_BORDER);
    tracker_gfx_rect(&g, 0, 432, 480, 48, C_BG);
}

static void tile_icon(tracker_graphics_t *g, int x, int y, int app)
{
    const uint16_t color = app == 2 || app == 6 ? C_ACCENT : app == 4 ? C_FX : C_CYAN;
    if (app == 0) {
        tracker_gfx_outline(g, x, y + 8, 46, 28, color);
        tracker_gfx_line(g, x, y + 2, x + 16, y + 2, color);
        tracker_gfx_line(g, x + 16, y + 2, x + 25, y + 8, color);
    } else if (app == 1) {
        tracker_gfx_outline(g, x + 7, y + 5, 30, 30, C_MUTED);
        tracker_gfx_outline(g, x + 15, y + 13, 14, 14, C_MUTED);
        for (int i = 0; i < 4; ++i) {
            tracker_gfx_line(g, x + 10 + i * 8, y, x + 10 + i * 8, y + 5, C_MUTED);
            tracker_gfx_line(g, x + 10 + i * 8, y + 35, x + 10 + i * 8, y + 42, C_MUTED);
        }
    } else if (app == 2) {
        for (int i = 0; i < 36; ++i) {
            tracker_gfx_line(g, x + i, y + i / 2, x + i, y + 36 - i / 2, color);
        }
    } else if (app == 3) {
        for (int i = 0; i < 3; ++i) {
            tracker_gfx_line(g, x + i * 15, y, x + i * 15, y + 42, color);
            tracker_gfx_outline(g, x + i * 15 - 4, y + 10 + i * 8, 9, 5, color);
        }
    } else if (app == 4) {
        for (int i = 0; i < 9; ++i) {
            tracker_gfx_outline(g, x + i % 3 * 15, y + i / 3 * 15, 10, 10, color);
        }
    } else if (app == 5) {
        for (int i = 0; i < 7; ++i) {
            tracker_gfx_outline(g, x + i * 6, y + 3, 6, 38, color);
        }
        for (int i = 0; i < 5; ++i) {
            tracker_gfx_rect(g, x + 5 + i * 7, y + 3, 4, 24, C_BG);
        }
    } else if (app == 7) {
        tracker_gfx_text(g, x, y, 1, color, "C4 01 ..");
        tracker_gfx_text(g, x, y + 11, 1, color, ".. 02 C4");
        tracker_gfx_text(g, x, y + 22, 1, color, "C4 01 ..");
    } else {
        for (int i = 0; i < 10; ++i) {
            const int height = 6 + (i * 13 + 7) % 35;
            tracker_gfx_rect(g, x + i * 5, y + 22 - height / 2, 2, height, color);
        }
    }
}

static void home(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    static const char *const titles[] = {"OPEN PROJECT", "SETTINGS", "PLAYBACK", "MIXER", "PADS", "SYNTH", "SAMPLER", "PATTERN", "WAVES"};
    for (int cell = 0; cell < 4; ++cell) {
        const int app = w->home_page * 4 + cell;
        const int x = cell % 2 ? 248 : 24;
        const int y = cell / 2 ? 262 : 96;
        tracker_gfx_rect(g, x, y, 208, 152, app < 9 ? C_PANEL : C_BG);
        tracker_gfx_outline(g, x, y, 208, 152, app < 9 ? C_BORDER : C_LINE);
        if (app >= 9) {
            continue;
        }
        tile_icon(g, x + 22, y + 30, app);
        tracker_gfx_text(g, x + 16, y + 105, 2, C_TEXT, titles[app]);
    }
}

static void projects(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    button(g, 24, 92, 124, 36, "+ NEW", true);
    button(g, 156, 92, 124, 36, "IMPORT", false);
    button(g, 288, 92, 80, 36, w->show_recent ? "ALL" : "RECENT", w->show_recent);
    button(g, 376, 92, 80, 36, "REFRESH", false);
    for (int row = 0; row < TRACKER_WORKSPACE_ROWS; ++row) {
        const unsigned index = (unsigned)(w->project_first + row);
        const int y = 140 + row * 50;
        if (index >= tracker_workspace_project_count(w)) {
            break;
        }
        const app_project_entry_t *entry = tracker_workspace_project_entry(w, index);
        if (!entry) {
            char unavailable[34];
            tracker_workspace_pixel_name(unavailable, sizeof(unavailable), w->recent_names[index], 28);
            tracker_gfx_outline(g, 24, y, 432, 45, C_DISABLED);
            tracker_gfx_text(g, 40, y + 9, 2, C_DISABLED, unavailable);
            label(g, 40, y + 30, "UNAVAILABLE");
            continue;
        }
        const bool current = w->project_view.id[0] && strcmp(entry->id, w->project_view.id) == 0;
        char name[34];
        tracker_workspace_pixel_name(name, sizeof(name), entry->name, 28);
        tracker_gfx_rect(g, 24, y, 432, 45, C_PANEL);
        tracker_gfx_outline(g, 24, y, 432, 45, w->selected_project == (int)index ? C_CYAN : C_BORDER);
        tracker_gfx_text(g, 40, y + 9, 2, current ? C_CYAN : C_TEXT, name);
        char detail[56];
        snprintf(detail, sizeof(detail), "REV %llu / %llu KB", (unsigned long long)entry->saved_revision,
                 (unsigned long long)(entry->bytes / 1024U));
        label(g, 40, y + 30, detail);
        tracker_gfx_text_right(g, 442, y + 31, 1, C_CYAN, current ? "CURRENT" : ">");
    }
    if (!tracker_workspace_project_count(w)) label(g, 40, 194,
                                                       w->show_recent ? "NO RECENT PROJECTS" :
                                                       w->project_view.storage_ready ? "NO PROJECTS" : "NAND UNAVAILABLE");
    button(g, 24, 342, 76, 28, "<", false);
    button(g, 380, 342, 76, 28, ">", false);
    char page[48];
    snprintf(page, sizeof(page), "%u %s%s", tracker_workspace_project_count(w),
             w->show_recent ? "RECENT" : "PROJECTS",
             !w->show_recent && w->project_view.entries_truncated ? " / LIMITED" : "");
    label(g, 126, 353, page);
    if (w->message[0]) {
        label(g, 24, 366, w->message);
    }
    button(g, 24, 378, 208, 44, "OPEN", true);
    button(g, 248, 378, 208, 44, "SAVE AS", false);
}

static void keyboard(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    static const char *const letters[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const char *const numbers[] = {"1234567890", "-_.", ""};
    tracker_gfx_rect(g, 24, 124, 432, 46, C_PANEL);
    tracker_gfx_outline(g, 24, 124, 432, 46, C_CYAN);
    tracker_gfx_text(g, 38, 139, 2, C_TEXT, w->typed_name);
    for (int row = 0; row < 3; ++row) {
        const char *keys = w->numeric_keys ? numbers[row] : letters[row];
        for (int column = 0; keys[column]; ++column) {
            const char key[] = {keys[column], '\0'};
            button(g, 24 + column * 43, 196 + row * 47, 38, 38, key, false);
        }
    }
    button(g, 325, 290, 131, 38, "DEL", false);
    button(g, 24, 342, 100, 28, w->numeric_keys ? "ABC" : "123", false);
    button(g, 135, 342, 190, 28, "SPACE", false);
    button(g, 24, 378, 208, 44, "CANCEL", false);
    button(g, 248, 378, 208, 44, "SAVE AS", true);
}

static void import_files(tracker_graphics_t *g, const tracker_workspace_t *w)
{
    button(g, 24, 92, 208, 36, "S3M", !w->import_wav);
    button(g, 248, 92, 208, 36, "WAV SAMPLES", w->import_wav);
    if (!w->import_wav) {
        button(g, 24, 138, 432, 42, "IMPORT BUILT-IN ARYX", true);
    }
    int filtered = 0;
    int visible = 0;
    for (uint32_t i = 0; i < w->project_view.inbox_count && i < APP_PROJECT_LIST_MAX; ++i) {
        const app_project_inbox_entry_t *entry = &w->project_view.inbox[i];
        if ((entry->kind == APP_PROJECT_INBOX_SAMPLE) != w->import_wav) {
            continue;
        }
        if (filtered >= w->inbox_first && visible < 3) {
            char name[34];
            tracker_workspace_pixel_name(name, sizeof(name), entry->filename, 31);
            button(g, 24, 190 + visible * 50, 432, 44, name, filtered == w->selected_inbox);
            ++visible;
        }
        ++filtered;
    }
    if (!visible) {
        label(g, 24, 224, "NO FILES");
    }
    button(g, 24, 342, 76, 28, "<", false);
    button(g, 380, 342, 76, 28, ">", false);
    button(g, 24, 378, 432, 44, w->import_wav ? "IMPORT SAMPLE" : "IMPORT PROJECT", true);
}

void tracker_workspace_render(uint16_t *frame, const tracker_workspace_t *w,
                              const tracker_model_t *model)
{
    tracker_graphics_t g;
    tracker_gfx_init(&g, frame);
    tracker_gfx_rect(&g, 0, 0, 480, 432, C_BG);
    if (w->screen == TRACKER_WORKSPACE_HOME) {
        home(&g, w);
    } else if (w->screen == TRACKER_WORKSPACE_PROJECTS) {
        projects(&g, w);
    } else if (w->screen == TRACKER_WORKSPACE_NEW) {
        button(&g, 24, 132, 432, 96, "BLANK PROJECT", true);
        button(&g, 24, 248, 432, 96, "ARYX EXAMPLE", false);
    } else if (w->screen == TRACKER_WORKSPACE_IMPORT) {
        import_files(&g, w);
    } else if (w->screen == TRACKER_WORKSPACE_NAME) {
        keyboard(&g, w);
    } else if (w->screen == TRACKER_WORKSPACE_SWITCH) {
        tracker_gfx_text(&g, 24, 132, 3, C_ACCENT, "UNSAVED CHANGES");
        char name[32];
        tracker_workspace_pixel_name(name, sizeof(name), w->pending_switch.name, 28);
        tracker_gfx_text(&g, 24, 202, 2, C_TEXT, name);
        button(&g, 24, 250, 432, 48, "SAVE + SWITCH", true);
        button(&g, 24, 310, 432, 48, "DISCARD + SWITCH", false);
        button(&g, 24, 370, 432, 48, "CANCEL", false);
    } else if (w->screen == TRACKER_WORKSPACE_SAVE) {
        tracker_gfx_text(&g, 24, 132, 3, C_CYAN, "SAVE PROJECT");
        button(&g, 24, 292, 208, 48, "CANCEL", false);
        button(&g, 248, 292, 208, 48, "SAVE", true);
    } else if (w->screen == TRACKER_WORKSPACE_ERROR) {
        tracker_gfx_text(&g, 24, 132, 3, C_ACCENT, "OPERATION FAILED");
        label(&g, 24, 182, app_project_error_name(w->ui_error));
        button(&g, 24, 310, 208, 48, "RETRY", true);
        button(&g, 248, 310, 208, 48, "BACK", false);
    } else if (w->screen == TRACKER_WORKSPACE_STORAGE) {
        tracker_gfx_text(&g, 24, 112, 2, C_TEXT, "NAND");
        label(&g, 24, 145, w->project_view.storage_ready ? "READY" : "UNAVAILABLE");
        char bytes[64];
        if (w->project_view.storage_usage_valid) {
            snprintf(bytes, sizeof(bytes), "%llu / %llu KB USED",
                     (unsigned long long)(w->project_view.storage_used_bytes / 1024U),
                     (unsigned long long)(w->project_view.storage_total_bytes / 1024U));
            label(&g, 24, 188, bytes);
        } else {
            label(&g, 24, 188, "CAPACITY UNKNOWN");
        }
        button(&g, 24, 374, 432, 44, "BACK TO SETTINGS", false);
    } else if (w->screen == TRACKER_WORKSPACE_SAMPLER) {
        tracker_sampler_ui_render(frame, w);
    }
    if (w->message[0] && w->screen != TRACKER_WORKSPACE_HOME &&
            w->screen != TRACKER_WORKSPACE_SAMPLER && w->screen != TRACKER_WORKSPACE_PROJECTS &&
            w->screen != TRACKER_WORKSPACE_IMPORT && w->screen != TRACKER_WORKSPACE_NAME) {
        label(&g, 24, 405, w->message);
    }
    if (tracker_workspace_busy(w)) {
        tracker_gfx_rect(&g, 24, 167, 432, 104, C_PANEL);
        tracker_gfx_outline(&g, 24, 167, 432, 104, C_CYAN);
        tracker_gfx_text_center(&g, 240, 190, 2, C_CYAN, "WORKING...");
    }
    (void)model;
}
