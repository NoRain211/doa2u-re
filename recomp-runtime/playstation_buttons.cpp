// SPDX-License-Identifier: GPL-3.0-or-later
#include "playstation_buttons.h"

#include <algorithm>
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

constexpr int kSamples = 4; // per axis; smooths the discs and their symbols

enum class Glyph {
    Cross, Circle, Square, Triangle, L1, R1, L2, R2,
};

std::unordered_map<uint64_t, std::vector<Glyph>> rules;
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

bool shoulder(Glyph glyph)
{
    return glyph == Glyph::L1 || glyph == Glyph::R1 || glyph == Glyph::L2 || glyph == Glyph::R2;
}

const char *shoulderText(Glyph glyph)
{
    switch (glyph) {
    case Glyph::L1: return "L1";
    case Glyph::R1: return "R1";
    case Glyph::L2: return "L2";
    default: return "R2";
    }
}

/* ---- Drawing --------------------------------------------------------- */

struct Rgb { float r, g, b; };

Rgb discColour(Glyph glyph)
{
    switch (glyph) {
    case Glyph::Cross: return {0.18f, 0.42f, 0.84f}; // PlayStation blue
    case Glyph::Circle: return {0.84f, 0.23f, 0.23f}; // red
    case Glyph::Square: return {0.78f, 0.35f, 0.78f}; // pink
    case Glyph::Triangle: return {0.26f, 0.70f, 0.29f}; // green
    default: return {0.62f, 0.64f, 0.68f}; // the shoulder buttons' grey
    }
}

float clamp01(float value)
{
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

/* The symbol's mask, in the disc's pixel space: dx and dy from its centre. */
bool symbolMask(Glyph glyph, float dx, float dy, float radius)
{
    const float distance = std::sqrt(dx * dx + dy * dy);
    switch (glyph) {
    case Glyph::Cross: {
        const float diagonal = 0.70710678f;
        const float u = std::fabs(dx + dy) * diagonal, v = std::fabs(dx - dy) * diagonal;
        return distance < 0.62f * radius && (u < 0.15f * radius || v < 0.15f * radius);
    }
    case Glyph::Circle:
        return std::fabs(distance - 0.42f * radius) < 0.08f * radius;
    case Glyph::Square: {
        const float edge = std::max(std::fabs(dx), std::fabs(dy));
        return edge > 0.28f * radius && edge < 0.44f * radius;
    }
    case Glyph::Triangle: {
        /* Pointing up: the edge opposite each vertex faces away from it. */
        const float reach = 0.56f * radius, band = 0.09f * radius;
        const float normals[3][2] = {{0.0f, -1.0f}, {0.86602540f, 0.5f}, {-0.86602540f, 0.5f}};
        float nearest = -reach;
        bool inside = true;
        for (unsigned i = 0u; i < 3u; ++i) {
            const float edge = dx * normals[i][0] + dy * normals[i][1] - reach * 0.5f;
            inside = inside && edge < 0.0f;
            nearest = std::max(nearest, edge);
        }
        return inside && nearest > -band;
    }
    default:
        return false;
    }
}

/* A 3x5 font for the shoulder buttons' labels. */
struct TinyGlyph { char c; uint8_t rows[5]; };
const TinyGlyph kTiny[] = {
    {'L', {0x4u, 0x4u, 0x4u, 0x4u, 0x7u}},
    {'R', {0x6u, 0x5u, 0x6u, 0x5u, 0x5u}},
    {'1', {0x2u, 0x6u, 0x2u, 0x2u, 0x7u}},
    {'2', {0x6u, 0x1u, 0x2u, 0x4u, 0x7u}},
};

bool tinyRow(char c, unsigned row, uint8_t &bitsOut)
{
    for (const TinyGlyph &glyph : kTiny) {
        if (glyph.c == c) {
            bitsOut = glyph.rows[row];
            return true;
        }
    }
    return false;
}

/* The shoulder label's mask: two 3x5 characters centred in the disc. */
bool textMask(const char *text, float dx, float dy, float radius)
{
    const int count = static_cast<int>(std::strlen(text));
    const int units = count * 4 - 1;
    int scale = static_cast<int>(radius * 1.1f / 5.0f);
    if (scale < 1) scale = 1;
    const float width = static_cast<float>(units * scale), height = static_cast<float>(5 * scale);
    const float x = dx + width * 0.5f, y = dy + height * 0.5f;
    if (x < 0.0f || y < 0.0f || x >= width || y >= height) return false;
    const int column = static_cast<int>(x) / scale, row = static_cast<int>(y) / scale;
    const int index = column / 4;
    if (index >= count || column % 4 == 3) return false;
    uint8_t bits = 0u;
    return tinyRow(text[index], static_cast<unsigned>(row), bits) &&
        (bits & (0x4u >> (column % 4))) != 0u;
}

/* One 4x4-sample point of a glyph's cell: its colour and coverage. */
struct Sample { float r, g, b, a; };

Sample shade(Glyph glyph, float dx, float dy, float radius)
{
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float coverage = clamp01(radius - distance + 0.5f);
    if (coverage <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
    const float falloff = std::sqrt(clamp01(1.0f - distance * distance / (radius * radius)));
    float light = 0.58f + 0.42f * falloff;
    if (dx + dy < -0.35f * radius) light += 0.12f; // a highlight at the top left
    light = std::min(light, 1.0f);
    Rgb colour = discColour(glyph);
    if (shoulder(glyph) ? textMask(shoulderText(glyph), dx, dy, radius)
                        : symbolMask(glyph, dx, dy, radius)) {
        colour = {0.94f, 0.94f, 0.94f};
        light = 1.0f;
    }
    return {colour.r * light, colour.g * light, colour.b * light, coverage};
}

/* Draws one glyph centred at (cx, cy) with the given radius into B8G8R8A8 rows. */
void drawGlyph(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height,
    float cx, float cy, float radius, Glyph glyph)
{
    const int left = std::max(0, static_cast<int>(cx - radius) - 1);
    const int right = std::min(static_cast<int>(width), static_cast<int>(cx + radius) + 2);
    const int top = std::max(0, static_cast<int>(cy - radius) - 1);
    const int bottom = std::min(static_cast<int>(height), static_cast<int>(cy + radius) + 2);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            float red = 0.0f, green = 0.0f, blue = 0.0f, alpha = 0.0f;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float dx = static_cast<float>(x) + (sx + 0.5f) / kSamples - cx;
                    const float dy = static_cast<float>(y) + (sy + 0.5f) / kSamples - cy;
                    const Sample sample = shade(glyph, dx, dy, radius);
                    red += sample.r * sample.a;
                    green += sample.g * sample.a;
                    blue += sample.b * sample.a;
                    alpha += sample.a;
                }
            }
            if (alpha <= 0.0f) continue;
            const float divisor = alpha;
            const size_t at = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4u;
            pixels[at + 0u] = static_cast<uint8_t>(std::lround(clamp01(blue / divisor) * 255.0f));
            pixels[at + 1u] = static_cast<uint8_t>(std::lround(clamp01(green / divisor) * 255.0f));
            pixels[at + 2u] = static_cast<uint8_t>(std::lround(clamp01(red / divisor) * 255.0f));
            pixels[at + 3u] = static_cast<uint8_t>(std::lround(clamp01(alpha / (kSamples * kSamples)) * 255.0f));
        }
    }
}

} // namespace

void recomp_playstation_buttons_configure(const std::string &textures_folder)
{
    rules.clear();
    addBuiltInRules();
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

    RecompTexturePackLevel level;
    level.width = width;
    level.height = height;
    level.pitch = width * 4u;
    level.bytes.assign(static_cast<size_t>(width) * height * 4u, 0u);
    const unsigned count = static_cast<unsigned>(rule->second.size());
    const float radius = std::min(static_cast<float>(width) / count, static_cast<float>(height)) * 0.44f;
    for (unsigned i = 0u; i < count; ++i) {
        const float cx = static_cast<float>(width) * (i + 0.5f) / count;
        const float cy = static_cast<float>(height) * 0.5f;
        drawGlyph(level.bytes, width, height, cx, cy, radius, rule->second[i]);
    }
    out = {};
    out.format = RECOMP_TEXTURE_PACK_BGRA8;
    out.width = width;
    out.height = height;
    out.generate_mips = true;
    out.levels.clear();
    out.levels.push_back(std::move(level));
    return true;
}
