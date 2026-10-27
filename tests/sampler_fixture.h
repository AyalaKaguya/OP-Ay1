// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static void fixture_u16(uint8_t *p, uint16_t value)
{ p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void fixture_u32(uint8_t *p, uint32_t value)
{ p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24); }

/* One channel, two independent signed PCM8 loops, one 64-row pattern.
 * The first event is C4, instrument1, explicit volume32 and D00. */
static void fixture_song(uint8_t bytes[1024])
{
    memset(bytes, 0, 1024);
    memcpy(bytes, "Sampler fixture", 15); bytes[28] = 0x1a; bytes[29] = 0x10;
    fixture_u16(bytes + 32, 2); fixture_u16(bytes + 34, 2); fixture_u16(bytes + 36, 1);
    fixture_u16(bytes + 42, 1); memcpy(bytes + 44, "SCRM", 4);
    bytes[48] = 64; bytes[49] = 6; bytes[50] = 125;
    memset(bytes + 64, 0xff, 32); bytes[64] = 0;
    bytes[96] = 0; bytes[97] = 0xff;
    fixture_u16(bytes + 98, 0x10); fixture_u16(bytes + 100, 0x15); fixture_u16(bytes + 102, 0x20);
    for (unsigned slot = 0; slot < 2; ++slot) {
        uint8_t *h = bytes + 0x100 + slot * 80;
        h[0] = 1; fixture_u16(h + 14, (uint16_t)(0x30 + slot * 2));
        fixture_u32(h + 16, 8); fixture_u32(h + 24, 8);
        h[28] = slot ? 32 : 48; h[31] = 1;
        fixture_u32(h + 32, 48000); memcpy(h + 76, "SCRS", 4);
    }
    fixture_u16(bytes + 0x200, 72);
    bytes[0x202] = 0xe0; bytes[0x203] = 0x40; bytes[0x204] = 1;
    bytes[0x205] = 32; bytes[0x206] = 4;
    memset(bytes + 0x300, 64, 8); memset(bytes + 0x320, 192, 8);
}

static size_t fixture_wav(uint8_t *out, unsigned bits, unsigned channels,
                           uint32_t rate, const int16_t *interleaved, unsigned frames)
{
    const unsigned width = bits / 8U, align = channels * width;
    const unsigned data_bytes = frames * align, padding = data_bytes & 1U;
    memset(out, 0, 44U + data_bytes + padding);
    memcpy(out, "RIFF", 4); fixture_u32(out + 4, 36U + data_bytes + padding);
    memcpy(out + 8, "WAVEfmt ", 8); fixture_u32(out + 16, 16);
    fixture_u16(out + 20, 1); fixture_u16(out + 22, (uint16_t)channels);
    fixture_u32(out + 24, rate); fixture_u32(out + 28, rate * align);
    fixture_u16(out + 32, (uint16_t)align); fixture_u16(out + 34, (uint16_t)bits);
    memcpy(out + 36, "data", 4); fixture_u32(out + 40, data_bytes);
    for (unsigned i = 0; i < frames * channels; ++i) {
        if (bits == 8U) { out[44U + i] = (uint8_t)(interleaved[i] / 256 + 128); }
        else { fixture_u16(out + 44U + i * 2U, (uint16_t)interleaved[i]); }
    }
    return 44U + data_bytes + padding;
}
