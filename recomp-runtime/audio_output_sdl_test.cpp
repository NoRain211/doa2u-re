// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_matrix_ramp.h"
#include "audio_output.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" void recomp_audio_output_test_reset(void);
extern "C" void recomp_audio_output_test_reverb_postmix(int channels, int freq, float *buffer, int bytes);

static void check_fail(const char *expr, const char *file, int line)
{
    std::fprintf(stderr, "Check failed at %s:%d: %s\n", file, line, expr);
    std::abort();
}
#define CHECK(expr) do { if (!(expr)) check_fail(#expr, __FILE__, __LINE__); } while (0)

int main()
{
    // The dummy driver needs no audio device, so initialization must succeed.
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);

    // Initially disabled before init
    CHECK(recomp_audio_output_enabled() == 0);
    // The speaker count reads the environment only.
    setenv("RECOMP_AUDIO_SURROUND", "0", 1);
    CHECK(recomp_audio_output_speakers() == 2);
    setenv("RECOMP_AUDIO_SURROUND", "1", 1);
    CHECK(recomp_audio_output_speakers() == 6);
    unsetenv("RECOMP_AUDIO_SURROUND");

    // Test with valid gain first so real SDL audio initialization is exercised
    setenv("RECOMP_AUDIO_GAIN", "0.5", 1);
    recomp_audio_output_initialize();
    CHECK(recomp_audio_output_enabled() != 0);

    // Test submitting audio
    std::vector<int16_t> pcm_stereo_16(480 * 2, 0);
    for (size_t i = 0; i < pcm_stereo_16.size(); ++i) {
        pcm_stereo_16[i] = static_cast<int16_t>(1000 * std::sin(i * 0.1));
    }

    // Submit stereo 48000 Hz 16-bit
    recomp_audio_output_submit(
        0, reinterpret_cast<const uint8_t *>(pcm_stereo_16.data()),
        static_cast<uint32_t>(pcm_stereo_16.size() * sizeof(int16_t)),
        48000, 2, 16, 0);

    // Submit mono 22050 Hz 8-bit
    std::vector<uint8_t> pcm_mono_8(220, 128);
    recomp_audio_output_submit(
        1, pcm_mono_8.data(), static_cast<uint32_t>(pcm_mono_8.size()),
        22050, 1, 8, -600);

    // Submit stereo 48000 Hz 32-bit float, as the HRTF mixer does
    std::vector<float> pcm_float(480 * 2, 0.25f);
    recomp_audio_output_submit(
        2, reinterpret_cast<const uint8_t *>(pcm_float.data()),
        static_cast<uint32_t>(pcm_float.size() * sizeof(float)),
        48000, 2, 32, 0);

    // Test invalid submissions (should be safely dropped)
    recomp_audio_output_submit(999, pcm_mono_8.data(), 10, 44100, 1, 8, 0); // invalid slot
    recomp_audio_output_submit(0, nullptr, 100, 44100, 1, 8, 0); // null pcm
    recomp_audio_output_submit(0, pcm_mono_8.data(), 3, 44100, 2, 16, 0); // unaligned block

    // Test reset voice
    const float stereo_dry[] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float silent_wet[] = {0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(recomp_audio_output_set_voice_routing(1, 2, stereo_dry, silent_wet));
    recomp_audio_output_reset_voice(0);
    recomp_audio_output_reset_voice(1);
    recomp_audio_output_reset_voice(255);

    // Shutdown
    recomp_audio_output_shutdown();
    CHECK(recomp_audio_output_enabled() == 0);

    // A 5.1 master hands the stereo reverb device six channels; its return
    // must still reach FL/FR (issue #145 regression: it was silenced).
    recomp_audio_output_test_reset();
    setenv("RECOMP_AUDIO_GAIN", "1", 1);
    setenv("RECOMP_AUDIO_SURROUND", "1", 1);
    recomp_audio_output_initialize();
    CHECK(recomp_audio_output_enabled() != 0);
    const RecompAudioReverb hall = {-1000, -100, 0.0f, 1.49f, 0.83f, -2602, 0.007f,
        200, 0.011f, 100.0f, 100.0f, 5000.0f};
    CHECK(recomp_audio_output_set_reverb(&hall));
    std::vector<float> mix(480 * 6, 0.0f);
    mix[0] = mix[1] = 1.0f;  // an impulse in FL/FR, where SDL upmixes the wet pair
    bool front = false, other = false;
    for (int block = 0; block < 20; ++block) {
        recomp_audio_output_test_reverb_postmix(6, 48000, mix.data(),
            static_cast<int>(mix.size() * sizeof(float)));
        for (size_t i = 0; i < mix.size(); ++i) {
            (i % 6 < 2 ? front : other) |= mix[i] != 0.0f;
        }
        std::fill(mix.begin(), mix.end(), 0.0f);
    }
    CHECK(front);
    CHECK(!other);
    recomp_audio_output_shutdown();
    unsetenv("RECOMP_AUDIO_SURROUND");

    // A voice's volume change eases in like XAudio2's instead of stepping,
    // which clicked on macOS and Linux (issue #145). Slot 4 is routed; slot
    // 257, like the hit voices, takes the default matrix (issue #212).
    for (const uint32_t slot : {4u, 257u}) {
        recomp_audio_output_test_reset();
        char capture[] = "/tmp/recomp_gain_ramp_XXXXXX";
        const int capture_fd = mkstemp(capture);
        CHECK(capture_fd >= 0);
        close(capture_fd);
        unlink(capture);  // the capture never overwrites a file
        setenv("RECOMP_AUDIO_CAPTURE", capture, 1);
        recomp_audio_output_initialize();
        CHECK(recomp_audio_output_enabled() != 0);
        const float front_dry[] = {1.0f, 0.0f, 0.0f, 1.0f};
        const float no_wet[] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (slot == 4u) CHECK(recomp_audio_output_set_voice_routing(slot, 2, front_dry, no_wet));
        std::vector<float> level(4800 * 2, 0.5f);  // 100 ms of DC
        const uint32_t level_bytes = static_cast<uint32_t>(level.size() * sizeof(float));
        recomp_audio_output_submit(slot, reinterpret_cast<const uint8_t *>(level.data()), level_bytes, 48000, 2, 32, 0);
        // Change the volume while the first chunk plays, as the game does, in
        // 2 ms chunks like game-thread pumps: each must continue the ramp.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        for (uint32_t done = 0; done < level_bytes; done += 96u * 2u * sizeof(float)) {
            recomp_audio_output_submit(slot, reinterpret_cast<const uint8_t *>(level.data()) + done,
                96u * 2u * sizeof(float), 48000, 2, 32, -600);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        recomp_audio_output_shutdown();
        unsetenv("RECOMP_AUDIO_CAPTURE");
        FILE *wav = std::fopen(capture, "rb");
        CHECK(wav != nullptr);
        std::fseek(wav, 44, SEEK_SET);
        // Past the resampler's ringing on the step up from silence, the level
        // must glide from 0.5 to -6 dB; the unramped gain jumped there.
        float frame[2], previous = 0.0f, largest = 0.0f;
        int sounding = 0;
        while (std::fread(frame, sizeof frame, 1, wav) == 1) {
            sounding = frame[0] > 0.1f ? sounding + 1 : 0;
            if (sounding > 64) largest = std::max(largest, std::fabs(frame[0] - previous));
            previous = frame[0];
        }
        std::fclose(wav);
        unlink(capture);
        CHECK(largest > 0.0f && largest < 0.01f);
    }

    // The ramp eases linearly over its quantum however the chunks split it,
    // and a change mid-ramp restarts from the level it had reached.
    RecompMatrixRamp ramp{};
    float loud[RecompMatrixRamp::kEntries] = {1.0f}, quiet[RecompMatrixRamp::kEntries] = {0.5f};
    ramp.retarget(loud, 480u);
    CHECK(!ramp.easing(0u) && ramp.atFrame(0u, 0u) == 1.0f);  // the first target applies at once
    ramp.retarget(quiet, 480u);
    for (uint32_t chunk = 0; chunk < 5u; ++chunk, ramp.advance(96u)) {
        for (uint32_t f = 0; f < 96u; ++f) {
            const float expected = 1.0f - 0.5f * static_cast<float>(chunk * 96u + f + 1u) / 480.0f;
            CHECK(std::fabs(ramp.atFrame(0u, f) - expected) < 1e-6f);
        }
    }
    CHECK(!ramp.easing(0u) && ramp.atFrame(0u, 0u) == 0.5f);
    ramp.retarget(loud, 480u);
    ramp.advance(240u);  // halfway: 0.75
    ramp.retarget(quiet, 480u);
    CHECK(std::fabs(ramp.atFrame(0u, 0u) - (0.75f - 0.25f / 480.0f)) < 1e-6f);

    // Reset attempted flag for muted lifecycle test
    recomp_audio_output_test_reset();

    // Initialize with 0 gain -> must stay disabled / muted
    setenv("RECOMP_AUDIO_GAIN", "0", 1);
    recomp_audio_output_initialize();
    CHECK(recomp_audio_output_enabled() == 0);

    recomp_audio_output_shutdown();

    std::printf("audio output SDL test passed\n");
    return 0;
}
