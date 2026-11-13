/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "adx_encoder.h"
#include "adx_decoder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME = RECOMP_ADX_FRAME_BYTES, SAMPLES = RECOMP_ADX_FRAME_SAMPLES, SECTOR = 2048, CUTOFF = 500 };

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }

/* The predictor adx_decoder.c (and the game's library) derives from the cutoff. */
static void coefficients(uint32_t rate, int32_t *coef1, int32_t *coef2)
{
    const float z = cosf(6.28318530718f * (float)CUTOFF / (float)rate);
    const float a = 1.41421353816986f - z, b = 0.414213538169861f;
    const float c = (a - sqrtf((a + b) * (a - b))) / b;
    *coef1 = (int16_t)(c * 8192.0f);
    *coef2 = (int16_t)(c * c * -4096.0f);
}

/* One channel's 32 samples into an 18-byte frame, from and into its history;
   decodes exactly as recomp_adx_decode does (stored scale + 1). */
static void encode_frame(const int16_t *in, uint32_t stride, int32_t coef1, int32_t coef2,
    int32_t history[2], uint8_t frame[FRAME])
{
    int32_t peak = 0, h0 = history[0], h1 = history[1];
    for (uint32_t i = 0u; i < SAMPLES; ++i) {
        const int32_t predicted = (coef1 * h0 + coef2 * h1) >> 12;
        const int32_t d = in[i * stride] - predicted;
        if (abs(d) > peak) peak = abs(d);
        h1 = h0;
        h0 = in[i * stride];
    }
    int32_t scale = (peak + 6) / 7;
    if (scale < 1) scale = 1;
    if (scale > 0x8000) scale = 0x8000;
    /* Quantising against the decoder's own reconstruction keeps the error
       from building up; a larger scale is tried if a sample would clip. */
    for (;;) {
        int32_t r0 = history[0], r1 = history[1];
        bool clipped = false;
        memset(frame, 0, FRAME);
        put16(frame, (uint32_t)(scale - 1));
        for (uint32_t i = 0u; i < SAMPLES; ++i) {
            const int32_t predicted = (coef1 * r0 + coef2 * r1) >> 12;
            const int32_t d = in[i * stride] - predicted;
            int32_t n = d >= 0 ? (d + scale / 2) / scale : -((-d + scale / 2) / scale);
            if (n > 7) { n = 7; clipped = clipped || d > 7 * scale + scale / 2; }
            if (n < -8) { n = -8; clipped = clipped || d < -8 * scale - scale / 2; }
            int32_t sample = n * scale + predicted;
            sample = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
            r1 = r0;
            r0 = sample;
            frame[2u + i / 2u] |= (uint8_t)((n & 15) << (i & 1u ? 0 : 4));
        }
        if (!clipped || scale >= 0x8000) {
            history[0] = r0;
            history[1] = r1;
            return;
        }
        scale += scale / 4 + 1;
        if (scale > 0x8000) scale = 0x8000;
    }
}

uint8_t *recomp_adx_encode(const int16_t *pcm, uint32_t frames, uint32_t channels, uint32_t sample_rate,
    int64_t loop_start, int64_t loop_end, size_t *size)
{
    if (pcm == NULL || size == NULL || channels == 0u || channels > 2u || sample_rate == 0u) return NULL;
    const bool loop = loop_start >= 0 && loop_end > loop_start && loop_end <= (int64_t)frames;
    /* Silence first, so the loop starts on a frame. */
    const uint32_t lead = loop ? (uint32_t)((SAMPLES - loop_start % SAMPLES) % SAMPLES) : 0u;
    const uint32_t total = frames + lead, blocks = (total + SAMPLES - 1u) / SAMPLES;
    const uint32_t block_bytes = FRAME * channels;
    uint32_t data = 0x24u; /* header, then "(c)CRI" just before the data */
    uint64_t begin_byte = 0u, end_byte = 0u;
    if (loop) {
        const uint64_t before = (uint64_t)(loop_start + lead) / SAMPLES * block_bytes;
        data = 0x2cu + 6u;
        while ((data + before) % SECTOR != 0u) ++data;
        begin_byte = data + before;
        end_byte = data + ((uint64_t)(loop_end + lead) + SAMPLES - 1u) / SAMPLES * block_bytes;
    }
    const size_t bytes = data + (size_t)blocks * block_bytes + block_bytes;
    uint8_t *out = calloc(1u, bytes);
    if (out == NULL) return NULL;
    out[0] = 0x80u;
    put16(out + 2, data - 4u);
    out[4] = 3u;
    out[5] = FRAME;
    out[6] = 4u;
    out[7] = (uint8_t)channels;
    put32(out + 8, sample_rate);
    put32(out + 0x0c, total);
    put16(out + 0x10, CUTOFF);
    out[0x12] = 3u;
    if (loop) {
        put16(out + 0x14, lead);
        put16(out + 0x16, 1u);
        put32(out + 0x18, 1u);
        put32(out + 0x1c, (uint32_t)(loop_start + lead));
        put32(out + 0x20, (uint32_t)begin_byte);
        put32(out + 0x24, (uint32_t)(loop_end + lead));
        put32(out + 0x28, (uint32_t)end_byte);
    }
    memcpy(out + data - 6u, "(c)CRI", 6u);

    int32_t coef1, coef2, history[2][2] = {{0}};
    coefficients(sample_rate, &coef1, &coef2);
    int16_t block[SAMPLES * 2];
    for (uint32_t b = 0u; b < blocks; ++b) {
        for (uint32_t i = 0u; i < SAMPLES; ++i) {
            const int64_t at = (int64_t)b * SAMPLES + i - lead;
            for (uint32_t ch = 0u; ch < channels; ++ch)
                block[i * channels + ch] = at >= 0 && at < (int64_t)frames ? pcm[(size_t)at * channels + ch] : 0;
        }
        for (uint32_t ch = 0u; ch < channels; ++ch)
            encode_frame(block + ch, channels, coef1, coef2, history[ch],
                out + data + (size_t)b * block_bytes + ch * FRAME);
    }
    /* The end marker: a frame whose scale reads 0x8001. */
    uint8_t *end = out + data + (size_t)blocks * block_bytes;
    end[0] = 0x80u;
    end[1] = 0x01u;
    end[3] = (uint8_t)(block_bytes - 4u);
    *size = bytes;
    return out;
}

/* ---- WAV ------------------------------------------------------------------ */

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

int16_t *recomp_wav_read(const uint8_t *data, size_t size, uint32_t *frames, uint32_t *channels,
    uint32_t *sample_rate, int64_t *loop_start, int64_t *loop_end)
{
    *loop_start = *loop_end = -1;
    if (size < 12u || memcmp(data, "RIFF", 4u) != 0 || memcmp(data + 8, "WAVE", 4u) != 0) return NULL;
    uint32_t format = 0u, count = 0u, rate = 0u, bits = 0u;
    const uint8_t *samples = NULL;
    size_t sample_bytes = 0u;
    for (size_t at = 12u; at + 8u <= size;) {
        const uint32_t length = le32(data + at + 4);
        const uint8_t *body = data + at + 8;
        const size_t available = size - at - 8u < length ? size - at - 8u : length;
        if (memcmp(data + at, "fmt ", 4u) == 0 && available >= 16u) {
            format = le16(body);
            count = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (format == 0xfffeu && available >= 26u) format = le16(body + 24); /* WAVE_FORMAT_EXTENSIBLE */
        } else if (memcmp(data + at, "data", 4u) == 0) {
            samples = body;
            sample_bytes = available;
        } else if (memcmp(data + at, "smpl", 4u) == 0 && available >= 36u + 24u && le32(body + 28) > 0u) {
            *loop_start = le32(body + 36 + 8);
            *loop_end = (int64_t)le32(body + 36 + 12) + 1; /* smpl's end is inclusive */
        }
        at += 8u + length + (length & 1u);
    }
    const bool integer = format == 1u && (bits == 8u || bits == 16u || bits == 24u || bits == 32u);
    const bool real = format == 3u && bits == 32u;
    if (samples == NULL || count == 0u || count > 2u || rate == 0u || (!integer && !real)) return NULL;
    const uint32_t width = bits / 8u, n = (uint32_t)(sample_bytes / (width * count));
    int16_t *out = malloc((size_t)n * count * sizeof *out + 1u);
    if (out == NULL) return NULL;
    for (size_t i = 0u; i < (size_t)n * count; ++i) {
        const uint8_t *p = samples + i * width;
        int32_t v;
        if (real) {
            float f;
            memcpy(&f, p, 4u);
            v = (int32_t)lrintf((f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f) * 32767.0f);
        } else if (width == 1u) {
            v = ((int32_t)p[0] - 128) << 8;
        } else if (width == 2u) {
            v = (int16_t)le16(p);
        } else if (width == 3u) {
            v = (int32_t)((uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 16;
        } else {
            v = (int32_t)le32(p) >> 16;
        }
        out[i] = (int16_t)v;
    }
    if (*loop_end > (int64_t)n || *loop_start >= *loop_end) *loop_start = *loop_end = -1;
    *frames = n;
    *channels = count;
    *sample_rate = rate;
    return out;
}
