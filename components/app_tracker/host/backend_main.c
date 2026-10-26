// SPDX-License-Identifier: Apache-2.0
/* Own the native lifecycle so a normal duration exit tears down application
 * callbacks before the simulator closes the connection. */
#include "gsp_sim_bridge.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stopping;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

int main(int argc, char **argv)
{
    const char *endpoint = NULL;
    uint64_t duration_ms = 0;
    for (int arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--endpoint") && arg + 1 < argc) {
            endpoint = argv[++arg];
        } else if (!strcmp(argv[arg], "--duration-ms") && arg + 1 < argc) {
            duration_ms = strtoull(argv[++arg], NULL, 10);
        } else {
            fprintf(stderr, "usage: %s --endpoint tcp://127.0.0.1:PORT [--duration-ms N]\n", argv[0]);
            return 2;
        }
    }
    if (!endpoint) {
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    esp_gsp_handle_t ui = NULL;
    esp_gsp_err_t result = gsp_sim_bridge_open(endpoint, &ui);
    if (result != ESP_GSP_OK) {
        return 1;
    }
    result = gsp_bridge_app_init(ui);
    const char *stop_file = getenv("OP_AY1_SIM_STOP_FILE");
    const uint64_t start = gsp_sim_bridge_time_ms();
    while (result == ESP_GSP_OK && !stopping &&
            (!duration_ms || gsp_sim_bridge_time_ms() - start < duration_ms)) {
        result = gsp_sim_bridge_poll(ui, 20);
        if (stop_file && *stop_file) {
            FILE *requested = fopen(stop_file, "rb");
            if (requested) {
                (void)fclose(requested);
                break;
            }
        }
    }
    gsp_bridge_app_deinit(ui);
    gsp_sim_bridge_close(ui);
    if (result != ESP_GSP_OK) {
        fprintf(stderr, "op_ay1_backend: bridge failed (%d)\n", result);
    }
    return result == ESP_GSP_OK ? 0 : 1;
}
