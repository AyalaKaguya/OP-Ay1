// SPDX-License-Identifier: Apache-2.0
#include "audio_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#endif

#define HOST_CHUNKS 4

struct host_audio {
    app_audio_bus_t *bus;
    int16_t chunks[HOST_CHUNKS][APP_AUDIO_BLOCK_FRAMES * 2];
    uint64_t previous_ms;
    uint64_t frames_due;
    FILE *capture;
    uint32_t capture_frames;
#if defined(_WIN32)
    HWAVEOUT device;
    WAVEHDR headers[HOST_CHUNKS];
    unsigned prepared;
#endif
};

static void put_u32(FILE *f, uint32_t value)
{
    const uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8),
                             (uint8_t)(value >> 16), (uint8_t)(value >> 24)
                            };
    (void)fwrite(bytes, sizeof(bytes), 1, f);
}

static void wav_header(host_audio_t *audio)
{
    FILE *f = audio->capture;
    const uint32_t bytes = audio->capture_frames * 4;
    rewind(f);
    (void)fwrite("RIFF", 4, 1, f);
    put_u32(f, bytes + 36);
    (void)fwrite("WAVEfmt ", 8, 1, f);
    put_u32(f, 16);
    const uint8_t format[] = {1, 0, 2, 0};
    (void)fwrite(format, 4, 1, f);
    put_u32(f, APP_AUDIO_SAMPLE_RATE);
    put_u32(f, APP_AUDIO_SAMPLE_RATE * 4);
    const uint8_t size[] = {4, 0, 16, 0};
    (void)fwrite(size, 4, 1, f);
    (void)fwrite("data", 4, 1, f);
    put_u32(f, bytes);
}

host_audio_t *host_audio_create(app_audio_bus_t *bus)
{
    host_audio_t *audio = calloc(1, sizeof(*audio));
    if (!audio) {
        return NULL;
    }
    audio->bus = bus;
    const char *path = getenv("OP_AY1_SIM_WAV");
    if (path && *path) {
        audio->capture = fopen(path, "wb");
        if (!audio->capture) {
            free(audio);
            return NULL;
        }
        wav_header(audio);
    }
#if defined(_WIN32)
    const char *audible = getenv("OP_AY1_SIM_AUDIO");
    if (audible && strcmp(audible, "1") == 0) {
        WAVEFORMATEX format = {0};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = APP_AUDIO_SAMPLE_RATE;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = APP_AUDIO_SAMPLE_RATE * 4;
        if (waveOutOpen(&audio->device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            fprintf(stderr, "op_ay1_backend: audio output unavailable; using clocked sink\n");
            audio->device = NULL;
        } else {
            for (unsigned i = 0; i < HOST_CHUNKS; ++i) {
                audio->headers[i].lpData = (LPSTR)audio->chunks[i];
                audio->headers[i].dwBufferLength = sizeof(audio->chunks[i]);
                if (waveOutPrepareHeader(audio->device, &audio->headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                    host_audio_destroy(audio);
                    return NULL;
                }
                ++audio->prepared;
            }
        }
    }
#endif
    return audio;
}

static void render_chunk(host_audio_t *audio, unsigned slot)
{
    app_audio_bus_render(audio->bus, audio->chunks[slot], APP_AUDIO_BLOCK_FRAMES);
    if (audio->capture && audio->capture_frames < UINT32_MAX / 4 - APP_AUDIO_BLOCK_FRAMES) {
        if (fwrite(audio->chunks[slot], sizeof(audio->chunks[slot]), 1, audio->capture) == 1) {
            audio->capture_frames += APP_AUDIO_BLOCK_FRAMES;
        }
    }
}

void host_audio_step(host_audio_t *audio, uint64_t time_ms)
{
    if (!audio) {
        return;
    }
    if (audio->previous_ms == 0) {
        audio->previous_ms = time_ms;
        return;
    }
    const uint64_t elapsed = time_ms - audio->previous_ms;
    audio->previous_ms = time_ms;
    audio->frames_due += (elapsed > 100 ? 100 : elapsed) * 48;
    /* Bounded catch-up prevents a slow RPC frame from freezing the UI. */
    for (unsigned slot = 0; slot < HOST_CHUNKS && audio->frames_due >= APP_AUDIO_BLOCK_FRAMES; ++slot) {
#if defined(_WIN32)
        if (audio->device && (audio->headers[slot].dwFlags & WHDR_INQUEUE)) {
            continue;
        }
#endif
        render_chunk(audio, slot);
        audio->frames_due -= APP_AUDIO_BLOCK_FRAMES;
#if defined(_WIN32)
        if (audio->device && waveOutWrite(audio->device, &audio->headers[slot], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            fprintf(stderr, "op_ay1_backend: audio write failed\n");
        }
#endif
    }
    if (audio->frames_due > APP_AUDIO_BLOCK_FRAMES * HOST_CHUNKS) {
        audio->frames_due = APP_AUDIO_BLOCK_FRAMES * HOST_CHUNKS;
    }
}

void host_audio_volume(host_audio_t *audio, int percent)
{
    (void)audio;
    (void)percent;
#if defined(_WIN32)
    if (audio && audio->device) {
        const DWORD value = (DWORD)percent * 65535u / 100u;
        (void)waveOutSetVolume(audio->device, value | value << 16);
    }
#endif
}

void host_audio_destroy(host_audio_t *audio)
{
    if (!audio) {
        return;
    }
#if defined(_WIN32)
    if (audio->device) {
        (void)waveOutReset(audio->device);
        for (unsigned i = 0; i < audio->prepared; ++i) {
            (void)waveOutUnprepareHeader(audio->device, &audio->headers[i], sizeof(WAVEHDR));
        }
        (void)waveOutClose(audio->device);
    }
#endif
    if (audio->capture) {
        wav_header(audio);
        (void)fclose(audio->capture);
    }
    free(audio);
}
