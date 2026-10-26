// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
typedef void *esp_codec_dev_handle_t;
typedef struct {
    int sample_rate;
    int bits_per_sample;
    int channel;
} esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_OK 0
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
int esp_codec_dev_open(esp_codec_dev_handle_t handle,
                         const esp_codec_dev_sample_info_t *format);
int esp_codec_dev_close(esp_codec_dev_handle_t handle);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t handle, int volume);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t handle, bool muted);
int esp_codec_dev_write(esp_codec_dev_handle_t handle, void *data, int size);
