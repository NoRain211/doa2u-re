/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_DSOUND_MIX_MODEL_H
#define DOA2U_RECOMP_DSOUND_MIX_MODEL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct RecompDsoundMixPair { uint32_t bin; int32_t volume; } RecompDsoundMixPair;
typedef struct RecompDsoundMixModel {
    uint32_t count, bins[8];
    int32_t volumes[32];
} RecompDsoundMixModel;
void recomp_dsound_mix_default(RecompDsoundMixModel *model);
bool recomp_dsound_mix_set(RecompDsoundMixModel *model, const RecompDsoundMixPair *pairs,
    uint32_t count, bool replace);
/* speakers is 2 (stereo fold) or 6 (5.1, FL FR C LFE BL BR). dry holds
   channels * speakers entries, wet channels * 2, as [source + destination * channels]. */
bool recomp_dsound_mix_matrices(const RecompDsoundMixModel *model, uint32_t channels,
    uint32_t speakers, const uint32_t headroom[32], float dry[18], float wet[6]);
#endif
