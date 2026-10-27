// SPDX-License-Identifier: Apache-2.0
#include "sampler_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAV_CONVERT_FRAMES 256U
#define WAV_MAX_FILE_BYTES (APP_SAMPLER_MAX_FRAMES * 4U + 1024U * 1024U)

typedef struct {
    const uint8_t *memory;
    FILE *file;
    size_t size;
} wav_reader_t;
static bool read_at(wav_reader_t *reader, size_t offset, void *out, size_t count)
{
    if (offset > reader->size || count > reader->size - offset) { return false; }
    if (reader->memory) { memcpy(out, reader->memory + offset, count); return true; }
    return fseek(reader->file, (long)offset, SEEK_SET) == 0 &&
           fread(out, 1, count, reader->file) == count;
}
static void sample_name(char out[APP_SAMPLER_NAME_BYTES], const char *name)
{
    if (!name) { name = "Imported sample"; }
    const char *base = name;
    for (const char *p = name; *p; ++p) { if (*p == '/' || *p == '\\') { base = p + 1; } }
    snprintf(out, APP_SAMPLER_NAME_BYTES, "%s", base);
}
static app_sampler_result_t import_wav(app_sampler_t *sampler, wav_reader_t *reader,
                                       const char *name, unsigned *index)
{
    uint8_t header[16];
    if (!read_at(reader, 0, header, 12) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        return APP_SAMPLER_FORMAT;
    }
    const uint64_t riff_end = (uint64_t)sampler_u32(header + 4) + 8U;
    if (riff_end > reader->size || riff_end < 12U) { return APP_SAMPLER_FORMAT; }
    size_t cursor = 12, data_offset = 0, data_bytes = 0;
    uint32_t rate = 0, bytes_per_second = 0;
    unsigned channels = 0, bits = 0, align = 0;
    bool have_format = false, have_data = false;
    while (cursor + 8U <= riff_end) {
        if (!read_at(reader, cursor, header, 8)) { return APP_SAMPLER_IO; }
        const uint32_t count = sampler_u32(header + 4);
        const uint64_t next = (uint64_t)cursor + 8U + count + (count & 1U);
        if (next > riff_end) { return APP_SAMPLER_FORMAT; }
        if (!memcmp(header, "fmt ", 4)) {
            if (have_format || count < 16U || !read_at(reader, cursor + 8U, header, 16)) {
                return APP_SAMPLER_FORMAT;
            }
            if (sampler_u16(header) != 1) { return APP_SAMPLER_FORMAT; }
            channels = sampler_u16(header + 2); rate = sampler_u32(header + 4);
            bytes_per_second = sampler_u32(header + 8); align = sampler_u16(header + 12);
            bits = sampler_u16(header + 14); have_format = true;
        } else if (!memcmp(header, "data", 4)) {
            if (have_data) { return APP_SAMPLER_FORMAT; }
            data_offset = cursor + 8U; data_bytes = count; have_data = true;
        }
        cursor = (size_t)next;
    }
    if (!have_format || !have_data || (channels != 1U && channels != 2U) ||
        (bits != 8U && bits != 16U) || rate < 8000U || rate > 192000U ||
        align != channels * (bits / 8U) || bytes_per_second != rate * align ||
        !data_bytes || data_bytes % align) { return APP_SAMPLER_FORMAT; }
    const uint32_t frames = (uint32_t)(data_bytes / align);
    if (frames > APP_SAMPLER_MAX_FRAMES || frames > rate * APP_SAMPLER_MAX_SECONDS) {
        return APP_SAMPLER_RANGE;
    }
    sampler_sample_t *sample = calloc(1, sizeof(*sample));
    uint8_t *chunk = malloc(WAV_CONVERT_FRAMES * 4U);
    if (!sample || !chunk) { free(sample); free(chunk); return APP_SAMPLER_NO_MEMORY; }
    sample->pcm = sampler_pcm_create(frames);
    if (!sample->pcm) { free(sample); free(chunk); return APP_SAMPLER_NO_MEMORY; }
    app_sampler_result_t result = APP_SAMPLER_OK;
    for (uint32_t offset = 0; offset < frames;) {
        const uint32_t count = frames - offset < WAV_CONVERT_FRAMES ? frames - offset : WAV_CONVERT_FRAMES;
        if (!read_at(reader, data_offset + (size_t)offset * align, chunk, (size_t)count * align)) {
            result = APP_SAMPLER_IO; break;
        }
        for (uint32_t frame = 0; frame < count; ++frame) {
            int32_t mono = 0;
            for (unsigned channel = 0; channel < channels; ++channel) {
                const uint8_t *p = chunk + frame * align + channel * (bits / 8U);
                mono += bits == 8U ? ((int32_t)*p - 128) * 256 : (int16_t)sampler_u16(p);
            }
            sample->pcm->data[offset + frame] = (int16_t)(mono / (int32_t)channels);
        }
        offset += count;
    }
    if (result == APP_SAMPLER_OK) {
        app_sampler_sample_info_t *i = &sample->info;
        sample_name(i->name, name); i->source = APP_SAMPLER_SOURCE_WAV;
        i->frames = frames; i->sample_rate = rate; i->c2_speed = rate;
        i->trim_end = frames; i->loop_end = frames; i->gain = 1.0f; i->default_volume = 1.0f;
        result = sampler_append(sampler, sample, index);
    }
    sampler_pcm_release(sample->pcm); free(sample); free(chunk);
    return result;
}
app_sampler_result_t app_sampler_import_wav(app_sampler_t *sampler,
    const uint8_t *bytes, size_t size, const char *name, unsigned *index)
{
    if (!sampler || !bytes) { return APP_SAMPLER_INVALID; }
    if (size > WAV_MAX_FILE_BYTES) { return APP_SAMPLER_RANGE; }
    wav_reader_t reader = {bytes, NULL, size};
    return import_wav(sampler, &reader, name, index);
}
app_sampler_result_t app_sampler_import_wav_file(app_sampler_t *sampler,
    const char *path, unsigned *index)
{
    if (!sampler || !path || !*path) { return APP_SAMPLER_INVALID; }
    FILE *file = fopen(path, "rb");
    if (!file) { return APP_SAMPLER_IO; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return APP_SAMPLER_IO; }
    const long length = ftell(file);
    if (length < 0 || (uint64_t)length > WAV_MAX_FILE_BYTES) { fclose(file); return APP_SAMPLER_RANGE; }
    wav_reader_t reader = {NULL, file, (size_t)length};
    const app_sampler_result_t result = import_wav(sampler, &reader, path, index);
    if (fclose(file) != 0 && result == APP_SAMPLER_OK) {
        /* The immutable import has already committed; fclose on a read handle
         * cannot invalidate it, so retain the successful result. */
    }
    return result;
}

app_sampler_bank_t *app_sampler_bank_from_s3m(const uint8_t *data, size_t size,
                                              app_sampler_result_t *result)
{
    app_sampler_result_t error = APP_SAMPLER_FORMAT;
    app_sampler_bank_t *bank = NULL;
    if (!data || size < 0x60U || memcmp(data + 0x2c, "SCRM", 4)) { goto done; }
    const unsigned orders = sampler_u16(data + 0x20), count = sampler_u16(data + 0x22);
    const size_t pointers = 0x60U + orders;
    if (count > APP_SAMPLER_MAX_SAMPLES) { error = APP_SAMPLER_FULL; goto done; }
    if (pointers > size || count * 2U > size - pointers) { goto done; }
    bank = sampler_bank_clone(NULL);
    if (!bank) { error = APP_SAMPLER_NO_MEMORY; goto done; }
    const bool unsigned_pcm = sampler_u16(data + 0x2a) == 2U;
    for (unsigned index = 0; index < count; ++index) {
        const size_t offset = (size_t)sampler_u16(data + pointers + index * 2U) * 16U;
        if (offset > size || 80U > size - offset) { goto fail; }
        const uint8_t *h = data + offset;
        sampler_sample_t *sample = &bank->samples[bank->count++];
        app_sampler_sample_info_t *i = &sample->info;
        i->source = APP_SAMPLER_SOURCE_S3M; i->original_slot = (uint16_t)(index + 1U);
        i->sample_rate = i->c2_speed = sampler_u32(h + 32);
        if (!i->c2_speed) { i->sample_rate = i->c2_speed = 8363U; }
        i->gain = 1.0f; i->default_volume = (float)h[28] / 64.0f;
        for (unsigned c = 0; c < 28U; ++c) { i->name[c] = h[48 + c] < 128U ? (char)h[48 + c] : '?'; }
        i->name[28] = '\0';
        if (h[0] == 0) {
            if (!sampler_info_valid(i)) { goto fail; }
            continue;
        }
        if (h[0] != 1U || h[30] != 0 || memcmp(h + 76, "SCRS", 4) || (h[31] & ~7U)) { goto fail; }
        const uint32_t frames = sampler_u32(h + 16);
        const unsigned channels = (h[31] & 2U) ? 2U : 1U, width = (h[31] & 4U) ? 2U : 1U;
        const size_t pcm_offset = (size_t)(((uint32_t)h[13] << 16) | sampler_u16(h + 14)) * 16U;
        const uint64_t bytes = (uint64_t)frames * channels * width;
        if (frames > APP_SAMPLER_MAX_FRAMES || pcm_offset > size || bytes > size - pcm_offset ||
            (size_t)frames * 2U > APP_SAMPLER_BANK_PCM_BYTES - bank->pcm_bytes) { goto fail; }
        i->frames = frames; i->trim_end = frames;
        i->assigned_slot = frames ? i->original_slot : 0;
        i->loop_enabled = (h[31] & 1U) != 0;
        i->loop_start = i->loop_enabled ? sampler_u32(h + 20) : 0;
        i->loop_end = i->loop_enabled ? sampler_u32(h + 24) : frames;
        if (!sampler_info_valid(i)) { goto fail; }
        if (!frames) { continue; }
        sample->pcm = sampler_pcm_create(frames);
        if (!sample->pcm) { error = APP_SAMPLER_NO_MEMORY; goto fail; }
        for (uint32_t frame = 0; frame < frames; ++frame) {
            int32_t mono = 0;
            for (unsigned channel = 0; channel < channels; ++channel) {
                const uint8_t *p = data + pcm_offset + ((size_t)channel * frames + frame) * width;
                if (width == 1U) { mono += (unsigned_pcm ? (int32_t)*p - 128 : (int8_t)*p) * 256; }
                else { mono += unsigned_pcm ? (int32_t)sampler_u16(p) - 32768 : (int16_t)sampler_u16(p); }
            }
            sample->pcm->data[frame] = (int16_t)(mono / (int32_t)channels);
        }
        bank->pcm_bytes += (size_t)frames * 2U;
    }
    error = APP_SAMPLER_OK; goto done;
fail:
    app_sampler_bank_destroy(bank); bank = NULL;
done:
    if (result) { *result = error; }
    return bank;
}
