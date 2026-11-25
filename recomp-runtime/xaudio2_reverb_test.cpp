/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Checks the llvm-mingw reverb declarations against the Windows SDK. */
#include "xaudio2_reverb.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static_assert(sizeof(recomp::ReverbParameters) == sizeof(XAUDIO2FX_REVERB_PARAMETERS), "layout");
static_assert(sizeof(recomp::ReverbI3dl2Parameters) == sizeof(XAUDIO2FX_REVERB_I3DL2_PARAMETERS), "layout");

int main()
{
    std::vector<XAUDIO2FX_REVERB_I3DL2_PARAMETERS> inputs = {
        XAUDIO2FX_I3DL2_PRESET_DEFAULT, XAUDIO2FX_I3DL2_PRESET_GENERIC,
        XAUDIO2FX_I3DL2_PRESET_PADDEDCELL, XAUDIO2FX_I3DL2_PRESET_ROOM,
        XAUDIO2FX_I3DL2_PRESET_BATHROOM, XAUDIO2FX_I3DL2_PRESET_LIVINGROOM,
        XAUDIO2FX_I3DL2_PRESET_STONEROOM, XAUDIO2FX_I3DL2_PRESET_AUDITORIUM,
        XAUDIO2FX_I3DL2_PRESET_CONCERTHALL, XAUDIO2FX_I3DL2_PRESET_CAVE,
        XAUDIO2FX_I3DL2_PRESET_ARENA, XAUDIO2FX_I3DL2_PRESET_HANGAR,
        XAUDIO2FX_I3DL2_PRESET_CARPETEDHALLWAY, XAUDIO2FX_I3DL2_PRESET_HALLWAY,
        XAUDIO2FX_I3DL2_PRESET_STONECORRIDOR, XAUDIO2FX_I3DL2_PRESET_ALLEY,
        XAUDIO2FX_I3DL2_PRESET_FOREST, XAUDIO2FX_I3DL2_PRESET_CITY,
        XAUDIO2FX_I3DL2_PRESET_MOUNTAINS, XAUDIO2FX_I3DL2_PRESET_QUARRY,
        XAUDIO2FX_I3DL2_PRESET_PLAIN, XAUDIO2FX_I3DL2_PRESET_PARKINGLOT,
        XAUDIO2FX_I3DL2_PRESET_SEWERPIPE, XAUDIO2FX_I3DL2_PRESET_UNDERWATER,
        XAUDIO2FX_I3DL2_PRESET_SMALLROOM, XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM,
        XAUDIO2FX_I3DL2_PRESET_LARGEROOM, XAUDIO2FX_I3DL2_PRESET_MEDIUMHALL,
        XAUDIO2FX_I3DL2_PRESET_LARGEHALL, XAUDIO2FX_I3DL2_PRESET_PLATE,
        // The runtime's accepted limits, including both delay clamps.
        {0.0f, -10000, -10000, 0.0f, 0.1f, 0.1f, -10000, 0.0f, -10000, 0.0f, 0.0f, 0.0f, 20.0f},
        {100.0f, 0, 0, 10.0f, 20.0f, 2.0f, 1000, 0.3f, 2000, 0.1f, 100.0f, 100.0f, 20000.0f},
        {100.0f, -1000, -100, 0.0f, 1.49f, 1.0f, -2602, 0.0005f, 200, 0.085f, 100.0f, 100.0f, 5000.0f},
    };
    std::mt19937 random(0x0d0a2u);
    auto uniform = [&](float low, float high) { return std::uniform_real_distribution<float>(low, high)(random); };
    auto integer = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); };
    for (int i = 0; i < 200000; ++i)
        inputs.push_back({uniform(0.0f, 100.0f), integer(-10000, 0), integer(-10000, 0),
            uniform(0.0f, 10.0f), uniform(0.1f, 20.0f), uniform(0.1f, 2.0f),
            integer(-10000, 1000), uniform(0.0f, 0.3f), integer(-10000, 2000),
            uniform(0.0f, 0.1f), uniform(0.0f, 100.0f), uniform(0.0f, 100.0f),
            uniform(20.0f, 20000.0f)});
    size_t failures = 0;
    for (const auto &input : inputs) {
        for (BOOL seven_dot_one : {FALSE, TRUE}) {
            XAUDIO2FX_REVERB_PARAMETERS expected;
            recomp::ReverbParameters actual;
            std::memset(static_cast<void *>(&expected), 0xcd, sizeof expected);
            std::memset(static_cast<void *>(&actual), 0xcd, sizeof actual);
            recomp::ReverbI3dl2Parameters copy;
            std::memcpy(&copy, &input, sizeof copy);
            ReverbConvertI3DL2ToNative(&input, &expected, seven_dot_one);
            recomp::reverb_i3dl2_to_native(&copy, &actual, seven_dot_one);
            if (std::memcmp(&expected, &actual, sizeof actual) != 0 && failures++ < 5)
                std::fprintf(stderr, "reverb mismatch room=%d decay_hf=%.9g seven=%d\n",
                    int(input.Room), double(input.DecayHFRatio), int(seven_dot_one));
        }
    }
    std::printf("reverb conversions=%zu mismatches=%zu\n", inputs.size() * 2, failures);
    return failures != 0;
}
