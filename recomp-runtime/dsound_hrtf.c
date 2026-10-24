#include "dsound_hrtf.h"

#include <math.h>
#include <stdlib.h>

enum { FADE_STEPS = 32 };

/* The APU resamples in hardware with 8-point Lagrange interpolation (retail
   measurements, xemu PR 3105), between the two middle points of the window.
   Whole positions return a source sample exactly. */
static float resample(const RecompHrtfVoice *voice)
{
    const double t = (RECOMP_HRTF_LAGRANGE_TAPS / 2 - 1) + voice->phase;
    double sum = 0.0;
    for (int j = 0; j < RECOMP_HRTF_LAGRANGE_TAPS; ++j) {
        double weight = 1.0;
        for (int m = 0; m < RECOMP_HRTF_LAGRANGE_TAPS; ++m)
            if (m != j) weight *= (t - m) / (j - m);
        sum += weight * voice->source[(voice->source_head + 1u + (uint32_t)j) % RECOMP_HRTF_LAGRANGE_TAPS];
    }
    return (float)sum;
}

static void push_source(RecompHrtfVoice *voice, int16_t sample)
{
    voice->source_head = (voice->source_head + 1u) % RECOMP_HRTF_LAGRANGE_TAPS;
    voice->source[voice->source_head] = sample;
}

void recomp_hrtf_angles(float x, float y, float z, float *azimuth, float *elevation)
{
    /* 5849 0x36662F: piecewise ratio approximations, not atan2/asin. */
    float h = sqrtf(x * x + z * z), ax = fabsf(x), az = fabsf(z), ay = fabsf(y);
    float a = 0.0f, e;

    if (h != 0.0f) {
        a = az > ax ? 45.0f * ax / az : 90.0f - 45.0f * az / ax;
        if (z < 0.0f) a = 180.0f - a;
        if (x < 0.0f) a = -a;
    }
    if (h == 0.0f && ay == 0.0f) e = 0.0f;
    else e = ay < h ? 45.0f * ay / h : 90.0f - 45.0f * h / ay;
    *azimuth = a;
    *elevation = y < 0.0f ? -e : e;
}

/* 5849 0x36B5F2: CVTTSS2SI truncation toward zero. */
static int quantize(float t)
{
    return (int)t;
}

RecompHrtfFilter recomp_hrtf_lookup(const uint8_t *bank, const uint8_t *map,
    float azimuth, float elevation, bool fold_rear)
{
    /* 5849 0x366BFE; 0x369CAE sets fold_rear for surround or AC3 speakers. */
    int e = 6 * (quantize(elevation + (elevation >= 0.0f ? 3.0f : -3.0f)) / 6);
    int a = 0;
    if (e < -90 || e > 90) e = 0;
    if (abs(e) != 90) {
        int step = abs(e) > 60 ? 12 : abs(e) > 30 ? 6 : 3;
        a = step * (quantize(fabsf(azimuth) + step / 2.0f) / step);
        if (fold_rear && a > 90) a = 180 - a;
        if (a < 0 || a > 180) a = 0;
    }
    const uint8_t *entry = map + 2 * ((a / 3) * 31 + (e + 90) / 6);
    const int8_t *ear0 = (const int8_t *)bank + 32 * (entry[0] | entry[1] << 8);
    /* 0x369CAE: the canonical record's delay byte, negated with the ears. */
    if (azimuth >= 0.0f) return (RecompHrtfFilter){ear0, ear0 + 32, ear0[31]};
    return (RecompHrtfFilter){ear0 + 32, ear0, -ear0[31]};
}

/* 5849 hrtf_upload_voice_entry (0x369CAE) writes the table bytes unchanged to
   the APU HRIR registers, so the encoding is the APU's. Each tap is a sign bit,
   3-bit exponent and 4-bit mantissa (1/2048 to 31/32): of the readings tried
   on DOA2's own bank, only this one makes the near ear louder than the far
   ear at every elevation-0 azimuth, with the difference growing toward high
   frequencies. Taps apply unnormalized; the absolute scale is unconfirmed. */
static float tap_gain(int8_t tap)
{
    const uint8_t t = (uint8_t)tap;
    const int e = (t >> 4) & 7, m = t & 15;
    const float v = (float)(e ? (16 + m) << (e - 1) : m) / 2048.0f;
    return t & 0x80u ? -v : v;
}

static float tap(const RecompHrtfVoice *voice, const int8_t *ear, int delay)
{
    float sum = 0.0f;
    if (ear == NULL) return 0.0f;
    for (int k = 0; k < RECOMP_HRTF_TAPS; ++k)
        sum += tap_gain(ear[k]) * voice->history[(voice->head - (uint32_t)(k + delay)) % RECOMP_HRTF_HISTORY];
    return sum;
}

/* The effect writes these 24-bit coefficients on its first frame. DSOUND gives
   both ears of a pair one route gain, so filtering each source equals
   filtering the summed bins. */
void recomp_xtalk_process(RecompXtalk *xtalk, float *left, float *right)
{
    const float pole = (float)0x7645a1 / 0x800000, feed = (float)(0xf88313 - 0x1000000) / 0x800000;
    float *past_left = &xtalk->past[0][xtalk->head];
    float *past_right = &xtalk->past[1][xtalk->head];
    xtalk->state[1] = pole * xtalk->state[1] + feed * *past_left;
    xtalk->state[0] = pole * xtalk->state[0] + feed * *past_right;
    *left += xtalk->state[0];
    *right += xtalk->state[1];
    *past_left = *left;
    *past_right = *right;
    xtalk->head = (xtalk->head + 1u) % RECOMP_XTALK_DELAY;
}

void recomp_xtalk_process_block(RecompXtalk *xtalk, float *pcm, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; ++i)
        recomp_xtalk_process(xtalk, &pcm[2u * i], &pcm[2u * i + 1u]);
}

uint32_t recomp_hrtf_render(RecompHrtfVoice *voice, const int16_t *input,
    uint32_t input_frames, uint32_t source_rate, float *out, uint32_t out_frames)
{
    const double step = (double)source_rate / RECOMP_HRTF_RATE;
    uint32_t written = 0u, consumed = 0u;

    while (consumed < input_frames && written < out_frames) {
        if (voice->phase >= 1.0) {
            push_source(voice, input[consumed++]);
            voice->phase -= 1.0;
            continue;
        }
        voice->head = (voice->head + 1u) % RECOMP_HRTF_HISTORY;
        voice->history[voice->head] = resample(voice);
        voice->phase += step;

        const RecompHrtfFilter from = voice->rendered, to = voice->filter;
        float left = tap(voice, to.left, to.delay > 0 ? to.delay : 0);
        float right = tap(voice, to.right, to.delay < 0 ? -to.delay : 0);
        /* The APU steps a new filter's taps in over 32 samples (one frame). */
        if (from.left != to.left || from.right != to.right || from.delay != to.delay) {
            const float mix = (float)++voice->fade / FADE_STEPS;
            left = left * mix + tap(voice, from.left, from.delay > 0 ? from.delay : 0) * (1.0f - mix);
            right = right * mix + tap(voice, from.right, from.delay < 0 ? -from.delay : 0) * (1.0f - mix);
            if (voice->fade == FADE_STEPS) voice->rendered = to;
        }
        if (voice->rendered.left == to.left && voice->rendered.right == to.right &&
            voice->rendered.delay == to.delay) voice->fade = 0u;
        recomp_xtalk_process(&voice->xtalk, &left, &right);
        out[3u * written] = left / 32768.0f;
        out[3u * written + 1u] = right / 32768.0f;
        out[3u * written + 2u] = voice->history[voice->head] / 32768.0f;
        ++written;
    }
    while (consumed < input_frames) push_source(voice, input[consumed++]), voice->phase -= 1.0;
    return written;
}
