// SPDX-License-Identifier: GPL-3.0-or-later
#include "playstation_buttons.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

enum class Glyph {
    Cross, Circle, Square, Triangle, L1, R1, L2, R2, Count,
};

constexpr size_t kGlyphCount = static_cast<size_t>(Glyph::Count);

/* The art files' names in the glyphs folder: Kenney's Input Prompts pack,
   PlayStation Series, Default. Each is a 64x64 PNG with straight alpha. */
const char *glyphFile(Glyph glyph)
{
    switch (glyph) {
    case Glyph::Cross: return "playstation_button_color_cross.png";
    case Glyph::Circle: return "playstation_button_color_circle.png";
    case Glyph::Square: return "playstation_button_color_square.png";
    case Glyph::Triangle: return "playstation_button_color_triangle.png";
    case Glyph::L1: return "playstation_trigger_l1.png";
    case Glyph::R1: return "playstation_trigger_r1.png";
    case Glyph::L2: return "playstation_trigger_l2.png";
    case Glyph::R2: return "playstation_trigger_r2.png";
    default: return "";
    }
}

/* One glyph's BGRA pixels, straight alpha, as the pack ships them. */
struct GlyphArt {
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::vector<uint8_t> bgra;
    bool loaded() const { return width != 0u && height != 0u && !bgra.empty(); }
};

std::unordered_map<uint64_t, std::vector<Glyph>> rules;
std::array<GlyphArt, kGlyphCount> art;
std::array<bool, kGlyphCount> warned;
std::string glyphsFolder;
std::atomic<bool> playstation{false};

/* The game's 64x32 prompts: A/B, X/Y, and L/R. These are texture hashes, not
   retail images; the file beside the launcher adds other resolutions. */
void addBuiltInRules()
{
    rules[0x41b28f3648cd7b19ull] = {Glyph::Cross, Glyph::Circle};
    rules[0x63b8da55f36de4c2ull] = {Glyph::Square, Glyph::Triangle};
    rules[0x6d9306247838ceb2ull] = {Glyph::L1, Glyph::R1};
}

std::string trim(const std::string &text)
{
    const char *space = " \t\r\n";
    const size_t first = text.find_first_not_of(space);
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(space);
    return text.substr(first, last - first + 1u);
}

std::string lowerAscii(std::string text)
{
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool parseGlyph(const std::string &name, Glyph &glyph)
{
    const std::string lower = lowerAscii(name);
    if (lower == "cross") glyph = Glyph::Cross;
    else if (lower == "circle") glyph = Glyph::Circle;
    else if (lower == "square") glyph = Glyph::Square;
    else if (lower == "triangle") glyph = Glyph::Triangle;
    else if (lower == "l1") glyph = Glyph::L1;
    else if (lower == "r1") glyph = Glyph::R1;
    else if (lower == "l2") glyph = Glyph::L2;
    else if (lower == "r2") glyph = Glyph::R2;
    else return false;
    return true;
}

void loadArt(const std::string &folder)
{
    for (size_t i = 0u; i < kGlyphCount; ++i) {
        art[i] = {};
        warned[i] = false;
        const fs::path path = fs::u8path(folder) / glyphFile(static_cast<Glyph>(i));
        std::error_code error;
        if (!fs::is_regular_file(path, error)) continue;
        RecompTexturePackImage image;
        if (!recomp_texture_pack_read_png(path.u8string(), image) || image.levels.empty() ||
            image.width == 0u || image.height == 0u || image.width > 256u || image.height > 256u) {
            continue;
        }
        art[i].width = image.width;
        art[i].height = image.height;
        art[i].bgra = std::move(image.levels.front().bytes);
    }
}

/* Writes art into a square box inside the texture's cell, resampled by area:
   each destination texel averages the source texels it covers. */
void drawCell(std::vector<uint8_t> &pixels, uint32_t textureWidth, uint32_t textureHeight,
    const GlyphArt &glyph, uint32_t cellLeft, uint32_t cellWidth)
{
    const uint32_t box = std::min(cellWidth, textureHeight);
    if (box == 0u) return;
    const uint32_t boxLeft = cellLeft + (cellWidth - box) / 2u;
    const uint32_t boxTop = (textureHeight - box) / 2u;
    const double scaleX = static_cast<double>(glyph.width) / box;
    const double scaleY = static_cast<double>(glyph.height) / box;

    for (uint32_t y = 0u; y < box; ++y) {
        const double top = y, bottom = y + 1.0;
        const double sourceTop = top * scaleY, sourceBottom = bottom * scaleY;
        const uint32_t firstRow = static_cast<uint32_t>(sourceTop);
        const uint32_t lastRow = std::min(glyph.height, static_cast<uint32_t>(sourceBottom) + 1u);
        for (uint32_t x = 0u; x < box; ++x) {
            const double left = x, right = x + 1.0;
            const double sourceLeft = left * scaleX, sourceRight = right * scaleX;
            const uint32_t firstColumn = static_cast<uint32_t>(sourceLeft);
            const uint32_t lastColumn = std::min(glyph.width, static_cast<uint32_t>(sourceRight) + 1u);
            double weight = 0.0, alpha = 0.0, red = 0.0, green = 0.0, blue = 0.0;
            for (uint32_t sourceY = firstRow; sourceY < lastRow; ++sourceY) {
                const double rowWeight = std::min(sourceY + 1.0, sourceBottom) -
                    std::max(static_cast<double>(sourceY), sourceTop);
                if (rowWeight <= 0.0) continue;
                for (uint32_t sourceX = firstColumn; sourceX < lastColumn; ++sourceX) {
                    const double columnWeight = std::min(sourceX + 1.0, sourceRight) -
                        std::max(static_cast<double>(sourceX), sourceLeft);
                    if (columnWeight <= 0.0) continue;
                    const size_t at = (static_cast<size_t>(sourceY) * glyph.width + sourceX) * 4u;
                    const double texelAlpha = glyph.bgra[at + 3u] / 255.0;
                    const double texelWeight = rowWeight * columnWeight;
                    weight += texelWeight;
                    alpha += texelAlpha * texelWeight;
                    red += glyph.bgra[at + 2u] / 255.0 * texelAlpha * texelWeight;
                    green += glyph.bgra[at + 1u] / 255.0 * texelAlpha * texelWeight;
                    blue += glyph.bgra[at + 0u] / 255.0 * texelAlpha * texelWeight;
                }
            }
            if (weight <= 0.0 || alpha <= 0.0) continue;
            const size_t at = (static_cast<size_t>(boxTop + y) * textureWidth + boxLeft + x) * 4u;
            pixels[at + 0u] = static_cast<uint8_t>(std::lround(std::min(1.0, blue / alpha) * 255.0));
            pixels[at + 1u] = static_cast<uint8_t>(std::lround(std::min(1.0, green / alpha) * 255.0));
            pixels[at + 2u] = static_cast<uint8_t>(std::lround(std::min(1.0, red / alpha) * 255.0));
            pixels[at + 3u] = static_cast<uint8_t>(std::lround(std::min(1.0, alpha / weight) * 255.0));
        }
    }
}

} // namespace

void recomp_playstation_buttons_configure(const std::string &textures_folder,
    const std::string &art_folder)
{
    rules.clear();
    addBuiltInRules();
    glyphsFolder = art_folder;
    loadArt(art_folder);
    const fs::path path = fs::u8path(textures_folder) / "playstation-buttons.txt";
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::string line;
    unsigned line_number = 0u;
    while (std::getline(file, line)) {
        ++line_number;
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#') continue;
        const size_t equals = text.find('=');
        std::vector<Glyph> glyphs;
        const std::string key_text = equals == std::string::npos ? std::string() : trim(text.substr(0u, equals));
        if (equals != std::string::npos) {
            const char *start = key_text.c_str();
            char *end = nullptr;
            const unsigned long long key = std::strtoull(start, &end, 16);
            if (key != 0u && end != start && *end == '\0' && key_text.size() <= 16u) {
                const std::string list = text.substr(equals + 1u);
                size_t at = 0u;
                while (at <= list.size()) {
                    const size_t comma = list.find(',', at);
                    const std::string name = trim(list.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                    Glyph glyph;
                    if (name.empty() || !parseGlyph(name, glyph) || glyphs.size() == 8u) {
                        glyphs.clear();
                        break;
                    }
                    glyphs.push_back(glyph);
                    if (comma == std::string::npos) break;
                    at = comma + 1u;
                }
            }
        }
        if (glyphs.empty()) {
            std::fprintf(stderr, "recomp PlayStation buttons: %s:%u is not <key> = <glyph>[,<glyph>...]\n",
                path.u8string().c_str(), line_number);
            continue;
        }
        rules[static_cast<uint64_t>(std::strtoull(key_text.c_str(), nullptr, 16))] = glyphs;
    }
}

void recomp_playstation_buttons_set_active(bool active)
{
    playstation.store(active, std::memory_order_relaxed);
}

bool recomp_playstation_buttons_active(void)
{
    return playstation.load(std::memory_order_relaxed);
}

bool recomp_playstation_buttons_replacing(void)
{
    return recomp_playstation_buttons_active() && !rules.empty();
}

bool recomp_playstation_buttons_image(uint64_t key, uint32_t width, uint32_t height,
    RecompTexturePackImage &out)
{
    if (!recomp_playstation_buttons_active() || key == 0u || width == 0u || height == 0u)
        return false;
    const auto rule = rules.find(key);
    if (rule == rules.end()) return false;

    /* All or nothing: a key whose art is missing keeps the game's own prompt
       rather than showing an Xbox icon beside a PlayStation one. */
    for (Glyph glyph : rule->second) {
        const size_t index = static_cast<size_t>(glyph);
        if (art[index].loaded()) continue;
        if (!warned[index]) {
            warned[index] = true;
            std::fprintf(stderr, "recomp PlayStation buttons: %s is missing; the game's own prompt stays\n",
                (fs::u8path(glyphsFolder) / glyphFile(glyph)).u8string().c_str());
        }
        return false;
    }

    RecompTexturePackLevel level;
    level.width = width;
    level.height = height;
    level.pitch = width * 4u;
    level.bytes.assign(static_cast<size_t>(width) * height * 4u, 0u);
    const unsigned count = static_cast<unsigned>(rule->second.size());
    const uint32_t cellWidth = width / count;
    for (unsigned i = 0u; i < count; ++i)
        drawCell(level.bytes, width, height, art[static_cast<size_t>(rule->second[i])], i * cellWidth, cellWidth);
    out = {};
    out.format = RECOMP_TEXTURE_PACK_BGRA8;
    out.width = width;
    out.height = height;
    out.generate_mips = true;
    out.levels.clear();
    out.levels.push_back(std::move(level));
    return true;
}
