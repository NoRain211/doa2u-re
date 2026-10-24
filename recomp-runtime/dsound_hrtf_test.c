#include "dsound_hrtf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL dsound hrtf: %s\n", what);
        exit(1);
    }
}

int main(void)
{
    static uint8_t bank[2222 * 32], map[61 * 31 * 2];
    float azimuth, elevation;

    recomp_hrtf_angles(1.0f, 0.0f, 0.0f, &azimuth, &elevation);
    check(azimuth == 90.0f && elevation == 0.0f, "right");
    recomp_hrtf_angles(-1.0f, 0.0f, 1.0f, &azimuth, &elevation);
    check(azimuth == -45.0f, "front left diagonal");
    recomp_hrtf_angles(0.0f, 0.0f, -1.0f, &azimuth, &elevation);
    check(azimuth == 180.0f, "behind");
    recomp_hrtf_angles(0.0f, -1.0f, 0.0f, &azimuth, &elevation);
    check(azimuth == 0.0f && elevation == -90.0f, "below");

    /* Azimuth 30 at elevation 0 is map row 10, column 15; point it at pair 4. */
    map[2 * (10 * 31 + 15)] = 8;
    bank[8 * 32] = 0x40;     /* ear0 tap 0: exponent 4, mantissa 0 = 1/16 */
    bank[8 * 32 + 31] = 3;   /* ear0 delay byte */
    bank[9 * 32 + 2] = 0x20; /* ear1 tap 2: exponent 2, mantissa 0 = 1/64 */
    RecompHrtfFilter right = recomp_hrtf_lookup(bank, map, 30.0f, 0.0f, false);
    check(right.left == (const int8_t *)bank + 8 * 32 && right.delay == 3, "right-side source delays left ear");
    RecompHrtfFilter left = recomp_hrtf_lookup(bank, map, -31.0f, 0.0f, false);
    check(left.left == right.right && left.right == right.left && left.delay == -3, "left side mirrors ears");
    /* Surround folds azimuth 150 onto the front response at 30. */
    check(recomp_hrtf_lookup(bank, map, 150.0f, 0.0f, true).left == right.left, "surround folds rear");
    check(recomp_hrtf_lookup(bank, map, 150.0f, 0.0f, false).left != right.left, "stereo keeps rear");
    /* 5849 truncates 1.5 + 1.5 to 3 (row 1); 3936's rounding chose row 0. */
    map[2 * (1 * 31 + 15)] = 10;
    check(recomp_hrtf_lookup(bank, map, 1.5f, 0.0f, false).left == (const int8_t *)bank + 10 * 32,
        "integer quantization ties truncate");

    /* A 48 kHz impulse comes out as each ear's taps, the far ear delayed. */
    RecompHrtfVoice voice;
    memset(&voice, 0, sizeof voice);
    voice.filter = voice.rendered = right;
    int16_t input[24] = {0, 27100};
    float out[72];
    check(recomp_hrtf_render(&voice, input, 24, 48000, out, 24) == 24, "frame count");
    /* Output n is input n-5 (resampler latency). Taps apply unnormalized. */
    check(out[3 * 8] == 0.0f && fabsf(out[3 * 9] - 27100.0f / 16.0f / 32768.0f) < 0.000001f,
        "left: tap 0 after 3-sample delay");
    check(fabsf(out[3 * 8 + 1] - 27100.0f / 64.0f / 32768.0f) < 0.000001f, "right: tap 2, undelayed");
    check(fabsf(out[3 * 6 + 2] - 27100.0f / 32768.0f) < 0.000001f && out[3 * 7 + 2] == 0.0f &&
        out[3 * 5 + 2] == 0.0f,
        "third channel is the unfiltered voice");

    /* 24 kHz input doubles the frame count. */
    memset(&voice, 0, sizeof voice);
    voice.filter = voice.rendered = right;
    check(recomp_hrtf_render(&voice, input, 8, 24000, out, 8) == 8, "upsampled pieces stop at capacity");

    /* 8-point Lagrange keeps a 16 kHz voice's top octave: a 6 kHz tone loses
       1.3 dB (linear interpolation lost 4). */
    {
        static int16_t tone[1600];
        static float wide[3 * 4800];
        for (int i = 0; i < 1600; ++i) tone[i] = (int16_t)(16000.0 * sin(2.0 * 3.14159265358979 * 6000.0 * i / 16000.0));
        memset(&voice, 0, sizeof voice);
        voice.filter = voice.rendered = right;
        check(recomp_hrtf_render(&voice, tone, 1600, 16000, wide, 4800) >= 4700, "tone frames");
        double energy = 0.0;
        for (int i = 1000; i < 4000; ++i) energy += (double)wide[3 * i + 2] * wide[3 * i + 2];
        const double rms = sqrt(energy / 3000.0), expected = 16000.0 / 32768.0 / sqrt(2.0);
        check(fabs(20.0 * log10(rms / expected)) < 2.0, "6 kHz tone keeps its level");
    }

    /* 0x7F is the largest tap (31/32); bit 7 is a sign, not two's complement. */
    const int8_t largest[32] = {0x7F};
    const int8_t negative[32] = {(int8_t)0xFF};
    memset(&voice, 0, sizeof voice);
    voice.filter = voice.rendered = (RecompHrtfFilter){largest, negative, 0};
    for (int i = 0; i < 24; ++i) input[i] = 32767;
    check(recomp_hrtf_render(&voice, input, 24, 48000, out, 24) == 24 &&
        fabsf(out[33] - 31.0f / 32.0f * 32767.0f / 32768.0f) < 0.000001f &&
        fabsf(out[34] + 31.0f / 32.0f * 32767.0f / 32768.0f) < 0.000001f, "taps are sign-magnitude floats");

    /* A filter change steps in over 32 output samples, even across calls. */
    memset(&voice, 0, sizeof voice);
    voice.rendered = (RecompHrtfFilter){largest, NULL, 0};
    voice.filter = (RecompHrtfFilter){negative, NULL, 0};
    check(recomp_hrtf_render(&voice, input, 16, 48000, out, 16) == 16 && voice.fade == 16u &&
        voice.rendered.left == largest, "fade is halfway after 16 samples");
    check(recomp_hrtf_render(&voice, input, 16, 48000, out, 16) == 16 && voice.fade == 0u &&
        voice.rendered.left == negative, "fade completes at 32 samples");

    /* xtalkamp effect 14: a left-ear impulse reaches the right ear 11 samples
       later scaled by the feed, then decays by the pole. */
    const float pole = (float)0x7645a1 / 0x800000, feed = (float)(0xf88313 - 0x1000000) / 0x800000;
    int16_t impulse[24] = {0, 27100};
    float ringing[72];
    memset(&voice, 0, sizeof voice);
    voice.filter = voice.rendered = (RecompHrtfFilter){largest, NULL, 0};
    check(recomp_hrtf_render(&voice, impulse, 24, 48000, ringing, 24) == 24, "canceller frames");
    const float direct = 31.0f / 32.0f * 27100.0f / 32768.0f;
    check(fabsf(ringing[3 * 6] - direct) < 0.000001f && ringing[3 * 16 + 1] == 0.0f &&
        fabsf(ringing[3 * 17 + 1] - feed * direct) < 0.000001f &&
        fabsf(ringing[3 * 18 + 1] - pole * feed * direct) < 0.000001f, "crosstalk canceller");
    puts("PASS dsound hrtf angles, lookup, mirroring, delay, render");
    return 0;
}
