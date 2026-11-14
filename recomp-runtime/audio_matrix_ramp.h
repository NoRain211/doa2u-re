// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RECOMP_AUDIO_MATRIX_RAMP_H
#define RECOMP_AUDIO_MATRIX_RAMP_H

#include <algorithm>
#include <cstdint>
#include <cstring>

/* An output matrix ([source + destination * channels], as XAudio2) that eases
   linearly to each new target over one quantum, however the audio is split
   into chunks, as XAudio2 eases volume and matrix changes; a step clicks.
   Zero-initialized, it takes its first target at once. */
struct RecompMatrixRamp {
    static constexpr uint32_t kEntries = 3 * 6;
    float from[kEntries], to[kEntries];
    uint32_t elapsed, length;  // frames into the ramp, and its length
    bool ready;

    /* target holds kEntries values. A change restarts the ramp from wherever
       the previous one had got to. */
    void retarget(const float *target, uint32_t quantum)
    {
        if (ready && std::memcmp(to, target, sizeof to) == 0) return;
        for (uint32_t i = 0; i < kEntries; ++i) from[i] = ready ? at(i, elapsed) : target[i];
        std::memcpy(to, target, sizeof to);
        length = quantum;
        elapsed = ready ? 0u : quantum;
        ready = true;
    }

    /* Entry i after `done` frames of the ramp. */
    float at(uint32_t i, uint32_t done) const
    {
        return done >= length ? to[i] : from[i] + (to[i] - from[i]) * done / length;
    }

    bool easing(uint32_t frame) const { return elapsed + frame < length; }
    /* Entry i at a frame of the chunk the ramp is now at. */
    float atFrame(uint32_t i, uint32_t frame) const { return at(i, elapsed + frame + 1u); }
    void advance(uint32_t frames) { elapsed = std::min(elapsed + frames, length); }
};

#endif
