// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_upscale.h"
#include "xbrz.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures;
static void expect(const char *what, bool ok)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main()
{
    expect("factor 4", recomp_texture_upscale_factor(256u, 128u) == 4u && recomp_texture_upscale_factor(16u, 16u) == 4u);
    expect("factor 2", recomp_texture_upscale_factor(512u, 64u) == 2u);
    expect("factor 1", recomp_texture_upscale_factor(1024u, 32u) == 1u && recomp_texture_upscale_factor(0u, 4u) == 1u);

    std::vector<uint8_t> out;
    const uint8_t pixel[4] = {1, 2, 3, 4};
    expect("bad factor", !recomp_texture_upscale(pixel, 1u, 1u, 1u, out) && !recomp_texture_upscale(pixel, 1u, 1u, 7u, out));

    /* A flat colour stays that colour, clear stays clear, at 4x the size. */
    std::vector<uint8_t> flat(32u * 16u * 4u);
    for (size_t i = 0; i < flat.size(); i += 4u) {
        const bool clear = (i / 4u) % 32u >= 16u;
        flat[i] = 0x20; flat[i + 1] = 0x40; flat[i + 2] = 0xc0; flat[i + 3] = clear ? 0x00 : 0xff;
        if (clear) flat[i] = flat[i + 1] = flat[i + 2] = 0;
    }
    expect("flat", recomp_texture_upscale(flat.data(), 32u, 16u, 4u, out) && out.size() == 128u * 64u * 4u);
    bool kept = true;
    for (uint32_t y = 0; y < 64u; ++y)
        for (uint32_t x = 0; x < 128u; ++x) {
            const uint8_t *p = &out[(y * 128u + x) * 4u];
            if (x < 56u && !(p[0] == 0x20 && p[1] == 0x40 && p[2] == 0xc0 && p[3] == 0xff)) kept = false;
            if (x >= 72u && p[3] != 0u) kept = false;
        }
    expect("flat kept", kept);

    /* Slices on several threads give the single pass's picture exactly. */
    const uint32_t w = 61u, h = 97u;
    std::vector<uint8_t> noise(w * h * 4u);
    uint32_t seed = 12345u;
    for (uint8_t &b : noise) { seed = seed * 1103515245u + 12345u; b = static_cast<uint8_t>(seed >> 24); }
    for (size_t i = 0; i < noise.size(); i += 4u) noise[i + 3] = noise[i + 3] < 64u ? 0u : 0xffu;
    std::vector<uint32_t> source(w * h), single(w * h * 9u);
    std::memcpy(source.data(), noise.data(), noise.size());
    xbrz::scale(3u, source.data(), single.data(), static_cast<int>(w), static_cast<int>(h), xbrz::ColorFormat::ARGB);
    expect("threads", recomp_texture_upscale(noise.data(), w, h, 3u, out) && out.size() == single.size() * 4u &&
        std::memcmp(out.data(), single.data(), out.size()) == 0);

    if (failures == 0) std::printf("texture upscale: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
