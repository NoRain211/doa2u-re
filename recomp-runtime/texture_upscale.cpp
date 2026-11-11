// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_upscale.h"

#include "xbrz.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>

uint32_t recomp_texture_upscale_factor(uint32_t width, uint32_t height)
{
    static const bool enabled = [] {
        const char *setting = std::getenv("RECOMP_UI_UPSCALE");
        return setting == nullptr || std::strcmp(setting, "0") != 0;
    }();
    if (!enabled || width == 0u || height == 0u) return 1u;
    const uint32_t side = std::max(width, height);
    return side <= 256u ? 4u : side <= 512u ? 2u : 1u;
}

bool recomp_texture_upscale(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t factor,
    std::vector<uint8_t> &out)
{
    if (bgra == nullptr || factor < 2u || factor > 6u || width == 0u || height == 0u ||
        width > 4096u || height > 4096u) return false;
    /* Little-endian BGRA bytes are xBRZ's ARGB words. */
    std::vector<uint32_t> source(static_cast<size_t>(width) * height);
    std::memcpy(source.data(), bgra, source.size() * 4u);
    std::vector<uint32_t> target(source.size() * factor * factor);
    /* Slices of at least 16 rows, one per hardware thread (xBRZ allows any
       non-overlapping source rows per thread). */
    const unsigned threads = std::max(1u, std::min({std::thread::hardware_concurrency(), 8u, height / 16u}));
    const auto slice = [&](unsigned i) {
        const int first = static_cast<int>(static_cast<uint64_t>(height) * i / threads);
        const int last = static_cast<int>(static_cast<uint64_t>(height) * (i + 1u) / threads);
        xbrz::scale(factor, source.data(), target.data(), static_cast<int>(width), static_cast<int>(height),
            xbrz::ColorFormat::ARGB, xbrz::ScalerCfg(), first, last);
    };
    std::vector<std::thread> workers;
    try {
        for (unsigned i = 1u; i < threads; ++i) workers.emplace_back(slice, i);
    } catch (const std::system_error &) {
        // Fewer threads: the rest run here.
    }
    slice(0u);
    for (unsigned i = static_cast<unsigned>(workers.size()) + 1u; i < threads; ++i) slice(i);
    for (std::thread &worker : workers) worker.join();
    out.resize(target.size() * 4u);
    std::memcpy(out.data(), target.data(), out.size());
    return true;
}
