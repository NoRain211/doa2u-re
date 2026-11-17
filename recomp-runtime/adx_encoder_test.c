/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The encoder writes what adx_decoder.c reads: a loop starts on a frame and a
   sector, the header counts its leading silence, and the WAV walk stops at a
   chunk that runs past the buffer instead of stalling. */
#include "adx_decoder.h"
#include "adx_encoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ADX encoder: line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void putle32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0u; i < 4u; ++i) p[i] = (uint8_t)(v >> (8u * i));
}

/* A looped stereo track comes back with a sector-aligned loop and its signal. */
static int test_looped(void)
{
    enum { FRAMES = 2205, RATE = 22050 };
    int16_t *pcm = malloc(FRAMES * 2 * sizeof *pcm);
    if (pcm == NULL) return 1;
    for (unsigned i = 0u; i < FRAMES; ++i) {
        pcm[2u * i] = (int16_t)(12000.0 * sin(6.28318530718 * 440.0 * i / RATE));
        pcm[2u * i + 1] = (int16_t)(9000.0 * sin(6.28318530718 * 1250.0 * i / RATE + 1.0));
    }
    size_t size = 0u;
    uint8_t *adx = recomp_adx_encode(pcm, FRAMES, 2u, RATE, 700, FRAMES, &size);
    CHECK(adx != NULL);
    if (adx == NULL) { free(pcm); return 1; }
    RecompAdxDecoder decoder;
    const uint32_t data = recomp_adx_open(&decoder, adx, (uint32_t)size);
    CHECK(data != 0u && decoder.channels == 2u && decoder.sample_rate == RATE);
    /* Sample 700 follows 4 samples of silence, so the loop starts at 704: frame
       22, whose bytes begin on a 2048-byte sector. */
    const uint32_t lead = (uint32_t)adx[0x14] << 8 | adx[0x15];
    const uint32_t begin = be32(adx + 0x1c), begin_byte = be32(adx + 0x20);
    CHECK(lead == 4u && begin == 704u && begin_byte % 2048u == 0u && begin_byte == data + 22u * 36u);
    CHECK(be32(adx + 0x0c) == FRAMES + lead && decoder.total_samples == FRAMES + lead);
    CHECK(be32(adx + 0x24) == FRAMES + lead && be32(adx + 0x28) == data + (FRAMES + lead + 31u) / 32u * 36u);
    double error = 0.0, signal = 0.0;
    uint32_t at = data, decoded = 0u;
    int16_t out[RECOMP_ADX_FRAME_SAMPLES * 2];
    for (uint32_t n; (n = recomp_adx_decode(&decoder, adx + at, out)) != 0u; at += 36u) {
        for (uint32_t i = 0u; i < n; ++i) {
            const int64_t source = (int64_t)decoded + i - lead;
            for (unsigned ch = 0u; ch < 2u; ++ch) {
                const double want = source >= 0 ? pcm[source * 2 + ch] : 0.0, diff = out[i * 2 + ch] - want;
                error += diff * diff;
                signal += want * want;
            }
        }
        decoded += n;
    }
    CHECK(decoded == FRAMES + lead);
    const double snr = 10.0 * log10(signal / (error + 1.0));
    if (snr < 20.0) {
        fprintf(stderr, "ADX encoder: looped SNR %.1f dB\n", snr);
        return 1;
    }
    free(adx);
    free(pcm);
    return 0;
}

/* A track played once has no loop header, and the end marker closes its list. */
static int test_unlooped(void)
{
    enum { FRAMES = 100, RATE = 22050 };
    int16_t pcm[FRAMES];
    for (unsigned i = 0u; i < FRAMES; ++i) pcm[i] = (int16_t)((int)i * 200 - 10000);
    size_t size = 0u;
    uint8_t *adx = recomp_adx_encode(pcm, FRAMES, 1u, RATE, -1, -1, &size);
    CHECK(adx != NULL);
    if (adx == NULL) return 1;
    CHECK(size == 0x24u + 4u * 18u + 18u);
    CHECK(adx[0x12] == 3u && adx[0x14] == 0u && adx[0x15] == 0u && adx[0x16] == 0u);
    RecompAdxDecoder decoder;
    uint32_t at = recomp_adx_open(&decoder, adx, (uint32_t)size);
    CHECK(at == 0x24u && decoder.total_samples == FRAMES);
    uint32_t decoded = 0u;
    int16_t out[RECOMP_ADX_FRAME_SAMPLES];
    for (uint32_t n; (n = recomp_adx_decode(&decoder, adx + at, out)) != 0u; at += 18u) decoded += n;
    CHECK(decoded == FRAMES && adx[at] == 0x80u && adx[at + 1] == 0x01u);
    free(adx);
    return 0;
}

/* A one-frame mono WAV: 8- or 24-bit integer, or 32-bit float, little endian. */
static size_t wave(uint8_t *wav, uint32_t format, uint32_t bits, uint32_t value)
{
    const uint32_t width = bits / 8u, size = 12u + 8u + 16u + 8u + width;
    memset(wav, 0, size);
    memcpy(wav, "RIFF", 4u); putle32(wav + 4, size - 8u); memcpy(wav + 8, "WAVEfmt ", 8u);
    putle32(wav + 16, 16u);
    wav[20] = (uint8_t)format;
    wav[22] = 1u;
    putle32(wav + 24, 44100u); putle32(wav + 28, 44100u * width);
    wav[32] = (uint8_t)width; wav[34] = (uint8_t)bits;
    memcpy(wav + 36, "data", 4u); putle32(wav + 40, width);
    for (uint32_t i = 0u; i < width; ++i) wav[44u + i] = (uint8_t)(value >> (8u * i));
    return size;
}

static int test_wav(void)
{
    uint8_t wav[48];
    uint32_t frames, channels, rate;
    int64_t start, end;
    size_t size = wave(wav, 1u, 8u, 0x80u);
    int16_t *pcm = recomp_wav_read(wav, size, &frames, &channels, &rate, &start, &end);
    CHECK(pcm != NULL && frames == 1u && channels == 1u && rate == 44100u && pcm[0] == 0 && start < 0);
    free(pcm);
    size = wave(wav, 1u, 24u, 0x123456u);
    pcm = recomp_wav_read(wav, size, &frames, &channels, &rate, &start, &end);
    CHECK(pcm != NULL && pcm[0] == 0x1234);
    free(pcm);
    size = wave(wav, 3u, 32u, 0x3f800000u); /* 1.0f */
    pcm = recomp_wav_read(wav, size, &frames, &channels, &rate, &start, &end);
    CHECK(pcm != NULL && pcm[0] == 32767);
    free(pcm);
    return 0;
}

/* A chunk longer than the file ends the walk: with a 32-bit size_t the advance
   would wrap, so the loop would stall or step backward forever. */
static int test_bad_chunk(void)
{
    uint8_t wav[12u + 8u + 4u];
    memset(wav, 0, sizeof wav);
    memcpy(wav, "RIFF", 4u); memcpy(wav + 8, "WAVE", 4u);
    memcpy(wav + 12, "junk", 4u);
    putle32(wav + 16, 0xFFFFFFF7u);
    uint32_t frames, channels, rate;
    int64_t start, end;
    CHECK(recomp_wav_read(wav, sizeof wav, &frames, &channels, &rate, &start, &end) == NULL);
    putle32(wav + 16, 0xFFFFFFF6u);
    CHECK(recomp_wav_read(wav, sizeof wav, &frames, &channels, &rate, &start, &end) == NULL);
    return 0;
}

int main(void)
{
    if (test_looped() || test_unlooped() || test_wav() || test_bad_chunk()) return 1;
    printf("ADX encoder: ok\n");
    return 0;
}
