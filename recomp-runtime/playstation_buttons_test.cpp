// SPDX-License-Identifier: GPL-3.0-or-later
#include "playstation_buttons.h"

#include <SDL3/SDL.h>

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

/* A 64x64 glyph: one opaque colour in its middle third, clear at the edges. */
static void writeGlyph(const fs::path &folder, const char *name, Uint8 red, Uint8 green, Uint8 blue)
{
    SDL_Surface *surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_BGRA32);
    if (surface == nullptr) return;
    const SDL_Rect middle{16, 16, 32, 32};
    SDL_FillSurfaceRect(surface, nullptr, 0u);
    SDL_FillSurfaceRect(surface, &middle, SDL_MapSurfaceRGBA(surface, red, green, blue, 255));
    SDL_SavePNG(surface, (folder / name).u8string().c_str());
    SDL_DestroySurface(surface);
}

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

static bool isColour(const RecompTexturePackImage &image, uint32_t x, uint32_t y,
    uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t b, g, r, a;
    texel(image, x, y, b, g, r, a);
    return a == 255u && r == red && g == green && b == blue;
}

int main()
{
    const fs::path root = fs::temp_directory_path() /
        fs::path("recomp_playstation_buttons_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    fs::remove_all(root, error);
    const fs::path folder = root / "textures";
    const fs::path art = root / "glyphs";
    fs::create_directories(folder, error);
    fs::create_directories(art, error);

    /* Each glyph a flat colour, so its cell says which file was drawn there:
       magenta Cross, cyan Circle, yellow Square, green Triangle, orange L1,
       blue R1. */
    writeGlyph(art, "playstation_button_color_cross.png", 255, 0, 255);
    writeGlyph(art, "playstation_button_color_circle.png", 0, 255, 255);
    writeGlyph(art, "playstation_button_color_square.png", 255, 255, 0);
    writeGlyph(art, "playstation_button_color_triangle.png", 0, 255, 0);
    writeGlyph(art, "playstation_trigger_l1.png", 255, 128, 0);
    writeGlyph(art, "playstation_trigger_r1.png", 0, 0, 255);

    RecompTexturePackImage image;
    uint8_t blue, green, red, alpha;
    /* The game's 64x32 prompts work with the built-in keys and the art. */
    recomp_playstation_buttons_configure(folder.u8string(), art.u8string());
    expect("inactive first", !recomp_playstation_buttons_active());
    expect("inactive", !recomp_playstation_buttons_image(kFaces, 64u, 32u, image));
    recomp_playstation_buttons_set_active(true);
    expect("active", recomp_playstation_buttons_active());

    /* A/B: Cross left, Circle right, at the guest texture's size. */
    expect("built-in faces", recomp_playstation_buttons_image(0x41b28f3648cd7b19ull, 64u, 32u, image));
    expect("faces shape", image.format == RECOMP_TEXTURE_PACK_BGRA8 && image.width == 64u &&
        image.height == 32u && image.generate_mips && image.levels.size() == 1u &&
        image.levels[0].pitch == 64u * 4u && image.levels[0].bytes.size() == 64u * 32u * 4u);
    expect("cross left", isColour(image, 16u, 16u, 255u, 0u, 255u));
    expect("circle right", isColour(image, 48u, 16u, 0u, 255u, 255u));
    texel(image, 0u, 0u, blue, green, red, alpha);
    expect("corner clear", alpha == 0u);

    /* X/Y: Square left, Triangle right. */
    expect("built-in X/Y", recomp_playstation_buttons_image(0x63b8da55f36de4c2ull, 64u, 32u, image));
    expect("square left", isColour(image, 16u, 16u, 255u, 255u, 0u));
    expect("triangle right", isColour(image, 48u, 16u, 0u, 255u, 0u));

    /* L/R: the shoulder art, and the same art at a doubled guest size. */
    expect("built-in shoulders", recomp_playstation_buttons_image(0x6d9306247838ceb2ull, 64u, 32u, image));
    expect("L1 left", isColour(image, 16u, 16u, 255u, 128u, 0u));
    expect("R1 right", isColour(image, 48u, 16u, 0u, 0u, 255u));
    expect("scaled shoulders", recomp_playstation_buttons_image(0x6d9306247838ceb2ull, 128u, 64u, image));
    expect("L1 scaled", isColour(image, 32u, 32u, 255u, 128u, 0u));
    expect("R1 scaled", isColour(image, 96u, 32u, 0u, 0u, 255u));

    /* Missing art leaves the game's own prompt: all or nothing. */
    const fs::path none = root / "no-art";
    fs::create_directories(none, error);
    recomp_playstation_buttons_configure(folder.u8string(), none.u8string());
    expect("missing art", !recomp_playstation_buttons_image(0x41b28f3648cd7b19ull, 64u, 32u, image));
    expect("no art unknown key", !recomp_playstation_buttons_image(0x1111111111111111ull, 64u, 32u, image));
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

    recomp_playstation_buttons_configure(folder.u8string(), art.u8string());
    expect("inactive first", !recomp_playstation_buttons_active());

    /* Without a PlayStation pad the game keeps its own icons. */
    expect("inactive", !recomp_playstation_buttons_image(kFaces, 64u, 32u, image));

    recomp_playstation_buttons_set_active(true);
    expect("active", recomp_playstation_buttons_active());
    expect("unknown key", !recomp_playstation_buttons_image(0x1111111111111111ull, 64u, 32u, image));
    expect("no key", !recomp_playstation_buttons_image(0u, 64u, 32u, image));

    /* The file's key and glyph names work in any case, at any size. */
    expect("file faces", recomp_playstation_buttons_image(kFaces, 64u, 32u, image));
    expect("file cross", isColour(image, 16u, 16u, 255u, 0u, 255u));
    expect("file circle", isColour(image, 48u, 16u, 0u, 255u, 255u));
    expect("file shoulders", recomp_playstation_buttons_image(kShoulders, 64u, 32u, image));
    expect("file L1", isColour(image, 16u, 16u, 255u, 128u, 0u));

    /* The later line for a key wins; the bad lines were skipped. */
    expect("one symbol", recomp_playstation_buttons_image(0xabull, 32u, 32u, image) &&
        image.width == 32u && image.height == 32u);
    expect("square over triangle", isColour(image, 16u, 16u, 255u, 255u, 0u));
    expect("bad glyph", !recomp_playstation_buttons_image(0xcdull, 32u, 32u, image));

    /* A file line replaces a built-in entry, and the other built-ins stay. */
    const fs::path overrides = root / "overrides";
    fs::create_directories(overrides, error);
    {
        std::ofstream file(overrides / "playstation-buttons.txt", std::ios::binary);
        file << "41b28f3648cd7b19 = triangle\n";
    }
    recomp_playstation_buttons_configure(overrides.u8string(), art.u8string());
    expect("built-in overridden", recomp_playstation_buttons_image(0x41b28f3648cd7b19ull, 32u, 32u, image));
    expect("override is one symbol", isColour(image, 16u, 16u, 0u, 255u, 0u));
    expect("other built-in kept", recomp_playstation_buttons_image(0x6d9306247838ceb2ull, 64u, 32u, image));

    fs::remove_all(root, error);
    if (failures == 0) std::puts("playstation buttons: ok");
    return failures == 0 ? 0 : 1;
}
