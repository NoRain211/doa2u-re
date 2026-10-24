#ifndef DOA3_RECOMP_DSOUND_HRTF_H
#define DOA3_RECOMP_DSOUND_HRTF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DSOUND 5849 full HRTF, rendered natively from the game's own table.
   Ported from DOA3 (DSOUND 3936); each step names its 5849 routine. */
enum { RECOMP_HRTF_TAPS = 31, RECOMP_HRTF_RATE = 48000, RECOMP_HRTF_HISTORY = 128,
    RECOMP_XTALK_DELAY = 11, RECOMP_HRTF_LAGRANGE_TAPS = 8 };

typedef struct RecompHrtfFilter {
    const int8_t *left, *right; /* 32-byte ear records; NULL = silent */
    int delay;                  /* 48 kHz samples; > 0 delays the left ear */
} RecompHrtfFilter;

/* DOA2's xtalkamp.bin effect 14 (DSP56300): each ear adds a one-pole
   low-passed copy of the other ear's output from 11 samples earlier. It is
   linear, so filtering a summed pair equals filtering each source first.
   Zero-initialize before use; samples are 48 kHz. */
typedef struct RecompXtalk {
    float state[2];                    /* canceller one-pole states, per ear */
    float past[2][RECOMP_XTALK_DELAY]; /* past canceller outputs, per ear */
    uint32_t head;
} RecompXtalk;

typedef struct RecompHrtfVoice {
    RecompHrtfFilter filter, rendered;
    float history[RECOMP_HRTF_HISTORY]; /* mono input at 48 kHz */
    uint32_t head;
    double phase; /* output position between the two middle source samples */
    float source[RECOMP_HRTF_LAGRANGE_TAPS]; /* recent source samples, ring */
    uint32_t source_head;                    /* newest source sample */
    uint32_t fade;                           /* steps into a filter change */
    RecompXtalk xtalk;
} RecompHrtfVoice;

/* Filters one stereo frame in place. */
void recomp_xtalk_process(RecompXtalk *xtalk, float *left, float *right);
/* Filters interleaved stereo float32 frames in place. */
void recomp_xtalk_process_block(RecompXtalk *xtalk, float *pcm, uint32_t frames);

/* Listener-space direction (+x right, +y up, +z ahead) to the library's
   ratio-approximated azimuth (-180..180) and elevation (-90..90). */
void recomp_hrtf_angles(float x, float y, float z, float *azimuth, float *elevation);

/* bank: 1,111 adjacent left/right 32-byte pairs; map: [61][31] little-endian
   u16 pair indices. fold_rear (surround or AC3 speaker config) reuses the
   mirrored front response for directions behind the listener. */
RecompHrtfFilter recomp_hrtf_lookup(const uint8_t *bank, const uint8_t *map,
    float azimuth, float elevation, bool fold_rear);

/* Resamples mono 16-bit input from source_rate to 48 kHz (8-point Lagrange,
   4 source samples of latency) and filters it into
   interleaved float left ear, right ear and the unfiltered resampled voice
   (1.0 = 32768 input units), without clipping or normalizing; taps use the
   APU's 8-bit sign-magnitude float encoding. The ears then pass DOA2's
   crosstalk canceller (xtalkamp.bin effect 14).
   A filter change steps in over 32 output samples, across calls.
   Returns frames written, at most out_frames; consumes all input. */
uint32_t recomp_hrtf_render(RecompHrtfVoice *voice, const int16_t *input,
    uint32_t input_frames, uint32_t source_rate, float *out, uint32_t out_frames);

#ifdef __cplusplus
}
#endif

#endif
