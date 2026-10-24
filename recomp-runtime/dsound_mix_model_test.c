/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_mix_model.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL mix line %d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    RecompDsoundMixModel model, prior;
    uint32_t headroom[32] = {0};
    float dry[18], wet[6];
    recomp_dsound_mix_default(&model);
    CHECK(recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet));
    CHECK(dry[0] == 1 && dry[1] == 0 && dry[2] == 0 && dry[3] == 1 && wet[0] == 0);
    const RecompDsoundMixPair speakers[6] = {{0,0},{1,0},{4,-10000},{5,-10000},{3,-10000},{2,-10000}};
    CHECK(recomp_dsound_mix_set(&model, speakers, 6, true));
    CHECK(recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet) && dry[0]==1 && dry[3]==1);
    /* DOA2's speaker panner: rear at -3.09 dB, LFE and center at -13.97 dB. */
    const RecompDsoundMixPair panned[6] = {{0,0},{1,0},{4,-309},{5,-309},{3,-1397},{2,-1397}};
    const float rear = powf(10.0f, -309 / 2000.0f) * .70710678f;
    const float center = powf(10.0f, -1397 / 2000.0f) * .70710678f;
    CHECK(recomp_dsound_mix_set(&model, panned, 6, true));
    CHECK(recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet));
    CHECK(fabsf(dry[0] - (1 + rear)) < .00001f && fabsf(dry[1] - center) < .00001f &&
        dry[2] == 0 && fabsf(dry[3] - (1 + rear + center)) < .00001f && wet[0] == 0);
    CHECK(recomp_dsound_mix_matrices(&model, 1, 2, headroom, dry, wet));
    CHECK(fabsf(dry[0] - (1 + rear + center)) < .00001f && fabsf(dry[1] - dry[0]) < .00001f);
    /* 5849's 3D order: XTLK front/back left then right; each bin keeps its ear. */
    const RecompDsoundMixPair crosstalk[5] = {{6,0},{8,-2000},{7,0},{9,0},{30,-10000}};
    CHECK(recomp_dsound_mix_set(&model, crosstalk + 4, 1, true));
    CHECK(recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet) && dry[0] == 0);
    CHECK(recomp_dsound_mix_set(&model, crosstalk, 5, true));
    CHECK(recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet));
    /* xtalkamp doubles XTLK bins; stereo folds the rear pair at -3 dB. */
    CHECK(fabsf(dry[0] - 2.14142136f) < .00001f && dry[1] == 0 && dry[2] == 0 &&
        fabsf(dry[3] - 3.41421356f) < .00001f && wet[0] == 0);
    CHECK(recomp_dsound_mix_matrices(&model, 1, 2, headroom, dry, wet));
    CHECK(fabsf(dry[0] - 2.14142136f) < .00001f && fabsf(dry[1] - 3.41421356f) < .00001f);
    /* A 3D voice: routes 0-3 take the ears, center and I3DL2 the unfiltered voice. */
    const RecompDsoundMixPair voiced[6] = {{6,0},{8,0},{7,0},{9,0},{2,0},{10,0}};
    CHECK(recomp_dsound_mix_set(&model, voiced, 6, true));
    CHECK(recomp_dsound_mix_matrices(&model, 3, 2, headroom, dry, wet));
    CHECK(fabsf(dry[0] - 3.41421356f) < .00001f && dry[1] == 0 && fabsf(dry[2] - .70710678f) < .00001f &&
        dry[3] == 0 && fabsf(dry[4] - 3.41421356f) < .00001f && fabsf(dry[5] - .70710678f) < .00001f &&
        wet[0] == 0 && wet[1] == 0 && wet[2] == 1 && wet[5] == 1);
    /* 5.1: XTLK pairs land on front and back speakers, center on center. */
    CHECK(recomp_dsound_mix_matrices(&model, 3, 6, headroom, dry, wet));
    CHECK(dry[0] == 2 && dry[12] == 2 && dry[4] == 2 && dry[16] == 2 && dry[8] == 1 &&
        dry[1] == 0 && dry[2] == 0 && dry[3] == 0 && wet[2] == 1 && wet[5] == 1);
    CHECK(recomp_dsound_mix_set(&model, panned, 6, true));
    CHECK(recomp_dsound_mix_matrices(&model, 1, 6, headroom, dry, wet));
    CHECK(dry[0] == 1 && dry[1] == 1 && fabsf(dry[4] - powf(10.0f, -309 / 2000.0f)) < .00001f &&
        fabsf(dry[3] - powf(10.0f, -1397 / 2000.0f)) < .00001f && dry[6] == 0);
    CHECK(!recomp_dsound_mix_matrices(&model, 1, 4, headroom, dry, wet));
    RecompDsoundMixPair pairs[3] = {{0,-2000},{1,0},{10,0}};
    CHECK(recomp_dsound_mix_set(&model, pairs, 3, true));
    headroom[10] = 1;
    CHECK(recomp_dsound_mix_matrices(&model, 1, 2, headroom, dry, wet));
    CHECK(fabsf(dry[0] - .1f) < .00001f && dry[1] == 1 && wet[0] == .5f && wet[1] == .5f);
    pairs[0] = (RecompDsoundMixPair){30,-10000};
    CHECK(recomp_dsound_mix_set(&model, pairs, 1, false) && model.count == 3 && model.volumes[30] == -10000);
    prior = model;
    pairs[0].volume = 1;
    CHECK(!recomp_dsound_mix_set(&model, pairs, 1, false) && memcmp(&prior,&model,sizeof model)==0);
    pairs[0] = (RecompDsoundMixPair){11,0};
    CHECK(recomp_dsound_mix_set(&model, pairs, 1, true));
    CHECK(!recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet));
    CHECK(recomp_dsound_mix_set(&model, NULL, 0, true));
    CHECK(recomp_dsound_mix_matrices(&model, 2, 2, headroom, dry, wet) && dry[0]==0 && wet[0]==0);
    return 0;
}
