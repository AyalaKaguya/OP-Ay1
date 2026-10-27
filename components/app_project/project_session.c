// SPDX-License-Identifier: Apache-2.0
#include "project_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put_float(uint8_t *p, float value)
{
    uint32_t bits; memcpy(&bits, &value, sizeof(bits)); put32(p, bits);
}
static float get_float(const uint8_t *p)
{
    const uint32_t bits = get32(p); float value; memcpy(&value, &bits, sizeof(value)); return value;
}

void project_state_defaults(project_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->patch = app_synth_patch_for_preset(APP_SYNTH_PATCH_INIT);
    state->speed_multiplier = 1.0f;
    state->master_volume = 100;
    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) state->channel_gain[ch] = 1.0f;
    for (unsigned source = 0; source < 4; ++source) state->source_gain[source] = 1.0f;
    for (int pad = 0; pad < APP_PROJECT_PAD_COUNT; ++pad) state->pad_patterns[pad] = pad;
}

bool project_state_encode(const project_state_t *state, uint8_t bytes[PROJECT_STATE_BYTES])
{
    if (!state || !bytes || sizeof(float) != 4) return false;
    memset(bytes, 0, PROJECT_STATE_BYTES);
    memcpy(bytes, "OPSTATE1", 8);
    const app_synth_patch_t *p = &state->patch;
    put32(bytes + 8, (uint32_t)p->waveform); put32(bytes + 12, (uint32_t)p->lfo_target);
    const float values[12] = {p->wavetable_position, p->detune_cents, p->gain, p->pan,
        p->filter_cutoff_hz, p->filter_resonance, p->attack_s, p->decay_s,
        p->sustain, p->release_s, p->lfo_hz, p->lfo_depth};
    for (unsigned index = 0; index < 12; ++index) put_float(bytes + 16 + index * 4, values[index]);
    put_float(bytes + 64, state->speed_multiplier);
    put32(bytes + 68, state->loop ? 1U : 0U); put32(bytes + 72, (uint32_t)state->master_volume);
    for (unsigned ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        put_float(bytes + 80 + ch * 4, state->channel_gain[ch]);
        bytes[144 + ch] = state->mute[ch] ? 1 : 0;
        bytes[160 + ch] = state->solo[ch] ? 1 : 0;
    }
    for (unsigned source = 0; source < 4; ++source) put_float(bytes + 176 + source * 4, state->source_gain[source]);
    for (unsigned pad = 0; pad < APP_PROJECT_PAD_COUNT; ++pad) put32(bytes + 192 + pad * 4, (uint32_t)state->pad_patterns[pad]);
    return true;
}

bool project_state_decode(const uint8_t bytes[PROJECT_STATE_BYTES], project_state_t *state)
{
    if (!state || !bytes || memcmp(bytes, "OPSTATE1", 8) || sizeof(float) != 4) return false;
    project_state_defaults(state);
    app_synth_patch_t *p = &state->patch;
    p->waveform = (app_synth_waveform_t)get32(bytes + 8);
    p->lfo_target = (app_synth_lfo_target_t)get32(bytes + 12);
    float *fields[12] = {&p->wavetable_position, &p->detune_cents, &p->gain, &p->pan,
        &p->filter_cutoff_hz, &p->filter_resonance, &p->attack_s, &p->decay_s,
        &p->sustain, &p->release_s, &p->lfo_hz, &p->lfo_depth};
    for (unsigned index = 0; index < 12; ++index) {
        *fields[index] = get_float(bytes + 16 + index * 4);
        if (!isfinite(*fields[index])) return false;
    }
    if (p->waveform < APP_SYNTH_WAVE_SAW || p->waveform > APP_SYNTH_WAVE_WAVETABLE ||
        p->lfo_target < APP_SYNTH_LFO_TARGET_PITCH || p->lfo_target > APP_SYNTH_LFO_TARGET_AMP ||
        p->wavetable_position < 0 || p->wavetable_position > 1 || p->detune_cents < -100 || p->detune_cents > 100 ||
        p->gain < 0 || p->gain > 1.5f || p->pan < -1 || p->pan > 1 ||
        p->filter_cutoff_hz < 20 || p->filter_cutoff_hz > 20000 || p->filter_resonance < 0 || p->filter_resonance > 0.95f ||
        p->attack_s < 0 || p->attack_s > 5 || p->decay_s < 0 || p->decay_s > 5 ||
        p->sustain < 0 || p->sustain > 1 || p->release_s < 0 || p->release_s > 5 ||
        p->lfo_hz < 0 || p->lfo_hz > 40 || p->lfo_depth < 0 ||
        p->lfo_depth > (p->lfo_target == APP_SYNTH_LFO_TARGET_PITCH ? 12.0f : 1.0f)) return false;
    state->speed_multiplier = get_float(bytes + 64);
    state->loop = get32(bytes + 68) != 0; state->master_volume = (int)get32(bytes + 72);
    if (!isfinite(state->speed_multiplier) || state->speed_multiplier < 0.5f || state->speed_multiplier > 2.0f ||
        get32(bytes + 68) > 1 || state->master_volume < 0 || state->master_volume > 100) return false;
    for (unsigned ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        state->channel_gain[ch] = get_float(bytes + 80 + ch * 4);
        if (!isfinite(state->channel_gain[ch]) || state->channel_gain[ch] < 0 || state->channel_gain[ch] > 2 ||
            bytes[144 + ch] > 1 || bytes[160 + ch] > 1) return false;
        state->mute[ch] = bytes[144 + ch] != 0; state->solo[ch] = bytes[160 + ch] != 0;
    }
    for (unsigned source = 0; source < 4; ++source) {
        state->source_gain[source] = get_float(bytes + 176 + source * 4);
        if (!isfinite(state->source_gain[source]) || state->source_gain[source] < 0 || state->source_gain[source] > 2) return false;
    }
    for (unsigned pad = 0; pad < APP_PROJECT_PAD_COUNT; ++pad) {
        const uint32_t value = get32(bytes + 192 + pad * 4);
        if (value != UINT32_MAX && value > 255) return false;
        state->pad_patterns[pad] = value == UINT32_MAX ? -1 : (int)value;
    }
    return true;
}

app_project_error_t project_sampler_error(app_sampler_result_t result)
{
    switch (result) {
    case APP_SAMPLER_OK: return APP_PROJECT_OK;
    case APP_SAMPLER_NO_MEMORY: return APP_PROJECT_NO_MEMORY;
    case APP_SAMPLER_BUSY: return APP_PROJECT_BUSY;
    case APP_SAMPLER_UNSUPPORTED: return APP_PROJECT_UNSUPPORTED;
    case APP_SAMPLER_FULL: return APP_PROJECT_NO_SPACE;
    case APP_SAMPLER_IO: return APP_PROJECT_IO_ERROR;
    default: return APP_PROJECT_INVALID;
    }
}

app_project_error_t project_capture(app_project_t *p, project_payload_t *payload)
{
    if (!tracker_engine_copy_blob(p->config.tracker, &payload->module, &payload->module_size)) return APP_PROJECT_NO_MEMORY;
    payload->bank = app_sampler_bank_snapshot(p->config.sampler);
    if (!payload->bank) return APP_PROJECT_NO_MEMORY;
    const app_project_error_t error = project_sampler_error(app_sampler_bank_encode(payload->bank,
                                                       &payload->bank_bytes, &payload->bank_size));
    if (error != APP_PROJECT_OK) return error;
    if (!app_synth_get_patch(p->config.synth, &payload->state.patch)) return APP_PROJECT_BUSY;
    payload->state.speed_multiplier = tracker_engine_speed_multiplier(p->config.tracker);
    payload->state.loop = tracker_engine_loop_enabled(p->config.tracker);
    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        payload->state.channel_gain[ch] = tracker_engine_channel_gain(p->config.tracker, ch);
        payload->state.mute[ch] = tracker_engine_channel_mute(p->config.tracker, ch);
        payload->state.solo[ch] = tracker_engine_channel_solo(p->config.tracker, ch);
    }
    for (int source = 0; source < APP_AUDIO_SOURCE_COUNT && source < 4; ++source) {
        payload->state.source_gain[source] = app_audio_bus_source_gain(p->config.bus, (app_audio_source_t)source);
    }
    project_platform_lock(&p->platform);
    payload->state.master_volume = p->snapshot.master_volume;
    memcpy(payload->state.pad_patterns, p->snapshot.pad_patterns, sizeof(payload->state.pad_patterns));
    payload->revision = p->snapshot.revision;
    strcpy(payload->id, p->snapshot.id); strcpy(payload->name, p->snapshot.name);
    project_platform_unlock(&p->platform);
    return APP_PROJECT_OK;
}

app_project_error_t project_prepare_payload(project_payload_t *payload)
{
    payload->song = tracker_engine_prepare(payload->module, payload->module_size);
    if (!payload->song) return APP_PROJECT_CORRUPT;
    app_sampler_result_t result = APP_SAMPLER_OK;
    if (!payload->bank) {
        payload->bank = payload->bank_bytes ? app_sampler_bank_prepare(payload->bank_bytes, payload->bank_size, &result)
                                           : app_sampler_bank_from_s3m(payload->module, payload->module_size, &result);
    }
    if (!payload->bank) return project_sampler_error(result);
    free(payload->bank_bytes); payload->bank_bytes = NULL; payload->bank_size = 0;
    return project_sampler_error(app_sampler_bank_apply_slots(payload->bank, payload->song));
}

app_project_error_t project_commit_payload(app_project_t *p, project_payload_t *payload)
{
    if (!app_audio_bus_begin_edit(p->config.bus, 1000)) return APP_PROJECT_TIMEOUT;
    /* Enqueue the only fallible mutation first. A rejected patch leaves all
     * old sources, router ownership and session metadata untouched. */
    if (!app_synth_set_patch(p->config.synth, &payload->state.patch)) {
        app_audio_bus_end_edit(p->config.bus); return APP_PROJECT_BUSY;
    }
    if (p->config.router) app_note_router_panic(p->config.router);
    else app_synth_panic(p->config.synth);
    tracker_engine_commit(p->config.tracker, payload->song); payload->song = NULL;
    app_sampler_bank_commit(p->config.sampler, payload->bank); payload->bank = NULL;
    tracker_engine_set_loop(p->config.tracker, payload->state.loop);
    tracker_engine_set_speed_multiplier(p->config.tracker, payload->state.speed_multiplier);
    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ++ch) {
        tracker_engine_set_channel_gain(p->config.tracker, ch, payload->state.channel_gain[ch]);
        tracker_engine_set_channel_mute(p->config.tracker, ch, payload->state.mute[ch]);
        tracker_engine_set_channel_solo(p->config.tracker, ch, payload->state.solo[ch]);
    }
    for (int source = 0; source < APP_AUDIO_SOURCE_COUNT && source < 4; ++source) {
        (void)app_audio_bus_set_source_gain(p->config.bus, (app_audio_source_t)source, payload->state.source_gain[source]);
    }
    app_audio_bus_end_edit(p->config.bus);
    p->sampler_revision = app_sampler_revision(p->config.sampler);
    project_platform_lock(&p->platform);
    strcpy(p->snapshot.id, payload->id); strcpy(p->snapshot.name, payload->name);
    p->snapshot.opened = true;
    p->snapshot.revision = payload->revision ? payload->revision : 1;
    p->snapshot.saved_revision = payload->id[0] ? payload->revision : 0;
    p->snapshot.dirty = p->snapshot.revision != p->snapshot.saved_revision;
    p->snapshot.master_volume = payload->state.master_volume;
    memcpy(p->snapshot.pad_patterns, payload->state.pad_patterns, sizeof(p->snapshot.pad_patterns));
    p->snapshot.sample_count = app_sampler_count(p->config.sampler);
    ++p->snapshot.session_generation;
    project_platform_unlock(&p->platform);
    return APP_PROJECT_OK;
}

app_project_error_t project_blank_payload(project_payload_t *payload)
{
    /* Minimal editable S3M: one empty 64-row pattern, one order and sixteen
     * empty instrument slots reserved for explicit sampler assignment.
     * A blank project contains no ARYX pattern/sample/title bytes. */
    payload->module_size = 1504;
    payload->module = calloc(1, payload->module_size);
    if (!payload->module) return APP_PROJECT_NO_MEMORY;
    uint8_t *b = payload->module;
    memcpy(b, "UNTITLED", 8); b[28] = 0x1a; b[29] = 0x10;
    b[32] = 1; b[34] = 16; b[36] = 1; b[40] = 0x20; b[41] = 0x13;
    b[42] = 1; memcpy(b + 44, "SCRM", 4);
    b[48] = 64; b[49] = 6; b[50] = 125; b[51] = 0xb0;
    memset(b + 64, 0xff, 32); b[64] = 0;
    b[96] = 0;
    for (unsigned slot = 0; slot < 16; ++slot) {
        const unsigned offset = 144 + slot * 80;
        const unsigned paragraph = offset / 16;
        b[97 + slot * 2] = (uint8_t)paragraph;
        b[98 + slot * 2] = (uint8_t)(paragraph >> 8);
        b[offset] = 1; b[offset + 28] = 64;
        b[offset + 32] = 0xab; b[offset + 33] = 0x20; /* 8363 Hz C2. */
        memcpy(b + offset + 76, "SCRS", 4);
    }
    b[129] = 89; b[130] = 0; /* Pattern at aligned offset 1424. */
    b[1424] = 66; /* Packed size includes this 2-byte header and 64 terminators. */
    project_state_defaults(&payload->state);
    strcpy(payload->name, "Untitled");
    payload->revision = 1;
    return APP_PROJECT_OK;
}

void project_payload_destroy(project_payload_t *payload)
{
    if (!payload) return;
    tracker_engine_prepared_destroy(payload->song);
    app_sampler_bank_destroy(payload->bank);
    free(payload->module); free(payload->bank_bytes);
    memset(payload, 0, sizeof(*payload));
}
