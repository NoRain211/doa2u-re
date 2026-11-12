/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings_store.h"
#include "combo_hud.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/* One flat JSON object: each key with its value's source text, in order.
   Nested values are kept whole; this file only ever holds scalars. */
using Entries = std::vector<std::pair<std::string, std::string>>;

struct Parser {
    const std::string &text;
    std::size_t at = 0u;

    void space()
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    }
    bool string(std::string *out)
    {
        if (at >= text.size() || text[at] != '"') return false;
        const std::size_t start = at++;
        while (at < text.size() && text[at] != '"') at += text[at] == '\\' ? 2u : 1u;
        if (at >= text.size()) return false;
        ++at;
        if (out != nullptr) *out = text.substr(start, at - start);
        return true;
    }
    /* A value's source text: a string, a nested object or array, or a scalar. */
    bool value(std::string *out)
    {
        const std::size_t start = at;
        if (at < text.size() && text[at] == '"') return string(out);
        int depth = 0;
        while (at < text.size()) {
            const char c = text[at];
            if (c == '"') {
                if (!string(nullptr)) return false;
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') {
                if (depth == 0) break;
                --depth;
            } else if (c == ',' && depth == 0) break;
            ++at;
        }
        std::size_t end = at;
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1u]))) --end;
        *out = text.substr(start, end - start);
        return end > start;
    }
};

bool parse(const std::string &text, Entries *entries)
{
    Parser p{text};
    p.space();
    if (p.at >= text.size() || text[p.at] != '{') return false;
    ++p.at;
    p.space();
    if (p.at < text.size() && text[p.at] == '}') return true;
    for (;;) {
        std::string key, value;
        p.space();
        if (!p.string(&key)) return false;
        p.space();
        if (p.at >= text.size() || text[p.at] != ':') return false;
        ++p.at;
        p.space();
        if (!p.value(&value)) return false;
        entries->emplace_back(key.substr(1u, key.size() - 2u), value);
        p.space();
        if (p.at < text.size() && text[p.at] == ',') {
            ++p.at;
            continue;
        }
        return p.at < text.size() && text[p.at] == '}';
    }
}

const std::string *find(const Entries &entries, const char *key)
{
    for (const auto &entry : entries)
        if (entry.first == key) return &entry.second;
    return nullptr;
}

bool boolean(const Entries &entries, const char *key, bool fallback)
{
    const std::string *v = find(entries, key);
    if (v != nullptr && *v == "true") return true;
    if (v != nullptr && *v == "false") return false;
    return fallback;
}

int integer(const Entries &entries, const char *key, int fallback)
{
    const std::string *v = find(entries, key);
    if (v == nullptr || v->empty()) return fallback;
    char *end = nullptr;
    const long value = std::strtol(v->c_str(), &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<int>(value) : fallback;
}

/* A JSON string's text, for the plain shapes this file holds. */
std::string text(const Entries &entries, const char *key, const std::string &fallback)
{
    const std::string *v = find(entries, key);
    if (v == nullptr || v->size() < 2u || v->front() != '"' || v->back() != '"' ||
        v->find('\\') != std::string::npos) return fallback;
    return v->substr(1u, v->size() - 2u);
}

std::uint8_t nearest(int height)
{
    std::uint8_t best = 0u;
    for (std::uint8_t i = 1u; i < std::size(RECOMP_PC_HEIGHTS); ++i)
        if (std::abs(RECOMP_PC_HEIGHTS[i] - height) < std::abs(RECOMP_PC_HEIGHTS[best] - height)) best = i;
    return best;
}

void choose(const Entries &entries, const RecompPcSettingsStore &store, std::uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    choices[RECOMP_PC_RESOLUTION] = nearest(integer(entries, "height", store.primaryHeight));
    const int samples = integer(entries, "msaa", 32);
    choices[RECOMP_PC_MSAA] = static_cast<std::uint8_t>(std::size(RECOMP_PC_SAMPLES) - 1u);
    for (std::uint8_t i = 0u; i < std::size(RECOMP_PC_SAMPLES); ++i)
        if (RECOMP_PC_SAMPLES[i] == samples) choices[RECOMP_PC_MSAA] = i;
    choices[RECOMP_PC_SMAA] = boolean(entries, "smaa", true) ? 1u : 0u;
    const std::string screen = text(entries, "screen", boolean(entries, "widescreen", true) ? "16:9" : "4:3");
    choices[RECOMP_PC_SCREEN] = RECOMP_PC_ULTRAWIDE; // any other W:H is an ultrawide shape
    for (std::uint8_t i = 0u; i < std::size(RECOMP_PC_SCREENS); ++i)
        if (RECOMP_PC_SCREENS[i] != nullptr && screen == RECOMP_PC_SCREENS[i]) choices[RECOMP_PC_SCREEN] = i;
    choices[RECOMP_PC_RENDERER] = boolean(entries, "vulkan", false) ? 1u : 0u;
    choices[RECOMP_PC_VRR] = boolean(entries, "vrr", false) ? 1u : 0u;
    choices[RECOMP_PC_AUDIO] = boolean(entries, "surround", false) ? 1u : 0u;
    choices[RECOMP_PC_INTRO] = boolean(entries, "skipIntro", false) ? 1u : 0u;
    choices[RECOMP_PC_UNLOCKS] = boolean(entries, "unlockAll", true) ? 0u : 1u;
    choices[RECOMP_PC_CPU_COSTUMES] = boolean(entries, "randomCpuCostumes", false) ? 1u : 0u;
    choices[RECOMP_PC_FREE_CAMERA] = boolean(entries, "freeCamera", false) ? 1u : 0u;
    const std::string colour = text(entries, "lifeBarColour", RECOMP_LIFE_BAR_NAMES[0]);
    choices[RECOMP_PC_LIFE_BAR] = RECOMP_LIFE_BAR_DEFAULT;
    for (std::uint8_t i = 0u; i < RECOMP_LIFE_BAR_COLOURS; ++i)
        if (colour == RECOMP_LIFE_BAR_NAMES[i]) choices[RECOMP_PC_LIFE_BAR] = i;
}

bool read_file(const std::string &path, std::string *out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    *out = s.str();
    return true;
}

void put(Entries *entries, const char *key, std::string value)
{
    for (auto &entry : *entries)
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    entries->emplace_back(key, std::move(value));
}

} // namespace

bool recomp_pc_settings_read(const std::string &path, const RecompPcSettingsStore &store,
    std::uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    Entries entries;
    std::string content;
    const bool read = read_file(path, &content) && parse(content, &entries);
    if (!read) entries.clear();
    choose(entries, store, choices);
    return read;
}

bool recomp_pc_settings_write(const std::string &path, const RecompPcSettingsStore &store,
    const std::uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    Entries entries;
    std::string content;
    if (read_file(path, &content) && !parse(content, &entries)) return false; // never overwrite what we cannot read
    std::uint8_t saved[RECOMP_PC_SETTING_COUNT];
    choose(entries, store, saved);
    const auto flag = [](bool on) { return std::string(on ? "true" : "false"); };
    const unsigned height = choices[RECOMP_PC_RESOLUTION] % std::size(RECOMP_PC_HEIGHTS);
    put(&entries, "height", std::to_string(RECOMP_PC_HEIGHTS[height]));
    put(&entries, "msaa", std::to_string(RECOMP_PC_SAMPLES[choices[RECOMP_PC_MSAA] % std::size(RECOMP_PC_SAMPLES)]));
    put(&entries, "smaa", flag(choices[RECOMP_PC_SMAA] == 1u));
    const unsigned screen = choices[RECOMP_PC_SCREEN] % std::size(RECOMP_PC_SCREENS);
    put(&entries, "widescreen", flag(screen != 0u));
    /* An ultrawide shape already saved stays; choosing Ultrawide saves the monitor's. */
    if (screen != RECOMP_PC_ULTRAWIDE) put(&entries, "screen", std::string("\"") + RECOMP_PC_SCREENS[screen] + "\"");
    else if (saved[RECOMP_PC_SCREEN] != RECOMP_PC_ULTRAWIDE)
        put(&entries, "screen", "\"" + store.ultrawideShape + "\"");
    put(&entries, "vulkan", flag(choices[RECOMP_PC_RENDERER] == 1u));
    put(&entries, "vrr", flag(choices[RECOMP_PC_VRR] == 1u));
    put(&entries, "surround", flag(choices[RECOMP_PC_AUDIO] == 1u));
    put(&entries, "skipIntro", flag(choices[RECOMP_PC_INTRO] == 1u));
    put(&entries, "unlockAll", flag(choices[RECOMP_PC_UNLOCKS] == 0u));
    put(&entries, "randomCpuCostumes", flag(choices[RECOMP_PC_CPU_COSTUMES] == 1u));
    put(&entries, "freeCamera", flag(choices[RECOMP_PC_FREE_CAMERA] == 1u));
    put(&entries, "lifeBarColour", std::string("\"") +
        RECOMP_LIFE_BAR_NAMES[choices[RECOMP_PC_LIFE_BAR] % RECOMP_LIFE_BAR_COLOURS] + "\"");
    std::string out = "{";
    for (std::size_t i = 0u; i < entries.size(); ++i)
        out += (i == 0u ? "\"" : ",\"") + entries[i].first + "\":" + entries[i].second;
    out += "}";
    // Written beside the file and renamed over it, so a failed save keeps the old file.
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file << out;
        file.close();
        if (!file) {
            std::remove(temp.c_str());
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    if (error) std::remove(temp.c_str());
    return !error;
}
