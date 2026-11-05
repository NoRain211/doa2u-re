#include "adx_decoder.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "ADX decoder: line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

/* A synthetic stereo header: frames start at 0x24, 40 samples in total. */
static uint32_t header(uint8_t *bytes, uint32_t rate)
{
    static const uint8_t base[0x24] = {
        0x80, 0x00, 0x00, 0x20, 3, 18, 4, 2, 0, 0, 0, 0, 0, 0, 0, 40, 0x01, 0xf4, 4, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '(', 'c', ')', 'C', 'R', 'I'};
    memcpy(bytes, base, sizeof base);
    bytes[8] = (uint8_t)(rate >> 24); bytes[9] = (uint8_t)(rate >> 16);
    bytes[10] = (uint8_t)(rate >> 8); bytes[11] = (uint8_t)rate;
    return sizeof base;
}

int main(void)
{
    RecompAdxDecoder adx;
    uint8_t bytes[0x24];
    int16_t out[RECOMP_ADX_FRAME_SAMPLES * RECOMP_ADX_MAX_CHANNELS];

    /* Published predictor values for a 500 Hz cutoff. */
    header(bytes, 44100u);
    CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0x24u);
    CHECK(adx.coef1 == 7334 && adx.coef2 == -3283 && adx.channels == 2u && adx.total_samples == 40u);
    header(bytes, 48000u);
    CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0x24u);
    CHECK(adx.coef1 == 7400 && adx.coef2 == -3342);

    /* Encrypted, other encodings, a missing copyright tag and a short read are refused. */
    bytes[0x13] = 8; CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0u); bytes[0x13] = 0;
    bytes[4] = 4; CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0u); bytes[4] = 3;
    bytes[0x1e] = 'x'; CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0u); bytes[0x1e] = '(';
    CHECK(recomp_adx_open(&adx, bytes, 0x20u) == 0u);

    /* With no history a sample is nibble * (scale + 1); the left channel then
       predicts from it. Nibbles run high first and are signed. */
    CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0x24u);
    uint8_t frames[2 * RECOMP_ADX_FRAME_BYTES] = {0};
    frames[1] = 9;                     /* left scale 10 */
    frames[2] = 0x7f;                  /* +7, then -1 */
    frames[18] = 0x7f;                 /* right scale 32768: +7 and -8 clamp */
    frames[18 + 1] = 0xff;
    frames[18 + 2] = 0x78;
    CHECK(recomp_adx_decode(&adx, frames, out) == 32u);
    CHECK(out[0] == 70);
    CHECK(out[2] == -10 + ((7400 * 70) >> 12));
    CHECK(out[1] == 32767 && out[3] == -32768);

    /* The header's sample count trims the second frame; then decoding stops. */
    CHECK(recomp_adx_decode(&adx, frames, out) == 8u);
    CHECK(recomp_adx_decode(&adx, frames, out) == 0u);

    /* The end marker stops a stream before its sample count. */
    CHECK(recomp_adx_open(&adx, bytes, sizeof bytes) == 0x24u);
    frames[0] = 0x80; frames[1] = 0x01;
    CHECK(recomp_adx_decode(&adx, frames, out) == 0u && adx.ended);
    return 0;
}

