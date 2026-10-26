// SPDX-License-Identifier: Apache-2.0
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_settings.h"

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "settings_idle: FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static app_settings_idle_target_t evaluate(app_settings_idle_t *idle,
                                           uint64_t now_ms, bool busy,
                                           int preference)
{
    app_settings_idle_target_t target;
    app_settings_idle_evaluate(idle, now_ms, busy, preference, &target);
    return target;
}

static void default_off(void)
{
    app_settings_snapshot_t snapshot;
    app_settings_snapshot_defaults(&snapshot);
    require(snapshot.dim_after_s == 0 && snapshot.off_after_s == 0 &&
            !snapshot.dimmed && !snapshot.screen_off,
            "idle timers were not both disabled by default");
    require(snapshot.brightness == 70 && snapshot.effective_brightness == 70,
            "default preference and effective brightness disagree");
    app_settings_idle_t idle;
    app_settings_idle_init(&idle, 100);
    const app_settings_idle_target_t target = evaluate(&idle, UINT64_MAX, false, 70);
    require(!target.dimmed && !target.screen_off && target.effective_brightness == 70,
            "default-disabled timers changed the display after long uptime");
}

static void thresholds_and_wake(void)
{
    app_settings_idle_t idle;
    app_settings_idle_init(&idle, 1000);
    require(app_settings_idle_configure(&idle, 5, 10, 1000), "valid timer pair rejected");
    app_settings_idle_target_t target = evaluate(&idle, 5999, false, 70);
    require(!target.dimmed && !target.screen_off && target.effective_brightness == 70,
            "DIM became active before its exact boundary");
    target = evaluate(&idle, 6000, false, 70);
    require(target.dimmed && !target.screen_off && target.effective_brightness == 10,
            "DIM did not cap output at ten percent at its boundary");
    target = evaluate(&idle, 10999, false, 70);
    require(target.dimmed && !target.screen_off, "OFF became active before its boundary");
    target = evaluate(&idle, 11000, false, 70);
    require(!target.dimmed && target.screen_off && target.effective_brightness == 0,
            "OFF did not override DIM at its exact boundary");
    app_settings_idle_record_activity(&idle, 11001);
    target = evaluate(&idle, 11001, false, 70);
    require(!target.dimmed && !target.screen_off && target.effective_brightness == 70,
            "activity did not restore the unchanged preferred brightness");
    require(idle.dim_after_s == 5 && idle.off_after_s == 10,
            "wake erased configured timers");
}

static void independent_timers_and_preference(void)
{
    app_settings_idle_t idle;
    app_settings_idle_init(&idle, 0);
    require(app_settings_idle_configure(&idle, 2, 0, 0), "DIM-only configuration rejected");
    app_settings_idle_target_t target = evaluate(&idle, 2000, false, 5);
    require(target.dimmed && !target.screen_off && target.effective_brightness == 5,
            "DIM increased an already low user brightness");
    target = evaluate(&idle, 3000, false, 0);
    require(target.dimmed && target.effective_brightness == 0,
            "DIM increased zero-percent user brightness");
    target = evaluate(&idle, 500000, false, 100);
    require(target.dimmed && !target.screen_off && target.effective_brightness == 10,
            "disabled OFF timer still turned the screen off");
    require(app_settings_idle_configure(&idle, 0, 3, 500001), "OFF-only configuration rejected");
    target = evaluate(&idle, 503000, false, 100);
    require(!target.dimmed && !target.screen_off && target.effective_brightness == 100,
            "OFF-only timer dimmed or slept early");
    target = evaluate(&idle, 503001, false, 100);
    require(target.screen_off, "OFF-only timer did not sleep");
    require(app_settings_idle_configure(&idle, 10, 1, 600000), "OFF-before-DIM pair rejected");
    target = evaluate(&idle, 601000, false, 70);
    require(target.screen_off && !target.dimmed, "earlier OFF timer did not take precedence");
    require(app_settings_idle_configure(&idle, 1, 1, 700000), "equal timers rejected");
    target = evaluate(&idle, 701000, false, 70);
    require(target.screen_off && !target.dimmed, "equal timers did not choose OFF");
}

static void interaction_and_reconfiguration(void)
{
    app_settings_idle_t idle;
    app_settings_idle_init(&idle, 0);
    require(app_settings_idle_configure(&idle, 1, 2, 0), "valid timer pair rejected");
    app_settings_idle_target_t target = evaluate(&idle, 100000, true, 70);
    require(!target.dimmed && !target.screen_off, "held interaction allowed idle blanking");
    target = evaluate(&idle, 100999, false, 70);
    require(!target.dimmed, "release reused pre-interaction idle time");
    target = evaluate(&idle, 101000, false, 70);
    require(target.dimmed, "DIM timer did not restart from interaction release");
    /* Re-reading or resubmitting an unchanged preference is not activity. */
    require(app_settings_idle_configure(&idle, 1, 2, 101999), "unchanged timer pair rejected");
    target = evaluate(&idle, 102000, false, 70);
    require(target.screen_off, "unchanged configure call incorrectly reset the idle timer");
    require(app_settings_idle_configure(&idle, 2, 4, 102001), "changed timer pair rejected");
    target = evaluate(&idle, 102001, false, 70);
    require(!target.dimmed && !target.screen_off, "new timeout configuration did not wake/restart policy");
    require(app_settings_idle_configure(&idle, 0, 0, 110000), "disable-both request rejected");
    target = evaluate(&idle, UINT64_MAX, false, 70);
    require(!target.dimmed && !target.screen_off, "disabled policy retained a previous idle phase");
    require(!app_settings_idle_configure(&idle, APP_SETTINGS_IDLE_MAX_SECONDS + 1U, 0, 0) &&
            !app_settings_idle_configure(&idle, 0, UINT32_MAX, 0) &&
            idle.dim_after_s == 0 && idle.off_after_s == 0,
            "invalid timeout changed the accepted configuration");
}

static void clock_and_restart(void)
{
    app_settings_idle_t idle;
    app_settings_idle_init(&idle, 10000);
    require(app_settings_idle_configure(&idle, 1, 2, 10000), "valid timer pair rejected");
    require(evaluate(&idle, 12000, false, 70).screen_off, "clock test did not enter OFF");
    app_settings_idle_target_t target = evaluate(&idle, 100, false, 70);
    require(!target.screen_off && !target.dimmed, "backward clock retained an old OFF deadline");
    require(!evaluate(&idle, 1099, false, 70).dimmed,
            "backward clock failed to start a fresh timer epoch");
    require(evaluate(&idle, 1100, false, 70).dimmed, "fresh clock epoch did not reach DIM");
    /* Only timer preferences survive restart, never the last dim/off phase. */
    app_settings_idle_init(&idle, 0);
    require(app_settings_idle_configure(&idle, 1, 2, 0), "restored timer preferences rejected");
    target = evaluate(&idle, 0, false, 85);
    require(target.effective_brightness == 85 && !target.dimmed && !target.screen_off,
            "restart restored an idle phase instead of preferred brightness");
    app_settings_idle_init(&idle, UINT64_MAX - 1000U);
    require(app_settings_idle_configure(&idle, 0, 1, UINT64_MAX - 1000U),
            "late-uptime timeout configuration rejected");
    require(evaluate(&idle, UINT64_MAX, false, 70).screen_off,
            "64-bit boundary overflowed the timeout calculation");
    app_settings_idle_init(&idle, 0);
    require(app_settings_idle_configure(&idle, APP_SETTINGS_IDLE_MAX_SECONDS, 0, 0),
            "maximum supported timeout rejected");
    target = evaluate(&idle, (uint64_t)APP_SETTINGS_IDLE_MAX_SECONDS * 1000U - 1U, false, 200);
    require(!target.dimmed && target.effective_brightness == 100,
            "maximum timeout or brightness clamping failed");
    target = evaluate(&idle, (uint64_t)APP_SETTINGS_IDLE_MAX_SECONDS * 1000U, false, -1);
    require(target.dimmed && target.effective_brightness == 0,
            "long timeout or lower brightness clamp failed");
}

int main(void)
{
    default_off();
    thresholds_and_wake();
    independent_timers_and_preference();
    interaction_and_reconfiguration();
    clock_and_restart();
    puts("settings_idle: disabled defaults, boundaries, wake, preference, interaction, configure and clocks PASS");
    return EXIT_SUCCESS;
}
