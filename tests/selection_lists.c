// SPDX-License-Identifier: Apache-2.0
/* Production workspace selector scroll ownership and filtered-index checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracker_workspace.h"
#include "tracker_graphics.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "selection_lists:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static void pointer(tracker_workspace_t *w, tracker_model_t *m, tracker_settings_t *s,
                    int x, int y, bool down)
{
    if (!tracker_workspace_pointer(w, m, s, x, y, down)) {
        fprintf(stderr, "selection_lists:%d: pointer rejected\n", __LINE__);
        exit(1);
    }
}

int main(void)
{
    tracker_workspace_t w = {0};
    tracker_model_t model = {0};
    tracker_settings_t settings = {0};
    tracker_workspace_init(&w, NULL, NULL);
    tracker_settings_init(&settings);
    w.screen = TRACKER_WORKSPACE_PROJECTS;
    w.project_view.entry_count = APP_PROJECT_LIST_MAX;
    for (unsigned i = 0; i < APP_PROJECT_LIST_MAX; ++i) {
        snprintf(w.project_view.entries[i].id, APP_PROJECT_ID_BYTES, "id-%02u", i);
        snprintf(w.project_view.entries[i].name, APP_PROJECT_NAME_BYTES, "PROJECT %02u", i);
        w.project_view.entries[i].bytes = (uint64_t)(i + 1U) * 1000U;
    }
    pointer(&w, &model, &settings, 100, 200, true);
    pointer(&w, &model, &settings, 100, 163, true);
    pointer(&w, &model, &settings, 100, 163, false);
    CHECK(w.project_scroll == 37 && w.selected_project == 0);
    pointer(&w, &model, &settings, 100, 257, true);
    pointer(&w, &model, &settings, 100, 257, false);
    CHECK(w.selected_project == 2);
    pointer(&w, &model, &settings, 100, 200, true);
    pointer(&w, &model, &settings, 100, -3000, true);
    pointer(&w, &model, &settings, 100, -3000, false);
    CHECK(w.project_scroll == (int)(APP_PROJECT_LIST_MAX * TRACKER_PROJECT_LIST_ROW_HEIGHT - TRACKER_PROJECT_LIST_HEIGHT));
    CHECK(w.selected_project == 2);
    w.show_recent = false;
    w.recent_count = 2;
    strcpy(w.recent_ids[0], "id-12");
    strcpy(w.recent_ids[1], "id-29");
    pointer(&w, &model, &settings, 320, 110, true);
    pointer(&w, &model, &settings, 320, 110, false);
    tracker_workspace_poll(&w, &model);
    CHECK(w.project_scroll == 0 && tracker_workspace_project_count(&w) == 2);
    CHECK(tracker_workspace_project_entry(&w, 1) == &w.project_view.entries[29]);

    w.screen = TRACKER_WORKSPACE_IMPORT;
    w.show_recent = false;
    w.import_wav = false;
    w.project_view.inbox_count = APP_PROJECT_LIST_MAX;
    for (unsigned i = 0; i < APP_PROJECT_LIST_MAX; ++i) {
        snprintf(w.project_view.inbox[i].filename, APP_PROJECT_FILENAME_BYTES, "file-%02u.s3m", i);
        w.project_view.inbox[i].bytes = i * 100U;
        w.project_view.inbox[i].kind = i % 2 ? APP_PROJECT_INBOX_SAMPLE : APP_PROJECT_INBOX_MODULE;
    }
    pointer(&w, &model, &settings, 100, 230, true);
    pointer(&w, &model, &settings, 100, 177, true);
    pointer(&w, &model, &settings, 100, 177, false);
    CHECK(w.inbox_scroll == 53 && w.selected_inbox == 0);
    pointer(&w, &model, &settings, 340, 110, true);
    pointer(&w, &model, &settings, 340, 110, false);
    CHECK(w.import_wav && w.inbox_scroll == 0 && w.selected_inbox == 0);
    CHECK(tracker_workspace_inbox_count(&w) == 16);
    CHECK(tracker_workspace_inbox_entry(&w, 0)->kind == APP_PROJECT_INBOX_SAMPLE);
    pointer(&w, &model, &settings, 100, 230, true);
    pointer(&w, &model, &settings, 100, 193, true);
    CHECK(w.inbox_scroll == 37);
    tracker_workspace_cancel_input(&w);
    pointer(&w, &model, &settings, 100, 193, false);
    CHECK(!w.pointer_down && !w.selection_list_pointer && w.selected_inbox == 0);
    w.inbox_scroll = 500; /* A previously populated directory was shortened. */
    w.project_view.inbox_count = 1;
    pointer(&w, &model, &settings, 100, 230, true);
    pointer(&w, &model, &settings, 100, 230, false);
    CHECK(w.inbox_scroll == 0 && tracker_workspace_inbox_count(&w) == 0);

    uint16_t *frame = calloc(480U * 480U, sizeof(*frame));
    CHECK(frame != NULL);
    tracker_workspace_render(frame, &w, &model);
    free(frame);
    puts("selection lists: continuous project/inbox scroll, precise offsets, filtered rows, bounds and no accidental selection PASS");
    return 0;
}
