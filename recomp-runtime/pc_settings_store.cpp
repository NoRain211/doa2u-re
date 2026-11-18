/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings_store.h"
#include "combo_hud.h"
#include "settings_file.h"

#include <cctype>
#include <cstdlib>
#include <iterator>
#include <utility>
#include <vector>

namespace {

using Entries = RecompSettingsEntries;

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

std::string text(const Entries &entries, const char *key, const std::string &fallback)
{
    const std::string *v = find(entries, key);
    std::string out;
    return v != nullptr && recomp_settings_string(*v, &out) ? out : fallback;
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
    choices[RECOMP_PC_DEDICATION] = boolean(entries, "dedicationCard", true) ? 0u : 1u;
    const std::string colour = text(entries, "lifeBarColour", RECOMP_LIFE_BAR_NAMES[0]);
    choices[RECOMP_PC_LIFE_BAR] = RECOMP_LIFE_BAR_DEFAULT;
    for (std::uint8_t i = 0u; i < RECOMP_LIFE_BAR_COLOURS; ++i)
        if (colour == RECOMP_LIFE_BAR_NAMES[i]) choices[RECOMP_PC_LIFE_BAR] = i;
    // hitVoices turns them off; hitVoiceVolume, added later, picks Quiet, Normal or Loud.
    const std::string volume = text(entries, "hitVoiceVolume", "normal");
    choices[RECOMP_PC_HIT_VOICES] = !boolean(entries, "hitVoices", true) ? 0u
        : volume == "quiet" ? 1u : volume == "loud" ? 3u : 2u;
    choices[RECOMP_PC_KO_REPLAY] = text(entries, "koReplay", "dynamic") == "original" ? 0u : 1u;
    // fightHud replaced comboHud, whose Off is Original and On is Both.
    const std::string hud = text(entries, "fightHud", boolean(entries, "comboHud", true) ? "both" : "original");
    choices[RECOMP_PC_FIGHT_HUD] = hud == "original" ? 0u : hud == "damage" ? 1u : hud == "stun" ? 2u : 3u;
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
    const bool read = recomp_settings_read(path, &content) && recomp_settings_parse(content, &entries);
    if (!read) entries.clear();
    choose(entries, store, choices);
    return read;
}

std::uint32_t recomp_pc_settings_custom_life_bar(const std::string &path)
{
    Entries entries;
    std::string content;
    if (!recomp_settings_read(path, &content) || !recomp_settings_parse(content, &entries)) return 0xffffffu;
    std::string hex = text(entries, "lifeBarCustom", "FFFFFF");
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    if (hex.size() != 6u) return 0xffffffu;
    std::uint32_t rgb = 0u;
    for (const char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return 0xffffffu;
        rgb = rgb << 4 | static_cast<std::uint32_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
            : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
    }
    return rgb;
}

bool recomp_pc_settings_write(const std::string &path, const RecompPcSettingsStore &store,
    const std::uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    Entries entries;
    std::string content;
    if (recomp_settings_read(path, &content) && !recomp_settings_parse(content, &entries)) return false; // never overwrite what we cannot read
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
    put(&entries, "dedicationCard", flag(choices[RECOMP_PC_DEDICATION] == 0u));
    put(&entries, "lifeBarColour", std::string("\"") +
        RECOMP_LIFE_BAR_NAMES[choices[RECOMP_PC_LIFE_BAR] % RECOMP_LIFE_BAR_COLOURS] + "\"");
    put(&entries, "hitVoices", flag(choices[RECOMP_PC_HIT_VOICES] != 0u));
    // Off keeps the volume last chosen.
    static const char *const VOLUMES[4] = {nullptr, "quiet", "normal", "loud"};
    if (choices[RECOMP_PC_HIT_VOICES] != 0u && choices[RECOMP_PC_HIT_VOICES] < 4u)
        put(&entries, "hitVoiceVolume", std::string("\"") + VOLUMES[choices[RECOMP_PC_HIT_VOICES]] + "\"");
    put(&entries, "koReplay", choices[RECOMP_PC_KO_REPLAY] == 0u ? "\"original\"" : "\"dynamic\"");
    static const char *const HUDS[4] = {"original", "damage", "stun", "both"};
    put(&entries, "fightHud", std::string("\"") + HUDS[choices[RECOMP_PC_FIGHT_HUD] & 3u] + "\"");
    std::string out = "{";
    for (std::size_t i = 0u; i < entries.size(); ++i)
        out += (i == 0u ? "\"" : ",\"") + entries[i].first + "\":" + entries[i].second;
    out += "}";
    return recomp_settings_write(path, out);
}
