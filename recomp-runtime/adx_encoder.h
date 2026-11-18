/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_ADX_ENCODER_H
#define DOA2U_RECOMP_ADX_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encodes 16-bit PCM as the ADX DOA2U's own music uses: encoding 3, 4-bit
   samples in 18-byte frames per channel, a 500 Hz high-pass predictor and a
   version 3 header. A loop is written as CRI writes one: silence before the
   first sample so the loop starts on a frame, and a header padded so that
   frame starts on a 2048-byte sector. adx_decoder.c reads it back.

   pcm holds frames * channels interleaved samples; channels is 1 or 2.
   loop_start < loop_end <= frames loops; anything else plays once.
   Returns a malloc'd file and its size, or NULL. */
uint8_t *recomp_adx_encode(const int16_t *pcm, uint32_t frames, uint32_t channels, uint32_t sample_rate,
    int64_t loop_start, int64_t loop_end, size_t *size);

/* A RIFF WAVE of 8-, 16-, 24- or 32-bit integer or 32-bit float PCM, 1 or 2
   channels, as interleaved 16-bit samples (malloc'd). Its smpl chunk's first
   loop, if any, goes to loop_start and loop_end (in frames, end exclusive);
   both are -1 without one. */
int16_t *recomp_wav_read(const uint8_t *data, size_t size, uint32_t *frames, uint32_t *channels,
    uint32_t *sample_rate, int64_t *loop_start, int64_t *loop_end);

#ifdef __cplusplus
}
#endif

#endif
