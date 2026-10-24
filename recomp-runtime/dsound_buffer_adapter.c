/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_buffer_adapter.h"
#include "dsound_api_adapter.h"
#include "dsound_buffer_model.h"
#include "dsound_mix_model.h"
#include "dsound_effects_adapter.h"
#include "dsound_hrtf.h"
#include "audio_output.h"
#include "xbox_adpcm.h"
#include "xapi_time_adapter.h"
#include "kernel_abi.h"
#include "stop_report.h"

#include <limits.h>
#include <math.h>
#include <string.h>

enum { BUFFER_COUNT = 256, ROOT_SIZE = 0x24, SETTINGS_SIZE = 0xe0, PUBLIC_OFFSET = 0x1c };
/* DSOUND 5849 full-HRTF tables: response bank, [61][31] map, 45-step center gains. */
enum { HRTF_BANK = 0x36de70u, HRTF_BANK_SIZE = 0x115c0u, HRTF_MAP = 0x37f430u, HRTF_MAP_SIZE = 0xec6u,
    CENTER_FRONT = 0x36dd00u, CENTER = 0x36ddb8u, CENTER_STEPS = 45 };
typedef struct Buffer {
    uint32_t root, settings, references, flags;
    uint32_t data, size, owned, owned_size, format, channels, bits, align, rate, frequency;
    uint32_t headroom;
    int32_t volume;
    const uint8_t *pcm;
    RecompDsoundBufferModel model;
    RecompDsoundMixModel mix;
    /* CTRL3D: the 5849 3D block fields DOA2 sets and their voice terms. */
    bool is3d;
    float position[3], velocity[3], min_distance, max_distance, distance_factor, rolloff;
    int32_t distance_volume, center_front, center, pan_front, pan_rear; /* hundredths of dB */
    int32_t doppler;                               /* 4096 per octave */
    RecompHrtfVoice hrtf;
} Buffer;
static Buffer buffers[BUFFER_COUNT];
static void update_3d(Buffer *buffer);

static uint32_t canonical(uint32_t address)
{
    uint32_t window = address & ~(RECOMP_XBOX_RAM_SIZE - 1u);
    return window == 0u || window == RECOMP_XBOX_CACHED_ALIAS ||
        window == RECOMP_XBOX_PHYSICAL_ALIAS ? address & (RECOMP_XBOX_RAM_SIZE - 1u) : UINT32_MAX;
}

static uint8_t *span(uint32_t address, uint32_t size)
{
    uint32_t offset = canonical(address);
    return address && offset < RECOMP_XBOX_RAM_SIZE && size <= RECOMP_XBOX_RAM_SIZE - offset
        ? recomp_memory(address, size) : NULL;
}

static uint32_t read32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, recomp_memory(address, sizeof value), sizeof value);
    return value;
}

static void write32(uint32_t address, uint32_t value)
{
    memcpy(recomp_memory(address, sizeof value), &value, sizeof value);
}

static uint64_t now_ms(void)
{
#ifdef RECOMP_DSOUND_TEST_CLOCK
    uint64_t recomp_test_dsound_now_ms(void);
    return recomp_test_dsound_now_ms();
#else
    return recomp_xapi_performance_counter() / 1000000u;
#endif
}

static Buffer *buffer_at(uint32_t handle)
{
    uint32_t address = canonical(handle);
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
        Buffer *buffer = &buffers[i];
        if (buffer->root && address == buffer->root + PUBLIC_OFFSET &&
            read32(buffer->root + 4u) == buffer->references &&
            read32(address) == buffer->settings) return buffer;
    }
    recomp_stop(2, "dsound:buffer-handle-invalid:%08x", handle);
    return NULL;
}

static uint32_t decoded_bytes(const Buffer *buffer, uint32_t bytes)
{
    return buffer->format == 0x69u
        ? bytes / buffer->align * XBOX_ADPCM_BLOCK_SAMPLES * buffer->channels * 2u : bytes;
}

static uint32_t source_cursor(const Buffer *buffer)
{
    uint32_t bytes = buffer->model.cursor_bytes;
    if (buffer->format == 0x69u) bytes = bytes / (64u * buffer->channels * 2u) * buffer->align;
    return bytes / buffer->align * buffer->align;
}

/* CVTTSS2SI: NaN and out-of-range values give the integer indefinite. */
static int32_t to_int(float value)
{
    return value > -2147483648.0f && value < 2147483648.0f ? (int32_t)value : INT32_MIN;
}

static int32_t clamp_db(int64_t value) { return value < -10000 ? -10000 : value > 0 ? 0 : (int32_t)value; }

/* 0x36344B: amplitude ratio to hundredths of dB. */
static int32_t amplitude_db(float ratio)
{
    return !(ratio > 0.0f) ? -10000 : ratio < 1.0f ? to_int(2000.0f * log10f(ratio)) : 0;
}

/* 0x369565 per-bin 3D terms. Cone and I3DL2 source terms stay zero at their
   defaults. */
static int32_t bin_3d(const Buffer *buffer, uint32_t bin)
{
    const int32_t base = clamp_db(buffer->distance_volume);
    if (bin == 2u) return clamp_db((int64_t)buffer->center + buffer->pan_front + base);
    if (bin == 6u || bin == 7u) return clamp_db((int64_t)buffer->center_front + buffer->pan_front + base);
    return bin == 8u || bin == 9u ? clamp_db((int64_t)buffer->pan_rear + base) : base;
}

/* 0x366A14 front/back split. 0x364B53 enables it only for surround or AC3
   speakers; stereo keeps the whole signal in front. */
static void pan_3d(Buffer *buffer, float azimuth, float elevation, float distance, bool surround)
{
    buffer->pan_front = 0;
    buffer->pan_rear = -10000;
    if (!surround) return;
    float p = ((fabsf(azimuth) * 0.011111111f - 1.0f) * (1.0f - fabsf(elevation) * 0.011111111f) + 1.0f) * 0.5f;
    p = p + p - 0.5f;
    p = !(p <= 1.0f) ? 1.0f : p < 0.0f ? 0.0f : p;
    /* Within half a meter the split eases toward an even front/back share. */
    const float meters = buffer->distance_factor * distance;
    if (meters < 0.5f) p = ((p - 0.5f) * meters + 1.0f) * 0.5f;
    buffer->pan_front = amplitude_db(1.0f - p);
    buffer->pan_rear = amplitude_db(p);
}

/* 0x3697B9: base pitch plus the 3D Doppler term, clamped to [-32767, 8191]
   at 4096 per octave from 48 kHz. */
static uint32_t pitch_hz(const Buffer *buffer)
{
    if (!buffer->doppler) return buffer->frequency;
    long pitch = lrint(4096.0 * log2(buffer->frequency / 48000.0)) + buffer->doppler;
    pitch = pitch < -32767 ? -32767 : pitch > 8191 ? 8191 : pitch;
    return (uint32_t)lround(48000.0 * exp2(pitch / 4096.0));
}

static void apply_routing(Buffer *buffer)
{
    uint32_t headroom[32], channels = buffer->is3d ? 3u : buffer->channels;
    float dry[18], wet[6];
    RecompDsoundMixModel mix = buffer->mix;
    for (uint32_t i = 0u; i < 32u; ++i) headroom[i] = recomp_dsound_effects_headroom(i);
    /* 3D voices reach their bins as the two HRTF ears and the unfiltered voice. */
    for (uint32_t i = 0u; buffer->is3d && i < mix.count; ++i) {
        mix.volumes[mix.bins[i]] = clamp_db((int64_t)mix.volumes[mix.bins[i]] + bin_3d(buffer, mix.bins[i]));
    }
    if (!recomp_dsound_mix_matrices(&mix, channels, recomp_audio_output_speakers(), headroom, dry, wet)) {
        recomp_stop(2, "dsound:signal-route-unbound");
    }
    if (!recomp_audio_output_set_voice_routing((uint32_t)(buffer - buffers), channels, dry, wet)) {
        recomp_stop(2, "dsound:routing-failed");
    }
}

void recomp_dsound_buffers_refresh_routing(void)
{
    for (uint32_t i = 0u; i < BUFFER_COUNT; ++i) if (buffers[i].root) {
        update_3d(&buffers[i]);
        apply_routing(&buffers[i]);
    }
}

static uint32_t read_mix(RecompDsoundMixModel *mix, uint32_t address, bool replace, bool is3d)
{
    if (!address) {
        if (!replace) return RECOMP_DSOUND_POINTER_ERROR;
        /* DirectSoundDefaultMixBins_3D (0x380424) or the stereo default. */
        if (is3d) *mix = (RecompDsoundMixModel){.count = 5u, .bins = {6u, 8u, 7u, 9u, 10u}};
        else recomp_dsound_mix_default(mix);
        return RECOMP_DSOUND_OK;
    }
    uint8_t *header = span(address, 8u);
    uint32_t words[2];
    if (!header) return RECOMP_DSOUND_POINTER_ERROR;
    memcpy(words, header, sizeof words);
    if (words[0] > 8u) return RECOMP_DSOUND_INVALID_PARAM;
    RecompDsoundMixPair pairs[8];
    uint8_t *data = words[0] ? span(words[1], words[0] * sizeof *pairs) : NULL;
    if (words[0] && !data) return RECOMP_DSOUND_POINTER_ERROR;
    if (words[0]) memcpy(pairs, data, words[0] * sizeof *pairs);
    RecompDsoundMixModel next = *mix;
    if (!recomp_dsound_mix_set(&next, pairs, words[0], replace)) return RECOMP_DSOUND_INVALID_PARAM;
    uint32_t headroom[32] = {0};
    float dry[18], wet[6];
    if (!recomp_dsound_mix_matrices(&next, 2u, 2u, headroom, dry, wet)) {
        recomp_stop(2, "dsound:signal-route-unbound");
    }
    *mix = next;
    return RECOMP_DSOUND_OK;
}

static void write_mix(Buffer *buffer)
{
    write32(buffer->settings + 0x24u, buffer->mix.count);
    for (uint32_t i = 0u; i < 8u; ++i) {
        *recomp_memory_i8(buffer->settings + 0x28u + i) =
            (int8_t)(i < buffer->mix.count ? buffer->mix.bins[i] : 0u);
    }
    memcpy(recomp_memory(buffer->settings + 0x30u, sizeof buffer->mix.volumes),
        buffer->mix.volumes, sizeof buffer->mix.volumes);
}

static void reset_output(Buffer *buffer)
{
    recomp_audio_output_reset_voice((uint32_t)(buffer - buffers));
    const RecompHrtfFilter filter = buffer->hrtf.filter;
    buffer->hrtf = (RecompHrtfVoice){.filter = filter, .rendered = filter};
}

static void pump(Buffer *buffer, uint64_t now)
{
    if (!recomp_audio_output_ready()) recomp_stop(2, "dsound:audio-output-failed");
    if (!buffer->model.playing || !buffer->pcm) return;
    uint32_t offset = 0u;
    uint32_t bytes = recomp_dsound_buffer_consume(&buffer->model, now, &offset);
    if (!bytes) return;
    int16_t samples[40000];
    uint8_t *pcm = (uint8_t *)samples;
    if (bytes > sizeof samples) recomp_stop(2, "dsound:buffer-output-size");
    uint32_t end = buffer->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING
        ? buffer->model.loop_end_bytes : buffer->model.size_bytes;
    for (uint32_t written = 0u; written < bytes;) {
        uint32_t count = end - offset;
        const uint8_t *source;
        int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2u];
        if (buffer->format == 0x69u) {
            uint32_t block_bytes = 64u * buffer->channels * 2u;
            uint32_t within = offset % block_bytes;
            if (count > block_bytes - within) count = block_bytes - within;
            if (!xbox_adpcm_decode_block(buffer->pcm + offset / block_bytes * buffer->align,
                    buffer->align, buffer->channels, decoded, 128u)) {
                recomp_stop(2, "dsound:buffer-adpcm-invalid");
            }
            source = (const uint8_t *)decoded + within;
        } else source = buffer->pcm + offset;
        if (count > bytes - written) count = bytes - written;
        memcpy(pcm + written, source, count);
        written += count;
        offset += count;
        if (offset == end) offset = buffer->model.loop_start_bytes;
    }
    if (!recomp_audio_output_ready()) recomp_stop(2, "dsound:audio-output-failed");
    apply_routing(buffer);
    const uint32_t slot = (uint32_t)(buffer - buffers);
    if (!buffer->is3d) {
        recomp_audio_output_submit(slot, pcm, bytes, buffer->model.sample_rate, buffer->channels,
            buffer->format == 0x69u ? 16u : buffer->bits, buffer->volume);
    }
    /* The APU filters 3D voices at 48 kHz; pieces keep each submission in bounds. */
    static float ears[3u * 20000u];
    uint32_t limit = (uint32_t)((uint64_t)19000u * buffer->model.sample_rate / RECOMP_HRTF_RATE);
    if (!limit) limit = 1u;
    for (uint32_t done = 0u, frames = buffer->is3d ? bytes / 2u : 0u; done < frames;) {
        uint32_t piece = frames - done < limit ? frames - done : limit;
        uint32_t rendered = recomp_hrtf_render(&buffer->hrtf, samples + done, piece,
            buffer->model.sample_rate, ears, 20000u);
        if (rendered) recomp_audio_output_submit(slot, (const uint8_t *)ears, rendered * 12u,
            RECOMP_HRTF_RATE, 3u, 32u, buffer->volume);
        done += piece;
    }
    if (!recomp_audio_output_ready()) recomp_stop(2, "dsound:audio-output-failed");
}

/* DSOUND 5849 CDirectSound3DCalculator (0x365553, 0x364236) with the full-HRTF
   routines DirectSoundUseFullHRTF installs (0x366D1A). DOA2 never sets listener
   orientation or factors, cones, mode, rolloff curves or I3DL2 sources; they
   keep their 0x3803B4 defaults (+z ahead, +y up, factors 1). */
static void update_3d(Buffer *buffer)
{
    static const uint8_t *bank, *map;
    static const float *front_table, *center_table;
    float listener[6], d[3], n[3] = {0.0f, 0.0f, 0.0f}, u = 0.0f, azimuth, elevation;
    if (!buffer->is3d) return;
    pump(buffer, now_ms());
    recomp_dsound_listener_vectors(listener);
    for (int i = 0; i < 3; ++i) d[i] = buffer->position[i] - listener[i];
    const float distance = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; distance > 0.0f && i < 3; ++i) n[i] = d[i] / distance;

    /* 0x366873 without a curve: -2000 log10(1 + rolloff (d / min - 1)), d held at max. */
    const float held = buffer->max_distance < distance ? buffer->max_distance : distance;
    const float x = (held / buffer->min_distance - 1.0f) * buffer->rolloff;
    buffer->distance_volume = distance <= buffer->min_distance || x < 0.0f
        ? 0 : to_int(-2000.0f * log10f(x + 1.0f));

    if (!bank) {
        bank = recomp_memory(HRTF_BANK, HRTF_BANK_SIZE);
        map = recomp_memory(HRTF_MAP, HRTF_MAP_SIZE);
        front_table = (const float *)(const void *)recomp_memory(CENTER_FRONT, CENTER_STEPS * sizeof(float));
        center_table = (const float *)(const void *)recomp_memory(CENTER, CENTER_STEPS * sizeof(float));
    }
    recomp_hrtf_angles(n[0], n[1], n[2], &azimuth, &elevation);
    const bool surround = recomp_audio_output_speakers() == 6u;
    buffer->hrtf.filter = recomp_hrtf_lookup(bank, map, azimuth, elevation, surround);
    pan_3d(buffer, azimuth, elevation, distance, surround);

    /* 0x366AE3, enabled by 0x364970 when mix slot 4 is the center bin. */
    buffer->center_front = buffer->center = 0;
    if (buffer->mix.count >= 5u && buffer->mix.bins[4] == 2u) {
        const int32_t a = to_int(fabsf(azimuth)), e = to_int(fabsf(elevation));
        buffer->center = -10000;
        if (a >= 0 && a < CENTER_STEPS && e >= 0 && e < CENTER_STEPS) {
            buffer->center = amplitude_db(center_table[a] * center_table[e]);
            buffer->center_front = amplitude_db(1.0f - front_table[a] * front_table[e]);
        }
    }

    /* 0x366828 and 0x366B85: radial speed in m/s against the speed of sound. */
    for (int i = 0; i < 3; ++i) u += (buffer->velocity[i] - listener[3 + i]) * n[i];
    const float v = buffer->distance_factor * u;
    buffer->doppler = v == 0.0f ? 0 : !(v < 342.0f) ? -32767 : !(v > -342.0f) ? 4096
        : (int32_t)lrintf(4096.0f * log2f(1.0f - v * 0.0029239766f));
    if (buffer->size && buffer->model.sample_rate != pitch_hz(buffer)) {
        recomp_dsound_buffer_set_frequency(&buffer->model, pitch_hz(buffer), now_ms());
    }
}

void recomp_dsound_buffers_pump(void)
{
    /* shortcut: API/DoWork pumping drops spans after >100 ms stalls; add an
       independent producer when continuous playback across stalls is required. */
    uint64_t now = now_ms();
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) if (buffers[i].root) pump(&buffers[i], now);
}

static void dispose(Buffer *buffer)
{
    recomp_audio_output_reset_voice((uint32_t)(buffer - buffers));
    if (buffer->owned) recomp_kernel_free_pool(buffer->owned);
    if (buffer->settings) recomp_kernel_free_pool(buffer->settings);
    if (buffer->root) recomp_kernel_free_pool(buffer->root);
    *buffer = (Buffer){0};
}

void recomp_dsound_buffers_reset(void)
{
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) if (buffers[i].root) {
        dispose(&buffers[i]);
        recomp_dsound_device_release_owner();
    }
}

int recomp_dsound_buffer_addref_root(uint32_t root, uint32_t *references)
{
    uint32_t address = canonical(root);
    for (uint32_t i = 0; i < BUFFER_COUNT; ++i) if (buffers[i].root && buffers[i].root == address) {
        Buffer *buffer = buffer_at(address + PUBLIC_OFFSET);
        if (buffer->references == UINT32_MAX) recomp_stop(2, "dsound:buffer-refcount-overflow");
        *references = ++buffer->references;
        write32(address + 4u, *references);
        return 1;
    }
    return 0;
}

static uint32_t set_data(Buffer *buffer, uint32_t data, uint32_t size)
{
    if (size % buffer->align || (buffer->format == 0x69u &&
        (uint64_t)(size / buffer->align) * 64u * buffer->channels * 2u > UINT32_MAX)) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    if (data && size && !span(data, size)) return RECOMP_DSOUND_POINTER_ERROR;
    if (canonical(data) == canonical(buffer->data) && size == buffer->size) return RECOMP_DSOUND_OK;
    if (buffer->owned && data && size) {
        uint32_t start = canonical(data);
        if (start < buffer->owned + buffer->owned_size && buffer->owned < (uint64_t)start + size)
            return RECOMP_DSOUND_INVALID_PARAM;
    }
    uint32_t owned = 0u;
    if (!data && size) {
        if (size > RECOMP_XBOX_RAM_SIZE - 31u) return RECOMP_DSOUND_OUT_OF_MEMORY;
        owned = recomp_kernel_allocate_pool(size + 31u);
        if (!owned) return RECOMP_DSOUND_OUT_OF_MEMORY;
        data = (owned + 31u) & ~31u;
        uint8_t *storage = span(data, size);
        if (!storage) { recomp_kernel_free_pool(owned); return RECOMP_DSOUND_OUT_OF_MEMORY; }
        memset(storage, buffer->format == 1u && buffer->bits == 8u ? 0x80 : 0, size);
    }
    pump(buffer, now_ms());
    reset_output(buffer);
    if (buffer->owned) recomp_kernel_free_pool(buffer->owned);
    buffer->owned = owned;
    buffer->owned_size = owned ? size + 31u : 0u;
    buffer->data = size ? data : 0u;
    buffer->size = size;
    buffer->pcm = size ? span(data, size) : NULL;
    buffer->model = (RecompDsoundBufferModel){0};
    if (size) recomp_dsound_buffer_configure(&buffer->model, decoded_bytes(buffer, size),
        pitch_hz(buffer), buffer->channels * (buffer->format == 0x69u ? 2u : buffer->bits / 8u), now_ms());
    write32(buffer->settings + 8u, buffer->flags | (owned ? 0x80000000u : 0u));
    write32(buffer->settings + 0xbcu, buffer->data);
    write32(buffer->settings + 0xc0u, size);
    return RECOMP_DSOUND_OK;
}

static void create_buffer(void)
{
    recomp_dsound_require_device(kernel_arg(1u));
    uint32_t desc_address = kernel_arg(2u), output = kernel_arg(3u);
    uint32_t result = RECOMP_DSOUND_POINTER_ERROR, desc[6];
    uint8_t *desc_span = span(desc_address, sizeof desc);
    if (!span(output, 4u)) { kernel_return(4u, result); return; }
    write32(output, 0u);
    if (!desc_span) { kernel_return(4u, result); return; }
    memcpy(desc, desc_span, sizeof desc);
    if (desc[0] != sizeof desc || kernel_arg(4u)) { kernel_return(4u, RECOMP_DSOUND_INVALID_PARAM); return; }
    if (desc[5]) recomp_stop(2, "dsound:buffer-input-bin-unbound");
    if (desc[1] & ~0x400b0u) recomp_stop(2, "dsound:buffer-create-flags-unbound:%08x", desc[1]);
    uint8_t *format = span(desc[3], 18u);
    if (!format) { kernel_return(4u, result); return; }
    uint16_t tag, channels, bits, align, extra;
    uint32_t rate, average;
    memcpy(&tag, format, 2); memcpy(&channels, format + 2, 2);
    memcpy(&rate, format + 4, 4); memcpy(&average, format + 8, 4);
    memcpy(&align, format + 12, 2); memcpy(&bits, format + 14, 2); memcpy(&extra, format + 16, 2);
    uint16_t samples = 0u;
    if (tag == 0x69u && extra == 2u && span(desc[3], 20u)) memcpy(&samples, format + 18, 2);
    result = RECOMP_DSOUND_INVALID_PARAM;
    if (channels < 1 || channels > 2 || rate < 1000 || rate > 200000 ||
        (tag != 1u && tag != 0x69u) ||
        (tag == 1u && ((bits != 8 && bits != 16) || align != channels * bits / 8 ||
            average != rate * align || extra != 0u)) ||
        (tag == 0x69u && (bits != 4 || align != 36u * channels || extra != 2u ||
            !span(desc[3], 20u) || samples != 64u))) {
        kernel_return(4u, result); return;
    }
    const bool is3d = (desc[1] & 0x10u) != 0u;
    if (is3d && (channels != 1u || (tag == 1u && bits != 16u))) recomp_stop(2, "dsound:buffer-3d-format-unbound");
    uint32_t slot = 0u;
    RecompDsoundMixModel mix;
    recomp_dsound_mix_default(&mix);
    result = read_mix(&mix, desc[4], true, is3d);
    if (result != RECOMP_DSOUND_OK) { kernel_return(4u, result); return; }
    while (slot < BUFFER_COUNT && buffers[slot].root) ++slot;
    if (slot == BUFFER_COUNT) { kernel_return(4u, RECOMP_DSOUND_OUT_OF_MEMORY); return; }
    Buffer *buffer = &buffers[slot];
    *buffer = (Buffer){.root = recomp_kernel_allocate_pool(ROOT_SIZE),
        .settings = recomp_kernel_allocate_pool(SETTINGS_SIZE), .references = 1u,
        .flags = desc[1], .format = tag, .channels = channels, .bits = bits, .align = align,
        .rate = rate, .frequency = rate, .headroom = (desc[1] & 0x382010u) ? 0u : 600u, .is3d = is3d,
        /* 3D defaults from the 5849 template at 0x3803B4. */
        .min_distance = 1.0f, .max_distance = 1000000000.0f, .distance_factor = 1.0f, .rolloff = 1.0f,
        .pan_rear = -10000};
    if (!buffer->root || !buffer->settings) { dispose(buffer); kernel_return(4u, RECOMP_DSOUND_OUT_OF_MEMORY); return; }
    buffer->volume = -(int32_t)buffer->headroom;
    buffer->mix = mix;
    memset(recomp_memory(buffer->root, ROOT_SIZE), 0, ROOT_SIZE);
    memset(recomp_memory(buffer->settings, SETTINGS_SIZE), 0, SETTINGS_SIZE);
    write32(buffer->root, 0x46c1d8u);
    write32(buffer->root + 4u, 1u);
    write32(buffer->root + 8u, recomp_dsound_current_device() - 8u);
    write32(buffer->root + 0x10u, read32(recomp_dsound_current_device()));
    write32(buffer->root + 0x14u, buffer->root + 0x14u);
    write32(buffer->root + 0x18u, buffer->root + 0x14u);
    write32(buffer->root + PUBLIC_OFFSET, buffer->settings);
    write32(buffer->settings + 8u, buffer->flags);
    write32(buffer->settings + 0x1cu, (uint32_t)buffer->volume);
    write32(buffer->settings + 0x20u, buffer->headroom);
    write_mix(buffer);
    update_3d(buffer);
    buffer->hrtf.rendered = buffer->hrtf.filter;
    result = set_data(buffer, 0u, desc[2]);
    if (result != RECOMP_DSOUND_OK) dispose(buffer);
    else { recomp_dsound_device_add_owner(); write32(output, buffer->root + PUBLIC_OFFSET); }
    kernel_return(4u, result);
}

static uint32_t release_buffer(Buffer *buffer)
{
    uint32_t references = --buffer->references;
    write32(buffer->root + 4u, references);
    if (!references) { dispose(buffer); recomp_dsound_device_release_owner(); }
    return references;
}

enum Operation { RELEASE, DATA, LOCK, UNLOCK, PLAY, STOP, STOP_EX, STATUS, POSITION, SEEK, LOOP, FREQUENCY, VOLUME, HEADROOM, MIX_BINS, MIX_VOLUMES,
    POSITION_3D, VELOCITY_3D, MIN_DISTANCE, MAX_DISTANCE, DISTANCE_FACTOR, ROLLOFF };
static void buffer_call(enum Operation operation, uint32_t arguments)
{
    Buffer *buffer = buffer_at(kernel_arg(1u));
    if (operation != RELEASE && !recomp_audio_output_ready()) recomp_stop(2, "dsound:audio-output-failed");
    uint32_t value = arguments >= 2u ? kernel_arg(2u) : 0u;
    uint32_t result = RECOMP_DSOUND_OK;
    uint64_t now = now_ms();
    switch (operation) {
    case MIX_BINS: case MIX_VOLUMES:
        result = read_mix(&buffer->mix, value, operation == MIX_BINS, buffer->is3d);
        if (!result) { write_mix(buffer); update_3d(buffer); apply_routing(buffer); }
        break;
    case POSITION_3D: case VELOCITY_3D: case MIN_DISTANCE: case MAX_DISTANCE: case DISTANCE_FACTOR: case ROLLOFF: {
        /* The 5849 setters store without validation (0x364C2B-0x364D68).
           shortcut: deferred (apply=1) values take effect at once instead of at
           commit; track deferral if a game sets and commits in different frames. */
        if (!buffer->is3d) recomp_stop(2, "dsound:buffer-not-3d");
        float *target = operation == POSITION_3D ? buffer->position : operation == VELOCITY_3D ? buffer->velocity
            : operation == MIN_DISTANCE ? &buffer->min_distance : operation == MAX_DISTANCE ? &buffer->max_distance
            : operation == DISTANCE_FACTOR ? &buffer->distance_factor : &buffer->rolloff;
        for (uint32_t i = 0u; i + 2u < arguments; ++i) {
            uint32_t bits = kernel_arg(2u + i);
            memcpy(target + i, &bits, sizeof bits);
        }
        update_3d(buffer);
        break;
    }
    case RELEASE: result = release_buffer(buffer); break;
    case DATA: result = set_data(buffer, value, kernel_arg(3u)); break;
    case LOCK: {
        uint32_t bytes = kernel_arg(3u), flags = kernel_arg(8u), size = buffer->size;
        uint32_t p1 = kernel_arg(4u), n1 = kernel_arg(5u), p2 = kernel_arg(6u), n2 = kernel_arg(7u);
        if (!span(p1, 4u) || !span(n1, 4u) || (p2 && !span(p2, 4u)) ||
            (n2 && !span(n2, 4u)) || (!p2 != !n2)) { result = RECOMP_DSOUND_POINTER_ERROR; break; }
        pump(buffer, now);
        if (flags & 1u) value = source_cursor(buffer);
        if (flags & 2u) bytes = size;
        if (!size || value >= size || !bytes || bytes > size || (flags & ~3u) ||
            value % buffer->align || bytes % buffer->align) { result = RECOMP_DSOUND_INVALID_PARAM; break; }
        uint32_t first = bytes < size - value ? bytes : size - value;
        if (bytes > first && !p2) { result = RECOMP_DSOUND_INVALID_PARAM; break; }
        write32(p1, buffer->data + value); write32(n1, first);
        if (p2) { write32(p2, bytes > first ? buffer->data : 0u); write32(n2, bytes - first); }
        break;
    }
    case UNLOCK: {
        uint32_t size = kernel_arg(3u), second = kernel_arg(4u), second_size = kernel_arg(5u);
        uint32_t offset = canonical(value), data = canonical(buffer->data);
        if (!buffer->size || offset < data || offset >= data + buffer->size ||
            size > data + buffer->size - offset || second_size > buffer->size - size ||
            (second_size && (offset + size != data + buffer->size || canonical(second) != data))) result = RECOMP_DSOUND_INVALID_PARAM;
        break;
    }
    case PLAY: {
        uint32_t flags = kernel_arg(4u);
        if (flags & ~3u) recomp_stop(2, "dsound:buffer-play-flags-unbound:%08x", flags);
        pump(buffer, now);
        uint32_t continuing = buffer->model.playing && !(flags & RECOMP_DSOUND_PLAY_FROMSTART);
        result = recomp_dsound_buffer_play(&buffer->model, flags, now);
        if (!result && !continuing) reset_output(buffer);
        break;
    }
    case STOP_EX:
        if (value || kernel_arg(3u) || kernel_arg(4u)) recomp_stop(2, "dsound:buffer-stop-ex-unbound");
        /* Zero time/flags have immediate Stop semantics. */
    case STOP:
        pump(buffer, now); recomp_dsound_buffer_stop(&buffer->model, now);
        reset_output(buffer); break;
    case STATUS:
        if (!span(value, 4u)) { result = RECOMP_DSOUND_POINTER_ERROR; break; }
        pump(buffer, now);
        write32(value, buffer->model.playing ? 1u | ((buffer->model.play_flags & 1u) ? 4u : 0u) : 0u); break;
    case POSITION: {
        uint32_t write = kernel_arg(3u);
        if ((!value && !write) || (value && !span(value, 4u)) || (write && !span(write, 4u))) {
            result = RECOMP_DSOUND_POINTER_ERROR; break;
        }
        pump(buffer, now);
        if (value) write32(value, source_cursor(buffer));
        if (write) write32(write, source_cursor(buffer));
        break;
    }
    case SEEK:
        if (value >= buffer->size || value % buffer->align) { result = RECOMP_DSOUND_INVALID_PARAM; break; }
        pump(buffer, now);
        result = recomp_dsound_buffer_set_position(&buffer->model, decoded_bytes(buffer, value), now);
        if (!result) reset_output(buffer); break;
    case LOOP: {
        uint32_t length = kernel_arg(3u);
        if (value >= buffer->size || !length || length > buffer->size - value ||
            value % buffer->align || length % buffer->align) { result = RECOMP_DSOUND_INVALID_PARAM; break; }
        pump(buffer, now);
        buffer->model.loop_start_bytes = decoded_bytes(buffer, value);
        buffer->model.loop_end_bytes = decoded_bytes(buffer, value + length);
        reset_output(buffer); break;
    }
    case FREQUENCY:
        if (!value) value = buffer->rate;
        if (value < 1000u || value > 200000u) { result = RECOMP_DSOUND_INVALID_PARAM; break; }
        pump(buffer, now); buffer->frequency = value;
        if (buffer->size) result = recomp_dsound_buffer_set_frequency(&buffer->model, pitch_hz(buffer), now); break;
    case VOLUME: case HEADROOM: {
        int32_t input = (int32_t)value;
        int64_t volume = operation == HEADROOM
            ? (int64_t)buffer->volume + buffer->headroom - value : (int64_t)input - buffer->headroom;
        /* Native arithmetic rejects overflow; retail has no headroom bound. */
        if ((operation == VOLUME && (input < -10000 || input > 0)) || volume < INT32_MIN || volume > 0) {
            result = RECOMP_DSOUND_INVALID_PARAM; break;
        }
        pump(buffer, now); buffer->volume = (int32_t)volume;
        if (operation == HEADROOM) { buffer->headroom = value; write32(buffer->settings + 0x20u, value); }
        write32(buffer->settings + 0x1cu, (uint32_t)buffer->volume); break;
    }
    }
    kernel_return(arguments, result);
}

#define METHOD(name, op, count) static void name(void) { buffer_call(op, count); }
METHOD(release, RELEASE, 1u) METHOD(data, DATA, 3u) METHOD(lock_buffer, LOCK, 8u)
METHOD(unlock, UNLOCK, 5u) METHOD(play, PLAY, 4u) METHOD(stop, STOP, 1u)
METHOD(stop_ex, STOP_EX, 4u) METHOD(status, STATUS, 2u) METHOD(position, POSITION, 3u)
METHOD(seek, SEEK, 2u) METHOD(loop, LOOP, 3u) METHOD(frequency, FREQUENCY, 2u)
METHOD(volume, VOLUME, 2u) METHOD(headroom, HEADROOM, 2u)
METHOD(mix_bins, MIX_BINS, 2u) METHOD(mix_volumes, MIX_VOLUMES, 2u)
METHOD(position_3d, POSITION_3D, 5u) METHOD(velocity_3d, VELOCITY_3D, 5u)
METHOD(min_distance, MIN_DISTANCE, 3u) METHOD(max_distance, MAX_DISTANCE, 3u)
METHOD(distance_factor, DISTANCE_FACTOR, 3u) METHOD(rolloff, ROLLOFF, 3u)
static void private_release(void)
{
    kernel_return(1u, release_buffer(buffer_at(kernel_arg(1u) + PUBLIC_OFFSET)));
}
static void unbound_stream(void) { recomp_stop(2, "dsound:stream-unbound"); }

RecompFunction recomp_dsound_buffer_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x365fbcu: return create_buffer;
    case 0x363641u: return release; case 0x363b91u: return private_release;
    case 0x365793u: return data; case 0x3648dau: return lock_buffer; case 0x36363cu: return unlock;
    case 0x364802u: return play; case 0x364826u: return stop; case 0x36483eu: return stop_ex;
    case 0x364882u: return status; case 0x36489eu: return position; case 0x3648beu: return seek;
    case 0x364862u: return loop; case 0x365121u: return frequency;
    case 0x3647aeu: return volume; case 0x3647cau: return headroom;
    case 0x3647e6u: return mix_volumes; case 0x36513du: return mix_bins;
    /* IDirectSoundBuffer 3D thunks verified in /DOA2.xbe (handle - 0x1C to CDirectSoundBuffer). */
    case 0x365159u: return max_distance; case 0x36517du: return min_distance;
    case 0x3651a1u: return position_3d; case 0x3651d6u: return velocity_3d;
    case 0x36520bu: return distance_factor; case 0x36522fu: return rolloff;
    case 0x363f70u: case 0x363fb7u: case 0x364005u: case 0x364104u:
    case 0x364155u: case 0x36406cu: case 0x3640b9u: case 0x36490au:
    case 0x36490fu: case 0x364914u: case 0x365253u: case 0x366257u: return unbound_stream;
    default: return NULL;
    }
}
