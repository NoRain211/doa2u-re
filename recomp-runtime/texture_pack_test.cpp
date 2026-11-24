// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_pack.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures;
static void expect(const char *what, bool ok)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void setDump(const char *value)
{
#ifdef _WIN32
    _putenv_s("RECOMP_TEXTURE_DUMP", value);
#else
    if (value[0] == '\0') unsetenv("RECOMP_TEXTURE_DUMP");
    else setenv("RECOMP_TEXTURE_DUMP", value, 1);
#endif
}

static void copyDump(const fs::path &root, const std::string &name)
{
    fs::create_directories(root / "textures" / "replacement");
    fs::copy_file(root / "textures" / "dumps" / name,
        root / "textures" / "replacement" / name, fs::copy_options::overwrite_existing);
}

int main()
{
    /* The four keys the other port's compiled C++ produces. */
    std::vector<uint8_t> a8(64u);
    for (size_t i = 0; i < a8.size(); ++i) a8[i] = static_cast<uint8_t>(i & 255u);
    expect("A8R8G8B8 key",
        recomp_texture_pack_key(0x06u, 4u, 4u, a8.data(), a8.size(), nullptr) == 0x90b9f6d015002a1cull);

    std::vector<uint8_t> dxt1(32u);
    for (size_t i = 0; i < dxt1.size(); ++i) dxt1[i] = static_cast<uint8_t>(i * 7u & 255u);
    expect("DXT1 key",
        recomp_texture_pack_key(0x0cu, 8u, 8u, dxt1.data(), dxt1.size(), nullptr) == 0xcbab05443ecb6f41ull);

    std::vector<uint8_t> dxt5(16u);
    for (size_t i = 0; i < dxt5.size(); ++i) dxt5[i] = static_cast<uint8_t>(255u - i);
    expect("DXT5 key",
        recomp_texture_pack_key(0x0fu, 4u, 4u, dxt5.data(), dxt5.size(), nullptr) == 0x7197e3de589eac74ull);

    std::vector<uint8_t> p8(8u);
    for (size_t i = 0; i < p8.size(); ++i) p8[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> palette(1024u);
    for (size_t i = 0; i < palette.size(); ++i) palette[i] = static_cast<uint8_t>(i * 3u & 255u);
    expect("P8 key", recomp_texture_pack_key(0x0bu, 4u, 2u, p8.data(), p8.size(), palette.data()) ==
        0x643b87b7d9d7592eull);

    /* Names */
    expect("DXT1 level 0", recomp_texture_pack_level0_bytes(0x0cu, 8u, 8u) == 32u);
    expect("P8 level 0", recomp_texture_pack_level0_bytes(0x0bu, 4u, 2u) == 8u);
    expect("A8 not packed", recomp_texture_pack_level0_bytes(0x19u, 4u, 4u) == 0u);
    expect("oversized", recomp_texture_pack_level0_bytes(0x06u, 32768u, 32768u) == 0u);
    expect("name",recomp_texture_pack_file_name(0x0cu, 256u, 256u, 0x90b9f6d015002a1cull, "png") ==
        "256x256_90b9f6d015002a1c_DXT1.png");
    expect("key in name",
        recomp_texture_pack_key_in_name("256x256_90b9f6d015002a1c_DXT1.png") == 0x90b9f6d015002a1cull);
    expect("key in extra parts",
        recomp_texture_pack_key_in_name("anim_2_DEADBEEF01234567_256x256_DXT1.dds") == 0xDEADBEEF01234567ull);
    expect("non-hex part", recomp_texture_pack_key_in_name("256x256_0123456789abcdeZ_DXT1.png") == 0u);
    expect("short part", recomp_texture_pack_key_in_name("256x256_0123456789abcde_DXT1.png") == 0u);

    /* One BC1 block: white with index 0, then transparent black from index 3. */
    /* colour0 = 0xffff > colour1 = 0, every index 0: opaque white. */
    uint8_t opaque[8] = {};
    opaque[0] = opaque[1] = 0xffu;
    std::vector<uint8_t> pixels;
    expect("BC1 decodes", recomp_texture_pack_decode_bc(opaque, sizeof opaque, 0x0cu, 4u, 4u, pixels) &&
        pixels.size() == 64u);
    bool white = pixels.size() == 64u;
    for (uint8_t byte : pixels) white = white && byte == 0xffu;
    expect("BC1 white", white);
    /* colour0 = 0 <= colour1 = 0xffff, every index 3: transparent black. */
    uint8_t clear[8] = {};
    std::memset(clear + 2, 0xff, 6u);
    expect("BC1 transparent",
        recomp_texture_pack_decode_bc(clear, sizeof clear, 0x0cu, 4u, 4u, pixels) &&
        pixels.size() == 64u);
    bool zero = pixels.size() == 64u;
    for (uint8_t byte : pixels) zero = zero && byte == 0u;
    expect("BC1 clear", zero);

    /* DDS round trip through a dump and the replacement index. */
    const fs::path root = fs::temp_directory_path() /
        fs::path("recomp_texture_pack_test_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);

    setDump("dds");
    recomp_texture_pack_configure((root / "textures").u8string());

    /* DXT1 with two levels. */
    std::vector<uint8_t> chain(32u + 8u);
    for (size_t i = 0; i < chain.size(); ++i) chain[i] = static_cast<uint8_t>(i * 11u & 255u);
    const uint64_t dxt_key = recomp_texture_pack_key(0x0cu, 8u, 8u, chain.data(), 32u, nullptr);
    recomp_texture_pack_dump(dxt_key, 0x0cu, 8u, 8u, 2u, chain.data(), chain.size(), nullptr);
    const std::string dxt_name = recomp_texture_pack_file_name(0x0cu, 8u, 8u, dxt_key, "dds");
    expect("DXT1 dump exists", fs::is_regular_file(root / "textures" / "dumps" / dxt_name));
    copyDump(root, dxt_name);

    /* BGRA with one level. */
    std::vector<uint8_t> bgra(2u * 2u * 4u);
    for (size_t i = 0; i < bgra.size(); ++i) bgra[i] = static_cast<uint8_t>(i);
    const uint64_t bgra_key = recomp_texture_pack_key(0x06u, 2u, 2u, bgra.data(), bgra.size(), nullptr);
    recomp_texture_pack_dump(bgra_key, 0x06u, 2u, 2u, 1u, bgra.data(), bgra.size(), nullptr);
    const std::string bgra_name = recomp_texture_pack_file_name(0x06u, 2u, 2u, bgra_key, "dds");
    expect("BGRA dump exists", fs::is_regular_file(root / "textures" / "dumps" / bgra_name));
    copyDump(root, bgra_name);

    recomp_texture_pack_configure((root / "textures").u8string());
    RecompTexturePackImage image;
    expect("DXT1 replacement", recomp_texture_pack_find(dxt_key, image) &&
        image.format == RECOMP_TEXTURE_PACK_BC1 && image.width == 8u && image.height == 8u &&
        image.levels.size() == 2u && image.levels[0].pitch == 16u && image.levels[1].pitch == 8u &&
        image.levels[0].bytes == std::vector<uint8_t>(chain.begin(), chain.begin() + 32) &&
        image.levels[1].bytes == std::vector<uint8_t>(chain.begin() + 32, chain.end()));
    expect("BGRA replacement", recomp_texture_pack_find(bgra_key, image) &&
        image.format == RECOMP_TEXTURE_PACK_BGRA8 && image.levels.size() == 1u &&
        image.levels[0].bytes == bgra);
    recomp_texture_pack_reject(bgra_key);
    expect("rejected replacement", !recomp_texture_pack_find(bgra_key, image));

    /* The same through PNG, which also gets a generated host mip chain. */
    setDump("png");
    recomp_texture_pack_configure((root / "textures").u8string());
    std::vector<uint8_t> png_pixels(2u * 2u * 4u);
    for (size_t i = 0; i < png_pixels.size(); ++i) png_pixels[i] = static_cast<uint8_t>(200u - i);
    const uint64_t png_key = recomp_texture_pack_key(0x06u, 2u, 2u, png_pixels.data(),
        png_pixels.size(), nullptr);
    recomp_texture_pack_dump(png_key, 0x06u, 2u, 2u, 1u, png_pixels.data(), png_pixels.size(), nullptr);
    const std::string png_name = recomp_texture_pack_file_name(0x06u, 2u, 2u, png_key, "png");
    expect("PNG dump exists", fs::is_regular_file(root / "textures" / "dumps" / png_name));
    copyDump(root, png_name);
    recomp_texture_pack_configure((root / "textures").u8string());
    expect("PNG replacement", recomp_texture_pack_find(png_key, image) &&
        image.format == RECOMP_TEXTURE_PACK_BGRA8 && image.generate_mips &&
        image.width == 2u && image.levels.size() == 1u && image.levels[0].bytes == png_pixels);

    setDump("");
    fs::remove_all(root, error);
    if (failures == 0) std::printf("texture pack: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
