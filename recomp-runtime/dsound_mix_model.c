/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_mix_model.h"
#include <math.h>
#include <string.h>

void recomp_dsound_mix_default(RecompDsoundMixModel *model)
{
    *model = (RecompDsoundMixModel){.count = 2u, .bins = {0u, 1u}};
}

bool recomp_dsound_mix_set(RecompDsoundMixModel *model, const RecompDsoundMixPair *pairs,
    uint32_t count, bool replace)
{
    if (!model || count > 8u || (count && !pairs)) return false;
    RecompDsoundMixModel next = *model;
    if (replace) next.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (pairs[i].bin >= 32u || pairs[i].volume < -10000 || pairs[i].volume > 0) return false;
        for (uint32_t j = 0; j < i; ++j) if (pairs[j].bin == pairs[i].bin) return false;
        if (replace) next.bins[i] = pairs[i].bin;
        next.volumes[pairs[i].bin] = pairs[i].volume;
    }
    *model = next;
    return true;
}

bool recomp_dsound_mix_matrices(const RecompDsoundMixModel *model, uint32_t channels,
    uint32_t speakers, const uint32_t headroom[32], float dry[18], float wet[6])
{
    if (!model || model->count > 8u || channels < 1u || channels > 3u ||
        (speakers != 2u && speakers != 6u)) return false;
    memset(dry, 0, 18u * sizeof *dry);
    memset(wet, 0, 6u * sizeof *wet);
    for (uint32_t i = 0; i < model->count; ++i) {
        uint32_t bin = model->bins[i];
        /* Three inputs are a 3D voice: routes 0-1 carry the left HRTF ear, 2-3
           the right, later routes the unfiltered voice. 5849's center volume
           (0x366AE3) pans a frontal source -3 dB into center and -6 dB into the
           ears, which only holds if center carries the source itself. */
        const uint32_t voice = i < 4u ? i / 2u : 2u;
        if (bin >= 32u) return false;
        /* A fully attenuated send is silent wherever it is routed. */
        if (model->volumes[bin] == -10000) continue;
        /* FX sends need their own native signal services. */
        if (bin > 10u) return false;
        float gain = powf(10.0f, model->volumes[bin] / 2000.0f);
        gain = ldexpf(gain, -(int)(headroom[bin] & 7u));
        if (bin == 10u) {
            if (channels == 3u) {
                wet[voice] += gain;
                wet[3u + voice] += gain;
            } else {
                wet[0] += gain;
                wet[channels == 1u ? 1u : 3u] += gain;
            }
            continue;
        }
        /* Stereo sources feed alternating slots, except paired speaker bins
           (0/1 and XTLK 6-9), which take the channel of their side. */
        const uint32_t in = channels == 3u ? voice : channels == 1u ? 0u
            : bin <= 1u || bin >= 6u ? bin & 1u : i % 2u;
        if (bin >= 6u) {
            /* XTLK front/back left (6/8) and right (7/9) carry the HRTF ear of
               their side. xtalkamp.bin effects 5-8 double each XTLK bin and
               effect 14 (run in dsound_hrtf.c) cancels crosstalk into front
               (6/7) and rear (8/9) speakers; stereo folds rear at -3 dB. */
            const uint32_t side = bin & 1u;
            const bool rear = bin >= 8u;
            dry[(speakers == 6u && rear ? 4u + side : side) * channels + in] +=
                gain * (speakers == 2u && rear ? 1.41421356f : 2.0f);
        } else if (speakers == 6u || bin <= 1u) {
            /* Bins 0-5 are FL, FR, C, LFE, BL, BR: the host's 5.1 order. */
            dry[bin * channels + in] += gain;
        } else if (bin != 3u) {
            /* DSOUND's stereo EP program (0x01067d58) folds center and rear at
               -3 dB and drops LFE. */
            if (bin != 5u) dry[in] += gain * 0.70710678f;
            if (bin != 4u) dry[channels + in] += gain * 0.70710678f;
        }
    }
    return true;
}
