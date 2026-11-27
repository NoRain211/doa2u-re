// SPDX-License-Identifier: GPL-3.0-or-later
#include "playstation_buttons.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures;
static void expect(const char *what, bool ok)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static const uint64_t kFaces = 0x0123456789abcdefull;
static const uint64_t kShoulders = 0xfedcba9876543210ull;

/* BGRA texel at x, y. */
static void texel(const RecompTexturePackImage &image, uint32_t x, uint32_t y,
    uint8_t &blue, uint8_t &green, uint8_t &red, uint8_t &alpha)
{
    const size_t at = (static_cast<size_t>(y) * image.width + x) * 4u;
    blue = image.levels[0].bytes[at + 0u];
    green = image.levels[0].bytes[at + 1u];
    red = image.levels[0].bytes[at + 2u];
    alpha = image.levels[0].bytes[at + 3u];
}

int main()
{
    const fs::path root = fs::temp_directory_path() /
        fs::path("recomp_playstation_buttons_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    const fs::path folder = root / "textures";
    fs::create_directories(folder, error);

    RecompTexturePackImage image;
    uint8_t blue, green, red, alpha;
    /* The game's 64x32 prompts work with no file beside the launcher. */
    recomp_playstation_buttons_configure(folder.u8string());
    expect("inactive first", !recomp_playstation_buttons_active());
    expect("inactive", !recomp_playstation_buttons_image(kFaces, 64u, 32u, image));
    recomp_playstation_buttons_set_active(true);
    expect("active", recomp_playstation_buttons_active());
    expect("built-in faces", recomp_playstation_buttons_image(0x41b28f3648cd7b19ull, 64u, 32u, image));
    texel(image, 16u, 16u, blue, green, red, alpha);
    expect("built-in cross", alpha == 255u && red > 200u && green > 200u && blue > 200u);
    expect("built-in shoulders", recomp_playstation_buttons_image(0x6d9306247838ceb2ull, 64u, 32u, image));
    recomp_playstation_buttons_set_active(false);

    {
        std::ofstream file(folder / "playstation-buttons.txt", std::ios::binary);
        file << "# the game's A/B and X/Y icons\n"
             << "0123456789ABCDEF = CROSS, Circle\n"
             << "fedcba9876543210 = l1,r1\n"
             << "# a texture with one symbol, and lines that cannot be used\n"
             << "00000000000000ab = triangle\n"
             << "not a hex key = cross\n"
             << "00000000000000cd = moon\n"
             << "00000000000000ab = square\n";
    }

    recomp_playstation_buttons_configure(folder.u8string());
    expect("inactive first", !recomp_playstation_buttons_active());

    /* Without a PlayStation pad the game keeps its own icons. */
    expect("inactive", !recomp_playstation_buttons_image(kFaces, 64u, 32u, image));

    recomp_playstation_buttons_set_active(true);
    expect("active", recomp_playstation_buttons_active());
    expect("unknown key", !recomp_playstation_buttons_image(0x1111111111111111ull, 64u, 32u, image));
    expect("no key", !recomp_playstation_buttons_image(0u, 64u, 32u, image));

    /* A/B: Cross left, Circle right, at the guest texture's size. */
    expect("faces found", recomp_playstation_buttons_image(kFaces, 64u, 32u, image));
    expect("faces shape", image.format == RECOMP_TEXTURE_PACK_BGRA8 && image.width == 64u &&
        image.height == 32u && image.generate_mips && image.levels.size() == 1u &&
        image.levels[0].pitch == 64u * 4u && image.levels[0].bytes.size() == 64u * 32u * 4u);
    texel(image, 0u, 0u, blue, green, red, alpha);
    expect("corner clear", alpha == 0u);
    /* Both disc centres are opaque and shaded; Cross's bars meet in white. */
    texel(image, 16u, 16u, blue, green, red, alpha);
    expect("cross centre white", alpha == 255u && red > 200u && green > 200u && blue > 200u);
    /* Circle's centre is the red disc, not its white ring. */
    texel(image, 48u, 16u, blue, green, red, alpha);
    expect("circle centre red", alpha == 255u && red > 140u && green < 100u && blue < 100u);
    /* The ring itself is white, just over a third of the way out. */
    texel(image, 54u, 16u, blue, green, red, alpha);
    expect("circle ring white", alpha > 0u && red > 150u && green > 150u && blue > 150u);

    /* L1/R1: two grey discs with their labels. */
    expect("shoulders found", recomp_playstation_buttons_image(kShoulders, 64u, 32u, image));
    bool light = false, shaded = false;
    for (uint32_t y = 8u; y < 24u; ++y) {
        for (uint32_t x = 4u; x < 28u; ++x) {
            texel(image, x, y, blue, green, red, alpha);
            light = light || (alpha != 0u && red > 190u && green > 190u && blue > 190u);
            shaded = shaded || (alpha != 0u && red < 200u && green < 200u && blue < 200u);
        }
    }
    expect("shoulder label on a grey disc", light && shaded);

    /* The later line for a key wins; the bad lines were skipped. */
    expect("one symbol", recomp_playstation_buttons_image(0xabull, 32u, 32u, image) &&
        image.width == 32u && image.height == 32u);
    expect("bad glyph", !recomp_playstation_buttons_image(0xcdull, 32u, 32u, image));
    texel(image, 16u, 16u, blue, green, red, alpha);
    const bool pink = alpha == 255u && red > 150u && green < 140u && blue > 150u;
    expect("last mapping wins", pink);

    /* A file line replaces a built-in entry, and the other built-ins stay. */
    const fs::path overrides = root / "overrides";
    fs::create_directories(overrides, error);
    {
        std::ofstream file(overrides / "playstation-buttons.txt", std::ios::binary);
        file << "41b28f3648cd7b19 = triangle\n";
    }
    recomp_playstation_buttons_configure(overrides.u8string());
    expect("built-in overridden", recomp_playstation_buttons_image(0x41b28f3648cd7b19ull, 32u, 32u, image));
    /* One symbol sits at the centre; the default's left Cross disc is blue at x=8. */
    texel(image, 8u, 16u, blue, green, red, alpha);
    expect("override is one symbol", alpha == 255u && green > red + 40u && green > blue + 40u);
    expect("other built-in kept", recomp_playstation_buttons_image(0x6d9306247838ceb2ull, 64u, 32u, image));

    fs::remove_all(root, error);
    if (failures == 0) std::puts("playstation buttons: ok");
    return failures == 0 ? 0 : 1;
}
