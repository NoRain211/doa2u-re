#ifndef DOA2U_RECOMP_ADX_DECODER_H
#define DOA2U_RECOMP_ADX_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* Standard CRI ADX as Sofdec movies carry it: encoding 3, 4-bit samples in
   18-byte frames per channel, high-pass predictor from the header's cutoff,
   no encryption. Decodes with the v4 library's predictor rounding. */
enum { RECOMP_ADX_FRAME_BYTES = 18, RECOMP_ADX_FRAME_SAMPLES = 32, RECOMP_ADX_MAX_CHANNELS = 2 };

typedef struct RecompAdxDecoder {
    uint32_t channels, sample_rate, total_samples, decoded;
    int32_t coef1, coef2;
    int32_t history[RECOMP_ADX_MAX_CHANNELS][2];
    bool ended;
} RecompAdxDecoder;

/* Reads the header at the start of the stream. Returns the offset of the
   first frame, or 0 when the header is incomplete or not a supported ADX. */
uint32_t recomp_adx_open(RecompAdxDecoder *decoder, const uint8_t *bytes, uint32_t size);
/* Decodes one frame per channel (18 * channels bytes) into interleaved
   samples. Returns frames written per channel: 32, fewer at the stream's
   sample count, 0 at its end marker or once ended. */
uint32_t recomp_adx_decode(RecompAdxDecoder *decoder, const uint8_t *frames,
    int16_t out[RECOMP_ADX_FRAME_SAMPLES * RECOMP_ADX_MAX_CHANNELS]);

#endif

