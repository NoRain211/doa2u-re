#include "adx_decoder.h"

#include <math.h>
#include <string.h>

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

uint32_t recomp_adx_open(RecompAdxDecoder *decoder, const uint8_t *bytes, uint32_t size)
{
    memset(decoder, 0, sizeof *decoder);
    if (size < 0x14u || be16(bytes) != 0x8000u) return 0u;
    const uint32_t data = be16(bytes + 2) + 4u;
    if (data > size || data < 0x14u || memcmp(bytes + data - 6u, "(c)CRI", 6u) != 0) return 0u;
    const uint32_t channels = bytes[7], rate = be32(bytes + 8), cutoff = be16(bytes + 0x10);
    if (bytes[4] != 3u || bytes[5] != RECOMP_ADX_FRAME_BYTES || bytes[6] != 4u || bytes[0x13] != 0u ||
        channels == 0u || channels > RECOMP_ADX_MAX_CHANNELS || rate == 0u || cutoff == 0u)
        return 0u;
    /* The library computes the predictor in float from the high-pass cutoff. */
    const float z = cosf(6.28318530718f * (float)cutoff / (float)rate);
    const float a = 1.41421353816986f - z, b = 0.414213538169861f;
    const float c = (a - sqrtf((a + b) * (a - b))) / b;
    decoder->channels = channels;
    decoder->sample_rate = rate;
    decoder->total_samples = be32(bytes + 0x0c);
    decoder->coef1 = (int16_t)(c * 8192.0f);
    decoder->coef2 = (int16_t)(c * c * -4096.0f);
    return data;
}

uint32_t recomp_adx_decode(RecompAdxDecoder *decoder, const uint8_t *frames,
    int16_t out[RECOMP_ADX_FRAME_SAMPLES * RECOMP_ADX_MAX_CHANNELS])
{
    if (decoder->ended || decoder->decoded >= decoder->total_samples) return 0u;
    /* An end marker replaces the next frame's scale. */
    if (frames[0] == 0x80u && frames[1] == 0x01u) {
        decoder->ended = true;
        return 0u;
    }
    for (uint32_t ch = 0u; ch < decoder->channels; ++ch) {
        const uint8_t *frame = frames + ch * RECOMP_ADX_FRAME_BYTES;
        const int32_t scale = (int16_t)be16(frame) + 1;
        int32_t *history = decoder->history[ch];
        for (uint32_t i = 0u; i < RECOMP_ADX_FRAME_SAMPLES; ++i) {
            const uint8_t byte = frame[2u + i / 2u];
            const int32_t nibble = ((i & 1u ? byte & 0x0f : byte >> 4) ^ 8) - 8;
            int32_t sample = nibble * scale + ((decoder->coef1 * history[0] + decoder->coef2 * history[1]) >> 12);
            sample = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
            history[1] = history[0];
            history[0] = sample;
            out[i * decoder->channels + ch] = (int16_t)sample;
        }
    }
    uint32_t count = decoder->total_samples - decoder->decoded;
    if (count > RECOMP_ADX_FRAME_SAMPLES) count = RECOMP_ADX_FRAME_SAMPLES;
    decoder->decoded += count;
    return count;
}

