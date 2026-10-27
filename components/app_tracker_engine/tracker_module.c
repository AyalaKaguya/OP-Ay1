// SPDX-License-Identifier: Apache-2.0
/* S3M decoder and immutable song model. This file owns format validation only;
 * transport, voices, and audio output live elsewhere. */

#include "tracker_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static void *module_alloc(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : malloc(bytes);
}
#else
static void *module_alloc(size_t bytes)
{
    return malloc(bytes);
}
#endif

#define S3M_SIGNATURE_OFFSET 0x2C
#define S3M_HEADER_SIZE 0x60
#define S3M_PATTERN_DATA_MIN 2

static bool range_contains(size_t size, size_t offset, size_t bytes)
{
    return offset <= size && bytes <= size - offset;
}

static bool decode_pattern(const tracker_module_t *module, int pattern,
                           const uint8_t *data, size_t size, size_t offset)
{
    tracker_cell_data_t *rows =
        module->cells + (size_t)pattern * TRACKER_PATTERN_ROWS *
        TRACKER_MAX_CHANNELS;
    memset(rows, TRACKER_EMPTY,
           (size_t)TRACKER_PATTERN_ROWS * TRACKER_MAX_CHANNELS *
           sizeof(*rows));
    if (offset == 0) {
        return true;    /* A null paragraph denotes an empty pattern. */
    }
    if (!range_contains(size, offset, S3M_PATTERN_DATA_MIN)) {
        return false;
    }
    const size_t packed_size = tracker_read_u16(data, offset);
    if (packed_size < S3M_PATTERN_DATA_MIN || !range_contains(size, offset, packed_size)) {
        return false;
    }
    const size_t end = offset + packed_size;
    size_t cursor = offset + S3M_PATTERN_DATA_MIN;
    int row_index = 0;
    while (row_index < TRACKER_PATTERN_ROWS && cursor < end) {
        const uint8_t packed = data[cursor++];
        if (packed == 0) {
            ++row_index;
            continue;
        }

        const int channel = packed & 0x1F;
        uint8_t note = TRACKER_EMPTY;
        uint8_t instrument = TRACKER_EMPTY;
        uint8_t volume = TRACKER_EMPTY;
        uint8_t effect = TRACKER_EMPTY;
        uint8_t param = 0;

        if ((packed & 0x20) != 0) {
            if (!range_contains(end, cursor, 2)) {
                return false;
            }
            note = data[cursor];
            instrument = data[cursor + 1];
            cursor += 2;
        }
        if ((packed & 0x40) != 0) {
            if (!range_contains(end, cursor, 1)) {
                return false;
            }
            volume = data[cursor++];
        }
        if ((packed & 0x80) != 0) {
            if (!range_contains(end, cursor, 2)) {
                return false;
            }
            effect = data[cursor];
            param = data[cursor + 1];
            cursor += 2;
        }

        const uint8_t slot = module->source_slot[channel];
        if (slot >= TRACKER_MAX_CHANNELS) {
            continue;
        }
        tracker_cell_data_t *cell =
            rows + (size_t)row_index * TRACKER_MAX_CHANNELS + slot;
        cell->note = note;
        cell->instrument = instrument;
        cell->volume = volume;
        cell->effect = effect;
        cell->param = param;
    }
    return row_index == TRACKER_PATTERN_ROWS;
}

static void decode_samples(tracker_module_t *module, size_t pointer_offset)
{
    const uint8_t * const data = module->blob;
    const size_t size = module->blob_size;
    for (int index = 0; index < module->sample_count; ++index) {
        const size_t header_offset =
            (size_t)tracker_read_u16(data, pointer_offset + (size_t)index * 2) *
            16;
        if (!range_contains(size, header_offset, 0x40) ||
                data[header_offset] != 1) {
            continue;
        }

        const uint8_t flags = data[header_offset + 31];
        if (data[header_offset + 30] != 0 || (flags & ~0x07U) != 0) {
            continue;
        }

        const uint32_t paragraph =
            ((uint32_t)data[header_offset + 13] << 16) |
            tracker_read_u16(data, header_offset + 14);
        const uint64_t sample_offset = (uint64_t)paragraph * 16U;
        const uint32_t length = tracker_read_u32(data, header_offset + 16);
        const uint8_t width = (flags & 4U) ? 2U : 1U;
        const uint8_t channels = (flags & 2U) ? 2U : 1U;
        const uint64_t sample_bytes = (uint64_t)length * width * channels;
        if (sample_offset + sample_bytes > size || length == 0) {
            continue;
        }

        tracker_sample_t *sample = &module->samples[index];
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        if ((flags & 1) != 0) {
            loop_start = tracker_read_u32(data, header_offset + 20);
            loop_end = tracker_read_u32(data, header_offset + 24);
        }
        const uint32_t c2_speed = tracker_read_u32(data, header_offset + 32);
        if (c2_speed == 0 || loop_start >= length || loop_end > length) {
            continue;
        }

        sample->data = data + sample_offset;
        sample->length = length;
        sample->loop_start = loop_start;
        sample->loop_end = loop_end;
        sample->loops = loop_end > loop_start;
        sample->volume = (float)data[header_offset + 28] / 64.0f;
        sample->gain = 1.0f;
        sample->c2_speed = c2_speed;
        sample->raw_bits = width * 8U;
        sample->raw_channels = channels;
    }
}

static void decode_channels(tracker_module_t *module, size_t pattern_end)
{
    const uint8_t * const data = module->blob;
    const size_t size = module->blob_size;
    const uint8_t *default_panning = NULL;
    if (data[0x35] == 252 && range_contains(size, pattern_end, 32)) {
        default_panning = data + pattern_end;
    }

    int channel_count = 0;
    memset(module->source_slot, TRACKER_EMPTY, sizeof(module->source_slot));
    for (int source = 0; source < TRACKER_SOURCE_CHANNELS; ++source) {
        if (data[0x40 + source] >= 16 || channel_count >= TRACKER_MAX_CHANNELS) {
            continue;
        }
        float pan = source < 8 ? -0.6f : 0.6f;
        if (default_panning != NULL && (default_panning[source] & 0x20) != 0) {
            pan = (float)(default_panning[source] & 0x0F) / 15.0f * 2.0f - 1.0f;
        }
        module->channel_source[channel_count] = (uint8_t)source;
        module->source_slot[source] = (uint8_t)channel_count;
        module->channel_pan[channel_count] = pan;
        ++channel_count;
    }

    if (channel_count == 0) {
        module->channel_source[0] = 0;
        module->source_slot[0] = 0;
        module->channel_pan[0] = 0.0f;
        channel_count = 1;
    }
    module->channel_count = channel_count;
}

void tracker_module_destroy(tracker_module_t *module)
{
    if (module != NULL) {
        for (int i = 0; i < TRACKER_MAX_INSTRUMENTS; ++i) {
            free(module->samples[i].pcm16);
            module->samples[i].pcm16 = NULL;
        }
        free(module->owned_blob);
        module->owned_blob = NULL;
        free(module->cells);
        module->cells = NULL;
    }
}

bool tracker_module_parse(tracker_module_t *module, const uint8_t *s3m,
                          size_t size)
{
    if (module == NULL || s3m == NULL || size < S3M_HEADER_SIZE ||
            memcmp(s3m + S3M_SIGNATURE_OFFSET, "SCRM", 4) != 0) {
        return false;
    }

    memset(module, 0, sizeof(*module));
    module->blob = s3m;
    module->blob_size = size;
    module->unsigned_samples = tracker_read_u16(s3m, 0x2A) == 2;
    module->global_volume = (float)s3m[0x30] / 64.0f;
    module->initial_speed = s3m[0x31] != 0 ? s3m[0x31] : TRACKER_DEFAULT_SPEED;
    module->initial_tempo =
        s3m[0x32] >= 32 ? s3m[0x32] : TRACKER_DEFAULT_TEMPO;

    const int order_count = tracker_read_u16(s3m, 0x20);
    const int sample_count = tracker_read_u16(s3m, 0x22);
    const int pattern_count = tracker_read_u16(s3m, 0x24);
    if (order_count <= 0 || pattern_count <= 0 ||
            order_count > TRACKER_MAX_ORDERS || sample_count > TRACKER_MAX_INSTRUMENTS ||
            pattern_count > TRACKER_MAX_PATTERNS) {
        return false;
    }

    const size_t pointer_offset = S3M_HEADER_SIZE + (size_t)order_count;
    const size_t instrument_bytes = (size_t)sample_count * 2;
    const size_t pattern_bytes = (size_t)pattern_count * 2;
    if (!range_contains(size, pointer_offset, instrument_bytes + pattern_bytes)) {
        return false;
    }

    module->order_count = order_count;
    module->sample_count = sample_count;
    module->pattern_count = pattern_count;
    memcpy(module->orders, s3m + S3M_HEADER_SIZE, (size_t)order_count);

    for (int index = 0; index < 28; ++index) {
        module->title[index] =
            (s3m[index] >= 32 && s3m[index] < 127) ? (char)s3m[index] : ' ';
    }
    module->title[28] = '\0';

    const size_t pattern_offset = pointer_offset + instrument_bytes;
    decode_samples(module, pointer_offset);
    memcpy(module->original_samples, module->samples, sizeof(module->samples));
    decode_channels(module, pattern_offset + pattern_bytes);

    const size_t cell_count = (size_t)pattern_count * TRACKER_PATTERN_ROWS *
                              TRACKER_MAX_CHANNELS;
    module->cells = module_alloc(cell_count * sizeof(*module->cells));
    if (module->cells == NULL) {
        return false;
    }
    for (int pattern = 0; pattern < pattern_count; ++pattern) {
        const size_t offset =
            (size_t)tracker_read_u16(s3m, pattern_offset + pattern * 2) * 16;
        if (!decode_pattern(module, pattern, s3m, size, offset)) {
            return false;
        }
    }
    return true;
}
