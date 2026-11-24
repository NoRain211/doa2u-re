// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_pack.h"

#include "d3d_texture_model.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPaletteBytes = 256u * 4u;
constexpr uint32_t kMaxSide = 16384u;
constexpr size_t kKeyDigits = 16u;

/* The 64x64 to 128 bit product the mix uses. clang and gcc have
   unsigned __int128; MSVC, which builds the runtime's own tests here, needs
   the same exact product from 32-bit halves. */
uint64_t mix64(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
#else
    const uint64_t a0 = a & 0xffffffffull, a1 = a >> 32;
    const uint64_t b0 = b & 0xffffffffull, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t middle = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);
    const uint64_t low = (middle << 32) | (p00 & 0xffffffffull);
    const uint64_t high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
    return low ^ high;
#endif
}

/* Copied from XboxXBE-Recomp (Copyright (c) 2026 TGP482, MIT; see NOTICE):
   gpu::Hash in sdk/runtime/gpu/shaders.cpp, as gpu::texturedumper::Key in
   sdk/runtime/gpu/texturedumper.cpp uses it. The shape dword order and the P8 palette step
   are what a DOA2U-XBERecomp texture pack's names hold. */
uint64_t Hash(const void *data, size_t size, uint64_t seed = 1469598103934665603ull)
{
    constexpr uint64_t k0 = 0x9E3779B97F4A7C15ull, k1 = 0xE7037ED1A0B428DBull;
    const auto mix = [](uint64_t a, uint64_t b) { return mix64(a, b); };
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint64_t state = seed ^ k0;
    size_t at = 0u;
    for (; at + 16u <= size; at += 16u) {
        uint64_t x, y;
        std::memcpy(&x, bytes + at, 8u);
        std::memcpy(&y, bytes + at + 8u, 8u);
        state = mix(x ^ k1, y ^ state);
    }
    if (const size_t rest = size - at) {
        uint64_t x = 0u, y = 0u;
        if (rest >= 8u) {
            std::memcpy(&x, bytes + at, 8u);
            std::memcpy(&y, bytes + size - 8u, 8u);
        } else {
            std::memcpy(&x, bytes + at, rest);
        }
        state = mix(x ^ k1, y ^ state ^ rest);
    }
    return mix(state ^ k0, static_cast<uint64_t>(size) ^ k1);
}

const char *formatName(uint32_t format_byte)
{
    switch (format_byte) {
    case 0x06u: return "A8R8G8B8";
    case 0x0bu: return "P8";
    case 0x0cu: return "DXT1";
    case 0x0eu: return "DXT3";
    case 0x0fu: return "DXT5";
    case 0x19u: return "A8";
    case 0x28u: return "V8U8";
    default: return "unknown";
    }
}

bool packedFormat(uint32_t format_byte)
{
    return format_byte == 0x06u || format_byte == 0x0bu || format_byte == 0x0cu ||
        format_byte == 0x0eu || format_byte == 0x0fu;
}

bool compressedFormat(uint32_t format_byte)
{
    return format_byte == 0x0cu || format_byte == 0x0eu || format_byte == 0x0fu;
}

uint32_t blockBytes(uint32_t format_byte)
{
    return format_byte == 0x0cu ? 8u : 16u;
}

uint32_t guestBpp(uint32_t format_byte)
{
    return format_byte == 0x0bu ? 1u : 4u;
}

uint32_t levelBytes(uint32_t format_byte, uint32_t width, uint32_t height)
{
    if (compressedFormat(format_byte))
        return std::max(1u, (width + 3u) / 4u) * std::max(1u, (height + 3u) / 4u) *
            blockBytes(format_byte);
    return width * height * guestBpp(format_byte);
}

uint32_t fullChain(uint32_t width, uint32_t height)
{
    uint32_t levels = 1u;
    while ((std::max(width, height) >> levels) != 0u) ++levels;
    return levels;
}

std::string lowerAscii(std::string text)
{
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string pathText(const fs::path &path)
{
    return path.u8string();
}

uint32_t read32(const uint8_t *bytes)
{
    uint32_t value;
    std::memcpy(&value, bytes, 4u);
    return value;
}

void write32(uint8_t *bytes, uint32_t value)
{
    std::memcpy(bytes, &value, 4u);
}

/* ---- Block decoding ----------------------------------------------------- */

void color565(uint16_t color, uint8_t out[4])
{
    const uint32_t r = (color >> 11) & 31u, g = (color >> 5) & 63u, b = color & 31u;
    out[2] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    out[0] = static_cast<uint8_t>((b << 3) | (b >> 2));
    out[3] = 255u;
}

/* BC1's colour block. BC2 and BC3 keep the four-colour mode even when the
   endpoints are ordered the other way. */
void bc1Colors(const uint8_t *block, bool allow_transparent, uint8_t out[16][4])
{
    const uint16_t c0 = static_cast<uint16_t>(block[0] | block[1] << 8);
    const uint16_t c1 = static_cast<uint16_t>(block[2] | block[3] << 8);
    uint8_t colors[4][4];
    color565(c0, colors[0]);
    color565(c1, colors[1]);
    if (!allow_transparent || c0 > c1) {
        for (unsigned channel = 0u; channel < 4u; ++channel) {
            colors[2][channel] = static_cast<uint8_t>((2u * colors[0][channel] + colors[1][channel]) / 3u);
            colors[3][channel] = static_cast<uint8_t>((colors[0][channel] + 2u * colors[1][channel]) / 3u);
        }
    } else {
        for (unsigned channel = 0u; channel < 3u; ++channel)
            colors[2][channel] = static_cast<uint8_t>((colors[0][channel] + colors[1][channel]) / 2u);
        colors[2][3] = 255u;
        colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0u;
    }
    const uint32_t indices = read32(block + 4u);
    for (unsigned i = 0u; i < 16u; ++i)
        std::memcpy(out[i], colors[(indices >> (2u * i)) & 3u], 4u);
}

void bc2Alpha(const uint8_t *block, uint8_t out[16])
{
    uint64_t bits;
    std::memcpy(&bits, block, 8u);
    for (unsigned i = 0u; i < 16u; ++i)
        out[i] = static_cast<uint8_t>(((bits >> (4u * i)) & 0xfu) * 17u);
}

void bc3Alpha(const uint8_t *block, uint8_t out[16])
{
    const uint8_t a0 = block[0], a1 = block[1];
    uint8_t table[8] = {a0, a1, 0u, 0u, 0u, 0u, 0u, 0u};
    if (a0 > a1) {
        for (unsigned i = 0u; i < 6u; ++i)
            table[2u + i] = static_cast<uint8_t>(((6u - i) * a0 + (1u + i) * a1) / 7u);
    } else {
        for (unsigned i = 0u; i < 4u; ++i)
            table[2u + i] = static_cast<uint8_t>(((4u - i) * a0 + (1u + i) * a1) / 5u);
        table[6] = 0u;
        table[7] = 255u;
    }
    uint64_t bits = 0u;
    std::memcpy(&bits, block + 2u, 6u);
    for (unsigned i = 0u; i < 16u; ++i)
        out[i] = table[(bits >> (3u * i)) & 7u];
}

bool decodeBc(const void *blocks, size_t bytes, uint32_t format_byte, uint32_t width,
    uint32_t height, std::vector<uint8_t> &bgra)
{
    if (blocks == nullptr || !compressedFormat(format_byte) || width == 0u || height == 0u)
        return false;
    const uint32_t blocks_wide = std::max(1u, (width + 3u) / 4u);
    const uint32_t blocks_high = std::max(1u, (height + 3u) / 4u);
    const size_t needed = static_cast<size_t>(blocks_wide) * blocks_high * blockBytes(format_byte);
    if (bytes < needed) return false;
    bgra.resize(static_cast<size_t>(width) * height * 4u);
    const auto *source = static_cast<const uint8_t *>(blocks);
    for (uint32_t by = 0u; by < blocks_high; ++by) {
        for (uint32_t bx = 0u; bx < blocks_wide; ++bx) {
            const uint8_t *block = source + (static_cast<size_t>(by) * blocks_wide + bx) *
                blockBytes(format_byte);
            uint8_t colors[16][4];
            uint8_t alpha[16];
            if (format_byte == 0x0cu) {
                bc1Colors(block, true, colors);
                for (unsigned i = 0u; i < 16u; ++i) alpha[i] = colors[i][3];
            } else if (format_byte == 0x0eu) {
                bc2Alpha(block, alpha);
                bc1Colors(block + 8u, false, colors);
            } else {
                bc3Alpha(block, alpha);
                bc1Colors(block + 8u, false, colors);
            }
            for (unsigned py = 0u; py < 4u; ++py) {
                const uint32_t y = by * 4u + py;
                if (y >= height) break;
                for (unsigned px = 0u; px < 4u; ++px) {
                    const uint32_t x = bx * 4u + px;
                    if (x >= width) break;
                    uint8_t *out = bgra.data() + (static_cast<size_t>(y) * width + x) * 4u;
                    std::memcpy(out, colors[py * 4u + px], 4u);
                    out[3] = alpha[py * 4u + px];
                }
            }
        }
    }
    return true;
}

/* ---- DDS ---------------------------------------------------------------- */

constexpr uint32_t fourcc(const char text[5])
{
    return static_cast<uint32_t>(static_cast<uint8_t>(text[0])) |
        static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8 |
        static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16 |
        static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24;
}

constexpr uint32_t kDdsMagic = fourcc("DDS ");
constexpr size_t kDdsHeaderBytes = 128u;
constexpr size_t kDdsFlags = 8u;
constexpr size_t kDdsHeight = 12u;
constexpr size_t kDdsWidth = 16u;
constexpr size_t kDdsPitch = 20u;
constexpr size_t kDdsMipCount = 28u;
constexpr size_t kDdsPixelSize = 76u;
constexpr size_t kDdsPixelFlags = 80u;
constexpr size_t kDdsFourCC = 84u;
constexpr size_t kDdsBitCount = 88u;
constexpr size_t kDdsRedMask = 92u;
constexpr size_t kDdsGreenMask = 96u;
constexpr size_t kDdsBlueMask = 100u;
constexpr size_t kDdsAlphaMask = 104u;
constexpr size_t kDdsCaps = 108u;
constexpr uint32_t kDdsHeaderSize = 124u;
constexpr uint32_t kDdsPixelFormatSize = 32u;
constexpr uint32_t kDdsdRequired = 0x1007u;  // caps, height, width, pixel format
constexpr uint32_t kDdsdPitch = 0x8u;
constexpr uint32_t kDdsdMipCount = 0x20000u;
constexpr uint32_t kDdsdLinearSize = 0x80000u;
constexpr uint32_t kDdpfAlphaPixels = 0x1u;
constexpr uint32_t kDdpfFourCC = 0x4u;
constexpr uint32_t kDdpfRgb = 0x40u;
constexpr uint32_t kCapsComplex = 0x8u;
constexpr uint32_t kCapsTexture = 0x1000u;
constexpr uint32_t kCapsMipmap = 0x400000u;

RecompTexturePackFormat mapPackedFormat(uint32_t format_byte)
{
    switch (format_byte) {
    case 0x0cu: return RECOMP_TEXTURE_PACK_BC1;
    case 0x0eu: return RECOMP_TEXTURE_PACK_BC2;
    case 0x0fu: return RECOMP_TEXTURE_PACK_BC3;
    default: return RECOMP_TEXTURE_PACK_BGRA8;
    }
}

bool readDds(const fs::path &path, RecompTexturePackImage &image)
{
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (file.size() < kDdsHeaderBytes || read32(file.data()) != kDdsMagic) return false;

    const uint32_t pixel_flags = read32(file.data() + kDdsPixelFlags);
    RecompTexturePackFormat format = RECOMP_TEXTURE_PACK_BGRA8;
    bool compressed = false;
    bool alpha_pixels = false;
    if ((pixel_flags & kDdpfFourCC) != 0u) {
        const uint32_t code = read32(file.data() + kDdsFourCC);
        if (code == fourcc("DXT1")) {
            format = RECOMP_TEXTURE_PACK_BC1;
        } else if (code == fourcc("DXT3")) {
            format = RECOMP_TEXTURE_PACK_BC2;
        } else if (code == fourcc("DXT5")) {
            format = RECOMP_TEXTURE_PACK_BC3;
        } else {
            return false;
        }
        compressed = true;
    } else {
        if ((pixel_flags & kDdpfRgb) == 0u || read32(file.data() + kDdsBitCount) != 32u ||
            read32(file.data() + kDdsRedMask) != 0x00ff0000u ||
            read32(file.data() + kDdsGreenMask) != 0x0000ff00u ||
            read32(file.data() + kDdsBlueMask) != 0x000000ffu ||
            read32(file.data() + kDdsAlphaMask) != 0xff000000u) {
            return false;
        }
        alpha_pixels = (pixel_flags & kDdpfAlphaPixels) != 0u;
    }

    const uint32_t width = read32(file.data() + kDdsWidth);
    const uint32_t height = read32(file.data() + kDdsHeight);
    if (width == 0u || height == 0u || width > kMaxSide || height > kMaxSide) return false;
    const uint32_t declared = (read32(file.data() + kDdsFlags) & kDdsdMipCount) != 0u
        ? read32(file.data() + kDdsMipCount) : 1u;
    const uint32_t levels = std::clamp(declared, 1u, fullChain(width, height));

    image = {};
    image.format = format;
    image.width = width;
    image.height = height;
    size_t at = kDdsHeaderBytes;
    uint32_t level_width = width, level_height = height;
    for (uint32_t level = 0u; level < levels; ++level) {
        RecompTexturePackLevel decoded;
        decoded.width = level_width;
        decoded.height = level_height;
        const uint32_t pitch = compressed
            ? std::max(1u, (level_width + 3u) / 4u) * (format == RECOMP_TEXTURE_PACK_BC1 ? 8u : 16u)
            : level_width * 4u;
        const size_t size = static_cast<size_t>(pitch) *
            (compressed ? std::max(1u, (level_height + 3u) / 4u) : level_height);
        if (at + size > file.size()) break;  // a short file: keep the levels it holds
        decoded.pitch = pitch;
        decoded.bytes.assign(file.begin() + static_cast<std::ptrdiff_t>(at),
            file.begin() + static_cast<std::ptrdiff_t>(at + size));
        if (!compressed && !alpha_pixels) {
            for (size_t i = 3u; i < decoded.bytes.size(); i += 4u) decoded.bytes[i] = 255u;
        }
        image.levels.push_back(std::move(decoded));
        at += size;
        level_width = std::max(1u, level_width / 2u);
        level_height = std::max(1u, level_height / 2u);
    }
    return !image.levels.empty();
}

std::vector<uint8_t> ddsHeader(const RecompTexturePackImage &image)
{
    std::vector<uint8_t> header(kDdsHeaderBytes, 0u);
    uint8_t *h = header.data();
    const bool compressed = image.format != RECOMP_TEXTURE_PACK_BGRA8;
    const uint32_t levels = static_cast<uint32_t>(image.levels.size());
    const RecompTexturePackLevel &top = image.levels.front();
    write32(h, kDdsMagic);
    write32(h + 4u, kDdsHeaderSize);
    write32(h + kDdsFlags, kDdsdRequired | kDdsdMipCount | (compressed ? kDdsdLinearSize : kDdsdPitch));
    write32(h + kDdsHeight, image.height);
    write32(h + kDdsWidth, image.width);
    write32(h + kDdsPitch, compressed ? static_cast<uint32_t>(top.bytes.size()) : top.pitch);
    write32(h + kDdsMipCount, levels);
    write32(h + kDdsPixelSize, kDdsPixelFormatSize);
    write32(h + kDdsCaps, kCapsTexture | (levels > 1u ? kCapsComplex | kCapsMipmap : 0u));
    if (compressed) {
        uint32_t code = fourcc("DXT1");
        if (image.format == RECOMP_TEXTURE_PACK_BC2) code = fourcc("DXT3");
        if (image.format == RECOMP_TEXTURE_PACK_BC3) code = fourcc("DXT5");
        write32(h + kDdsPixelFlags, kDdpfFourCC);
        write32(h + kDdsFourCC, code);
        return header;
    }
    write32(h + kDdsPixelFlags, kDdpfRgb | kDdpfAlphaPixels);
    write32(h + kDdsBitCount, 32u);
    write32(h + kDdsRedMask, 0x00ff0000u);
    write32(h + kDdsGreenMask, 0x0000ff00u);
    write32(h + kDdsBlueMask, 0x000000ffu);
    write32(h + kDdsAlphaMask, 0xff000000u);
    return header;
}

bool writeDds(const fs::path &path, const RecompTexturePackImage &image)
{
    if (image.levels.empty()) return false;
    std::ofstream output(path, std::ios::binary);
    const std::vector<uint8_t> header = ddsHeader(image);
    output.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
    for (const RecompTexturePackLevel &level : image.levels)
        output.write(reinterpret_cast<const char *>(level.bytes.data()),
            static_cast<std::streamsize>(level.bytes.size()));
    return static_cast<bool>(output);
}

/* ---- PNG ---------------------------------------------------------------- */

bool readPng(const fs::path &path, RecompTexturePackImage &image)
{
    const std::string name = pathText(path);
    SDL_Surface *loaded = SDL_LoadPNG(name.c_str());
    if (loaded == nullptr) {
        std::fprintf(stderr, "recomp texture pack: %s: %s\n", name.c_str(), SDL_GetError());
        return false;
    }
    SDL_Surface *bgra = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(loaded);
    if (bgra == nullptr) {
        std::fprintf(stderr, "recomp texture pack: %s: %s\n", name.c_str(), SDL_GetError());
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(bgra->w), height = static_cast<uint32_t>(bgra->h);
    if (width == 0u || height == 0u || width > kMaxSide || height > kMaxSide) {
        SDL_DestroySurface(bgra);
        return false;
    }
    image = {};
    image.format = RECOMP_TEXTURE_PACK_BGRA8;
    image.width = width;
    image.height = height;
    image.generate_mips = true;
    RecompTexturePackLevel level;
    level.width = width;
    level.height = height;
    level.pitch = width * 4u;
    level.bytes.resize(static_cast<size_t>(level.pitch) * height);
    for (uint32_t y = 0u; y < height; ++y)
        std::memcpy(level.bytes.data() + static_cast<size_t>(y) * level.pitch,
            static_cast<const uint8_t *>(bgra->pixels) + static_cast<size_t>(y) * bgra->pitch,
            level.pitch);
    SDL_DestroySurface(bgra);
    image.levels.push_back(std::move(level));
    return true;
}

bool writePng(const fs::path &path, const RecompTexturePackImage &image)
{
    if (image.levels.empty()) return false;
    const RecompTexturePackLevel &level = image.levels.front();
    SDL_Surface *surface = SDL_CreateSurfaceFrom(static_cast<int>(level.width),
        static_cast<int>(level.height), SDL_PIXELFORMAT_BGRA32,
        const_cast<uint8_t *>(level.bytes.data()), static_cast<int>(level.pitch));
    if (surface == nullptr) return false;
    const bool ok = SDL_SavePNG(surface, pathText(path).c_str());
    if (!ok) std::fprintf(stderr, "recomp texture pack: %s: %s\n", pathText(path).c_str(), SDL_GetError());
    SDL_DestroySurface(surface);
    return ok;
}

/* ---- Dumping ------------------------------------------------------------ */

bool decodeGuest(const void *guest, size_t guest_bytes, uint32_t format_byte, uint32_t width,
    uint32_t height, uint32_t levels, const void *palette, bool all_levels,
    RecompTexturePackImage &image)
{
    if (guest == nullptr || width == 0u || height == 0u || levels == 0u) return false;
    const auto *source = static_cast<const uint8_t *>(guest);
    image = {};
    image.width = width;
    image.height = height;

    if (compressedFormat(format_byte)) {
        if (!all_levels) {
            RecompTexturePackLevel level;
            level.width = width;
            level.height = height;
            level.pitch = width * 4u;
            if (!decodeBc(source, guest_bytes, format_byte, width, height, level.bytes)) return false;
            image.format = RECOMP_TEXTURE_PACK_BGRA8;
            image.levels.push_back(std::move(level));
            return true;
        }
        image.format = mapPackedFormat(format_byte);
    } else {
        image.format = RECOMP_TEXTURE_PACK_BGRA8;
    }

    size_t at = 0u;
    uint32_t level_width = width, level_height = height;
    const uint32_t count = compressedFormat(format_byte) || all_levels ? std::max(1u, levels) : 1u;
    for (uint32_t level = 0u; level < count; ++level) {
        const uint32_t bytes = levelBytes(format_byte, level_width, level_height);
        if (at + bytes > guest_bytes) return false;
        RecompTexturePackLevel decoded;
        decoded.width = level_width;
        decoded.height = level_height;
        if (compressedFormat(format_byte)) {
            decoded.pitch = std::max(1u, (level_width + 3u) / 4u) * blockBytes(format_byte);
            decoded.bytes.assign(source + at, source + at + bytes);
        } else {
            decoded.pitch = level_width * 4u;
            decoded.bytes.resize(static_cast<size_t>(level_width) * level_height * 4u);
            if (format_byte == 0x0bu) {
                std::vector<uint8_t> indices(static_cast<size_t>(level_width) * level_height);
                if (palette == nullptr || !recomp_d3d_texture_unswizzle(source + at, indices.data(),
                        level_width, level_height, 1u)) return false;
                const auto *colors = static_cast<const uint8_t *>(palette);
                for (size_t i = 0u; i < indices.size(); ++i)
                    std::memcpy(decoded.bytes.data() + i * 4u, colors + indices[i] * 4u, 4u);
            } else if (!recomp_d3d_texture_unswizzle(source + at, decoded.bytes.data(),
                           level_width, level_height, 4u)) {
                return false;
            }
        }
        image.levels.push_back(std::move(decoded));
        at += bytes;
        level_width = std::max(1u, level_width / 2u);
        level_height = std::max(1u, level_height / 2u);
    }
    return true;
}

/* ---- State -------------------------------------------------------------- */

struct State {
    fs::path replacement;
    fs::path dumps;
    bool dumping = false;
    bool dds = false;
    std::unordered_map<uint64_t, fs::path> files;
    std::unordered_set<uint64_t> failed;
    std::unordered_set<uint64_t> dumped;
    std::mutex lock;  // files is read-only after configure; the rest changes
};

State &state()
{
    static State value;
    return value;
}

}  // namespace

void recomp_texture_pack_configure(const std::string &textures_folder)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.lock);
    const fs::path root = fs::u8path(textures_folder);
    s.replacement = root / "replacement";
    s.dumps = root / "dumps";
    s.files.clear();
    s.failed.clear();
    s.dumped.clear();
    s.dumping = false;
    s.dds = false;
    if (const char *dump = std::getenv("RECOMP_TEXTURE_DUMP"); dump != nullptr) {
        if (std::strcmp(dump, "png") == 0) {
            s.dumping = true;
        } else if (std::strcmp(dump, "dds") == 0) {
            s.dumping = true;
            s.dds = true;
        }
    }

    std::error_code error;
    /* shortcut: one index at startup; a folder watcher like the other port's
       if a pack must show changes without a restart. */
    if (fs::is_directory(s.replacement, error)) {
        fs::recursive_directory_iterator it(s.replacement, error);
        const fs::recursive_directory_iterator end;
        for (; !error && it != end; it.increment(error)) {
            std::error_code file_error;
            if (!it->is_regular_file(file_error) || file_error) continue;
            const std::string extension = lowerAscii(it->path().extension().u8string());
            if (extension != ".png" && extension != ".dds") continue;
            const uint64_t key = recomp_texture_pack_key_in_name(it->path().stem().u8string());
            if (key != 0u) s.files.emplace(key, it->path());
        }
    }
    if (s.dumping) fs::create_directories(s.dumps, error);
    std::fprintf(stderr, "recomp texture pack: %zu replacement image(s) in %s%s\n", s.files.size(),
        pathText(s.replacement).c_str(), s.dumping ? s.dds ? ", dumping DDS" : ", dumping PNG" : "");
}

bool recomp_texture_pack_replacing()
{
    return !state().files.empty();
}

bool recomp_texture_pack_dumping()
{
    return state().dumping;
}

uint64_t recomp_texture_pack_key(uint32_t format_byte, uint32_t width, uint32_t height,
    const void *level0, size_t level0_bytes, const void *palette)
{
    if (!packedFormat(format_byte) || width == 0u || height == 0u || level0 == nullptr) return 0u;
    const uint32_t shape[3] = {format_byte, width, height};
    uint64_t key = Hash(level0, level0_bytes, Hash(shape, sizeof shape));
    if (format_byte == 0x0bu && palette != nullptr) key = Hash(palette, kPaletteBytes, key);
    return key != 0u ? key : 1u;
}

bool recomp_texture_pack_find(uint64_t key, RecompTexturePackImage &out)
{
    State &s = state();
    fs::path path;
    {
        std::lock_guard<std::mutex> lock(s.lock);
        const auto it = s.files.find(key);
        if (it == s.files.end() || s.failed.count(key) != 0u) return false;
        path = it->second;
    }
    RecompTexturePackImage image;
    const std::string extension = lowerAscii(path.extension().u8string());
    /* shortcut: load on the presenter thread; an async worker like the other
       port's if a large PNG ever hitches the frame. */
    const bool ok = extension == ".dds" ? readDds(path, image) : readPng(path, image);
    if (!ok) {
        std::lock_guard<std::mutex> lock(s.lock);
        if (s.failed.insert(key).second) {
            std::fprintf(stderr, "recomp texture pack: %s did not load; using the guest texture\n",
                pathText(path).c_str());
        }
        return false;
    }
    std::fprintf(stderr, "recomp texture pack: %s replaces %016llx\n", pathText(path).c_str(),
        static_cast<unsigned long long>(key));
    out = std::move(image);
    return true;
}

void recomp_texture_pack_dump(uint64_t key, uint32_t format_byte, uint32_t width,
    uint32_t height, uint32_t levels, const void *guest, size_t guest_bytes,
    const void *palette)
{
    State &s = state();
    if (!s.dumping || key == 0u || guest == nullptr || !packedFormat(format_byte)) return;
    {
        std::lock_guard<std::mutex> lock(s.lock);
        if (s.files.count(key) != 0u) return;
        if (!s.dumped.insert(key).second) return;
    }
    /* shortcut: synchronous dump; a worker thread like the other port's if
       a dump ever hitches the frame. */
    RecompTexturePackImage image;
    if (!decodeGuest(guest, guest_bytes, format_byte, width, height, levels, palette, s.dds, image)) {
        std::fprintf(stderr, "recomp texture pack: %016llx did not decode for dumping\n",
            static_cast<unsigned long long>(key));
        return;
    }
    const std::string name = recomp_texture_pack_file_name(format_byte, width, height, key,
        s.dds ? "dds" : "png");
    const fs::path path = s.dumps / fs::u8path(name);
    const bool ok = s.dds ? writeDds(path, image) : writePng(path, image);
    std::fprintf(stderr, "recomp texture pack: %s %s\n", ok ? "dumped" : "could not dump",
        pathText(path).c_str());
}

bool recomp_texture_pack_decode_bc(const void *blocks, size_t bytes, uint32_t format_byte,
    uint32_t width, uint32_t height, std::vector<uint8_t> &bgra)
{
    return decodeBc(blocks, bytes, format_byte, width, height, bgra);
}

std::string recomp_texture_pack_file_name(uint32_t format_byte, uint32_t width,
    uint32_t height, uint64_t key, const char *extension)
{
    char name[96];
    std::snprintf(name, sizeof name, "%ux%u_%016llx_%s.%s", width, height,
        static_cast<unsigned long long>(key), formatName(format_byte), extension);
    return name;
}

uint64_t recomp_texture_pack_key_in_name(const std::string &name)
{
    size_t start = 0u;
    while (start < name.size()) {
        const size_t end = std::min(name.find('_', start), name.size());
        const std::string part = name.substr(start, end - start);
        bool hex = part.size() == kKeyDigits;
        for (char c : part) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                hex = false;
                break;
            }
        }
        if (hex) return std::strtoull(part.c_str(), nullptr, 16);
        start = end + 1u;
    }
    return 0u;
}
