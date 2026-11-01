/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_effects_adapter.h"
#include "dsound_effects_image.h"
#include "dsound_device_model.h"
#include "dsound_api_adapter.h"
#include "dsound_buffer_adapter.h"
#include "audio_output.h"
#include "kernel_abi.h"
#include "stop_report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NO_EFFECT = 0xffffffffu, INVALID_EFFECT = 0x88780032u };
static RecompDsoundEffectsImage effects;
static uint32_t payload, descriptor, scratch;
static uint32_t locations[2] = {NO_EFFECT, NO_EFFECT};
static RecompAudioReverb pending_listener;
static bool listener_dirty;
static uint8_t *dirty;

typedef struct DsoundEffectsState {
    RecompDsoundEffectsImage effects;
    uint32_t payload, descriptor, scratch;
    uint32_t locations[2];
    RecompAudioReverb pending_listener;
    bool listener_dirty;
    uint8_t *dirty;
} DsoundEffectsState;

size_t recomp_dsound_effects_state_size(void) { return sizeof(DsoundEffectsState); }
void recomp_dsound_effects_state_save(void *state)
{
    DsoundEffectsState *out = state;
    out->effects = effects;
    out->payload = payload;
    out->descriptor = descriptor;
    out->scratch = scratch;
    memcpy(out->locations, locations, sizeof out->locations);
    out->pending_listener = pending_listener;
    out->listener_dirty = listener_dirty;
    out->dirty = dirty;
}
void recomp_dsound_effects_state_restore(const void *state)
{
    const DsoundEffectsState *in = state;
    effects = in->effects;
    payload = in->payload;
    descriptor = in->descriptor;
    scratch = in->scratch;
    memcpy(locations, in->locations, sizeof locations);
    pending_listener = in->pending_listener;
    listener_dirty = in->listener_dirty;
    dirty = in->dirty;
}

static uint8_t *span(uint32_t address, uint32_t bytes)
{
    uint32_t window = address & ~(RECOMP_XBOX_RAM_SIZE - 1u);
    uint32_t offset = address & (RECOMP_XBOX_RAM_SIZE - 1u);
    if (!address || (window != 0u && window != RECOMP_XBOX_CACHED_ALIAS &&
        window != RECOMP_XBOX_PHYSICAL_ALIAS) || bytes > RECOMP_XBOX_RAM_SIZE - offset) return NULL;
    return recomp_memory(address, bytes);
}

static uint32_t settings_address(void)
{
    return *recomp_memory_u32(recomp_dsound_current_device());
}

void recomp_dsound_listener_vectors(float vectors[6])
{
    memcpy(vectors, recomp_memory(settings_address() + 0x38u, 6u * sizeof *vectors), 6u * sizeof *vectors);
}

static void free_guest(uint32_t base, uint32_t bytes)
{
    if (!base) return;
    recomp_guest_memset(base, 0, bytes);
    recomp_kernel_free_pool(base);
}

void recomp_dsound_effects_reset(void)
{
    free_guest(payload, effects.payload_bytes);
    free_guest(descriptor, 8u + effects.count * 32u);
    free_guest(scratch, effects.scratch_bytes);
    payload = descriptor = scratch = 0u;
    locations[0] = locations[1] = NO_EFFECT;
    listener_dirty = false;
    free(dirty);
    dirty = NULL;
    recomp_dsound_effects_destroy(&effects);
}

uint32_t recomp_dsound_effects_headroom(uint32_t bin)
{
    const uint32_t handle = recomp_dsound_current_device();
    if (!handle || bin >= 32u) return 0u;
    return *recomp_memory_i8(*recomp_memory_u32(handle) + 0x10u + bin) & 7u;
}

static void download(void)
{
    recomp_dsound_require_device(kernel_arg(1u));
    uint32_t bytes = kernel_arg(3u), loc = kernel_arg(4u), output = kernel_arg(5u);
    uint8_t *input = span(kernel_arg(2u), bytes), *where = loc ? span(loc, 8u) : NULL;
    if (!input || (loc && !where) || (output && !span(output, 4u))) {
        kernel_return(5u, RECOMP_DSOUND_POINTER_ERROR); return;
    }
    uint32_t selected[2] = {NO_EFFECT, NO_EFFECT};
    if (where) memcpy(selected, where, sizeof selected);
    RecompDsoundEffectsImage next = {0};
    uint32_t seed[2];
    memcpy(&seed[0], recomp_memory(0x0036c38cu, 4u), 4u);
    memcpy(&seed[1], recomp_memory(0x0036c391u, 4u), 4u);
    const uint64_t initializer = (uint64_t)seed[0] << 32u | seed[1];
    uint32_t result = recomp_dsound_effects_parse(input, bytes, initializer, &next);
    if (result != RECOMP_DSOUND_OK) { kernel_return(5u, result); return; }
    /* shortcut: unassigned FX roles need a native graph; reject them before replacing reverb. */
    if (selected[0] == NO_EFFECT) { result = RECOMP_DSOUND_FAIL; goto failed; }
    if ((selected[0] != NO_EFFECT && selected[0] >= next.count) ||
        (selected[1] != NO_EFFECT && selected[1] >= next.count) ||
        next.payload_bytes > RECOMP_XBOX_RAM_SIZE || next.scratch_bytes > RECOMP_XBOX_RAM_SIZE) {
        result = RECOMP_DSOUND_INVALID_PARAM; goto failed;
    }
    if (selected[0] != NO_EFFECT && next.maps[selected[0]].state_bytes < 0x220u) {
        result = RECOMP_DSOUND_INVALID_PARAM; goto failed;
    }
    /* Locations select SDK-defined reverb/crosstalk roles; DSP programs are
       decoded for integrity and owned maps, never executed by this host. */
    uint8_t *next_dirty = calloc(next.payload_bytes, 1u);
    uint32_t next_payload = recomp_kernel_allocate_pool(next.payload_bytes);
    uint32_t next_descriptor = recomp_kernel_allocate_pool(8u + next.count * 32u);
    uint32_t next_scratch = next.scratch_bytes ? recomp_kernel_allocate_pool(next.scratch_bytes) : 0u;
    if (!next_dirty || !next_payload || !next_descriptor || (next.scratch_bytes && !next_scratch)) {
        free(next_dirty);
        free_guest(next_payload, next.payload_bytes);
        free_guest(next_descriptor, 8u + next.count * 32u);
        free_guest(next_scratch, next.scratch_bytes);
        result = RECOMP_DSOUND_OUT_OF_MEMORY; goto failed;
    }
    RecompAudioReverb listener;
    memcpy(&listener, recomp_memory(settings_address() + 0x84u, sizeof listener), sizeof listener);
    if (selected[0] != NO_EFFECT && !recomp_audio_output_set_reverb(&listener)) {
        free(next_dirty);
        free_guest(next_payload, next.payload_bytes);
        free_guest(next_descriptor, 8u + next.count * 32u);
        free_guest(next_scratch, next.scratch_bytes);
        result = RECOMP_DSOUND_FAIL; goto failed;
    }
    memcpy(recomp_memory(next_payload, next.payload_bytes), next.payload, next.payload_bytes);
    *recomp_memory_u32(next_descriptor) = next.count;
    *recomp_memory_u32(next_descriptor + 4u) = next.scratch_bytes;
    if (next_scratch) recomp_guest_memset(next_scratch, 0, next.scratch_bytes);
    for (uint32_t i = 0u; i < next.count; ++i) {
        const RecompDsoundEffectsMap *map = &next.maps[i];
        const uint32_t words[8] = { next_payload + map->code_offset, map->code_bytes,
            next_payload + map->state_offset, map->state_bytes, 0u, 0u,
            next_scratch ? next_scratch + map->scratch_offset : 0u, map->scratch_bytes };
        memcpy(recomp_memory(next_descriptor + 8u + i * 32u, sizeof words), words, sizeof words);
    }
    recomp_dsound_effects_reset();
    effects = next;
    dirty = next_dirty;
    payload = next_payload; descriptor = next_descriptor; scratch = next_scratch;
    memcpy(locations, selected, sizeof locations);
    pending_listener = listener;
    listener_dirty = selected[0] != NO_EFFECT;
    const uint32_t config = settings_address();
    *recomp_memory_u32(config + 8u) = locations[0];
    *recomp_memory_u32(config + 12u) = locations[1];
    if (output) *recomp_memory_u32(output) = descriptor;
    fprintf(stderr, "recomp dsound: effects decoded count=%u payload=%u scratch=%u descriptor=%08x\n",
        effects.count, effects.payload_bytes, effects.scratch_bytes, descriptor);
    kernel_return(5u, RECOMP_DSOUND_OK);
    return;
failed:
    recomp_dsound_effects_destroy(&next);
    kernel_return(5u, result);
}

static void set_effect_data(void)
{
    recomp_dsound_require_device(kernel_arg(1u));
    uint32_t index = kernel_arg(2u), offset = kernel_arg(3u), bytes = kernel_arg(5u);
    uint32_t flags = kernel_arg(6u);
    if (index >= effects.count) { kernel_return(6u, INVALID_EFFECT); return; }
    const RecompDsoundEffectsMap *map = &effects.maps[index];
    uint8_t *data = span(kernel_arg(4u), bytes);
    if (!data) { kernel_return(6u, RECOMP_DSOUND_POINTER_ERROR); return; }
    if (offset > map->state_bytes || bytes > map->state_bytes - offset || flags > 1u) {
        kernel_return(6u, RECOMP_DSOUND_INVALID_PARAM); return;
    }
    memmove(effects.payload + map->state_offset + offset, data, bytes);
    if (flags == 0u) {
        memmove(recomp_memory(payload + map->state_offset + offset, bytes), data, bytes);
        memset(dirty + map->state_offset + offset, 0, bytes);
    } else memset(dirty + map->state_offset + offset, 1, bytes);
    /* The linked callers use immediate updates. Deferred backing writes are
       retained; listener commit flushes them only with a dirty listener. */
    kernel_return(6u, RECOMP_DSOUND_OK);
}

static uint32_t commit_listener(void)
{
    /* Committing a moved listener recomputes every 3D voice. */
    uint32_t *flags = recomp_memory_u32(settings_address() + 0x30u);
    const bool moved = (*flags & 3u) != 0u;
    *flags &= ~3u;
    if (moved) recomp_dsound_buffers_refresh_routing();
    if (!listener_dirty) return RECOMP_DSOUND_OK;
    if (locations[0] == NO_EFFECT) return INVALID_EFFECT;
    if (!recomp_audio_output_set_reverb(&pending_listener)) {
        return recomp_audio_output_ready() ? RECOMP_DSOUND_INVALID_PARAM : RECOMP_DSOUND_FAIL;
    }
    for (uint32_t i = effects.state_offset; i < effects.payload_bytes; ++i) {
        if (dirty[i]) {
            *recomp_memory_i8(payload + i) = (int8_t)effects.payload[i];
            dirty[i] = 0u;
        }
    }
    const uint32_t control = effects.maps[locations[0]].state_offset + 0x10u;
    *recomp_memory_u32(payload + control) |= 4u;
    *recomp_memory_u32(settings_address() + 0x80u) = 0u;
    listener_dirty = false;
    return RECOMP_DSOUND_OK;
}

static void set_listener(void)
{
    recomp_dsound_require_device(kernel_arg(1u));
    const uint32_t source = kernel_arg(2u), apply = kernel_arg(3u);
    uint8_t *data = span(source, sizeof pending_listener);
    if (locations[0] == NO_EFFECT) { kernel_return(3u, INVALID_EFFECT); return; }
    if (!data) { kernel_return(3u, RECOMP_DSOUND_POINTER_ERROR); return; }
    if (apply > 1u) { kernel_return(3u, RECOMP_DSOUND_INVALID_PARAM); return; }
    memcpy(&pending_listener, data, sizeof pending_listener);
    memcpy(recomp_memory(settings_address() + 0x84u, sizeof pending_listener), data, sizeof pending_listener);
    *recomp_memory_u32(settings_address() + 0x80u) = 0xfffu;
    listener_dirty = true;
    kernel_return(3u, apply ? RECOMP_DSOUND_OK : commit_listener());
}

static void commit(void)
{
    recomp_dsound_require_device(kernel_arg(1u));
    kernel_return(1u, commit_listener());
}

/* Device SetPosition/SetVelocity(device, x, y, z, apply), ret 0x14. */
static void set_listener_vector(uint32_t offset, uint32_t dirty)
{
    recomp_dsound_require_device(kernel_arg(1u));
    const uint32_t settings = settings_address();
    for (uint32_t i = 0u; i < 3u; ++i) {
        *recomp_memory_u32(settings + offset + i * 4u) = kernel_arg(2u + i);
    }
    *recomp_memory_u32(settings + 0x30u) |= dirty;
    kernel_return(5u, (kernel_arg(5u) & 1u) ? RECOMP_DSOUND_OK : commit_listener());
}

static void set_position(void) { set_listener_vector(0x38u, 1u); }
static void set_velocity(void) { set_listener_vector(0x44u, 2u); }

static void headroom(void)
{
    recomp_dsound_require_device(kernel_arg(1u));
    uint32_t bin = kernel_arg(2u);
    if (bin >= 32u) { kernel_return(3u, RECOMP_DSOUND_INVALID_PARAM); return; }
    *recomp_memory_i8(settings_address() + 0x10u + bin) = (int8_t)kernel_arg(3u);
    recomp_dsound_buffers_refresh_routing();
    kernel_return(3u, RECOMP_DSOUND_OK);
}

RecompFunction recomp_dsound_effects_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0036473du: return download;
    case 0x00364764u: return set_effect_data;
    case 0x00365c6eu: return commit;
    case 0x0036604au: return set_listener;
    case 0x0036478eu: return headroom;
    case 0x00365fe0u: return set_position;
    case 0x00366015u: return set_velocity;
    default: return NULL;
    }
}
