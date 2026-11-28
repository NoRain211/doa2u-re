// SPDX-License-Identifier: GPL-3.0-or-later
// Linux launcher: pick resolution, anti-aliasing, widescreen, 5.1 surround, the free camera,
// the intro skip, the staff-roll dedication card and texture dumping, remember them in private/launcher.json, then exit 0 so
// run_game.py starts the game. Mirrors tools/launcher.cs, its GAME page's build included,
// with MSAA up to SDL_GPU's 8x: the backdrop
// under a top bar, and a panel on the left with the page tabs, the chosen page's
// rows and their information. Adapted from committed DOA3 tools/launcher_sdl.cpp at ce393e4.
// Gamepad, keyboard, mouse and touch all work, so it runs from Steam Game Mode.
// Exits 1 when closed without Play.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

// Everything is laid out on a 640x400 canvas and scaled by whole numbers,
// so SDL's 8x8 debug font stays sharp: 2x on the Steam Deck's 1280x800.
constexpr int kWidth = 640, kHeight = 400;
// Horizon Bar layout, as in tools/launcher.cs.
constexpr float kTopH = 30, kSide = 20, kFadeY = 230;
// The left panel: tab strip, rows, then two information lines at its bottom.
constexpr float kPanelW = 320, kPad = 14, kTabY = 44, kRowY = 72, kRowH = 24, kInfoY = 360;

const int kHeights[] = {480, 720, 1080, 1440, 2160};
const int kSamples[] = {1, 2, 4, 8};
const char *const kScreens[] = {"4:3", "16:9", "16:10", "21:9", "32:9"};
const char *const kSharpens[] = {"off", "low", "medium", "high"};
const char *const kDumps[] = {"off", "png", "dds"};
const char *const kSharpenLabels[] = {"OFF", "LOW", "MEDIUM", "HIGH"};
const char *const kDumpLabels[] = {"OFF", "PNG", "DDS"};

enum Item { RESOLUTION, MSAA, SMAA, SHARPEN, AO, WIDESCREEN, SURROUND, SKIP_INTRO, UNLOCKS, CPU_COSTUMES, FREE_CAMERA, DEDICATION, TEXTURE_DUMP, UPDATE, CHECK_UPDATES, CHOOSE_ISO, BUILD, PLAY, ITEM_COUNT };
// One row per Item, in Item order: its label, how many values it steps through and what it does.
struct Row { const char *label; int count; const char *info; };
const Row kRows[] = {
    {"RESOLUTION", static_cast<int>(std::size(kHeights)), "Sets the render height; the width follows the screen shape"},
    {"MSAA", static_cast<int>(std::size(kSamples)), "Smooths polygon edges, up to SDL_GPU's 8x"},
    {"SMAA", 2, "Smooths edges with SMAA Ultra after MSAA"},
    {"SHARPENING", static_cast<int>(std::size(kSharpens)), "Sharpens the picture with AMD FidelityFX CAS after anti-aliasing"},
    {"AMBIENT OCCLUSION", 2, "Darkens creases and contact areas; off leaves the picture as the game drew it"},
    {"SCREEN", static_cast<int>(std::size(kScreens)), "Widescreen widens the game's camera; its 16:9 menus keep their shape"},
    {"OUTPUT", 2, "5.1 plays the game's own surround mix"},
    {"INTRO MOVIES", 2, "Skip goes straight to the title screen at boot"},
    {"UNLOCKS", 2, "Earn offers only what the profile earned; Everything is never saved"},
    {"CPU COSTUMES", 2, "Random dresses CPU opponents in any unlocked costume; offline only"},
    {"FREE CAMERA", 2, "In game, F1 or a right-stick click flies the camera"},
    {"DEDICATION", 2, "Shows a memorial card before the staff roll"},
    {"TEXTURE DUMP", static_cast<int>(std::size(kDumps)), "Writes each texture once to textures/dumps for editing"},
    {"UPDATE", 1, "Checks GitHub for a newer release and installs it"},
    {"CHECK AT START", 2, "Looks for a newer release when the launcher opens"},
    {"DISC 2 ISO", 1, ""}, // its information is the build status
    {"BUILD", 1, "Builds the game from your own Disc 2 ISO; takes about 10 minutes"},
    {"START GAME", 1, "Saves these settings and starts the game"},
};
static_assert(std::size(kRows) == ITEM_COUNT, "kRows needs one row per Item");

struct Page { const char *name, *info; std::vector<int> rows; };
// Tab names are short: all five must fit the 320-pixel panel.
const Page kPages[] = {
    {"GAME", "Starts the game, or builds it from your own Disc 2 ISO", {PLAY, CHOOSE_ISO, BUILD}},
    {"VIDEO", "Sets resolution, anti-aliasing, sharpening, occlusion, screen shape and the intro movies", {RESOLUTION, MSAA, SMAA, SHARPEN, AO, WIDESCREEN, SKIP_INTRO}},
    {"AUDIO", "Chooses stereo or the game's own 5.1 surround mix", {SURROUND}},
    {"GAMEPLAY", "Chooses unlocks, CPU costumes, free camera, dedication and texture dumps", {UNLOCKS, CPU_COSTUMES, FREE_CAMERA, DEDICATION, TEXTURE_DUMP}},
    {"UPDATE", "Checks GitHub for a newer release and installs it", {UPDATE, CHECK_UPDATES}},
};
constexpr int kPageCount = static_cast<int>(std::size(kPages)), kGamePage = 0, kUpdatePage = 4;
// The GAME page's rows.
constexpr int kPlayRow = 0, kDiscRow = 1, kBuildRow = 2;

// launcher.cs's defaults: the screen height, the most MSAA, SMAA and widescreen on, sharpening and AO off.
struct Settings {
    int height = 0;
    int msaa = 8;
    bool smaa = true;
    std::string sharpen = "off"; // "off", "low", "medium" or "high"
    bool ao = false;
    std::string screen = "16:9"; // "4:3", "16:9", "16:10", "32:9" or a 21:9 "W:H"
    bool surround = false;
    bool skip_intro = false;
    bool check_updates = true;
    bool unlock_all = true;
    bool random_cpu_costumes = false;
    bool free_camera = false;
    bool dedication = true;
    std::string texture_dump = "off"; // "off", "png" or "dds"
    std::string display_mode = "borderless"; // "windowed", "borderless" or "fullscreen", the game's PC SETTINGS row
    std::string game_source; // the Disc 2 ISO the GAME page builds from, shared with the Windows launcher's key
};

struct Color { Uint8 r, g, b; };

// True when the first open gamepad is a PlayStation one: the top bar's prompts
// then show its own button glyphs.
bool playstationPad()
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    bool playstation = false;
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) continue;
        const SDL_GamepadType type = SDL_GetGamepadType(pad);
        playstation = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
            type == SDL_GAMEPAD_TYPE_PS5;
        break;
    }
    SDL_free(ids);
    return playstation;
}
constexpr Color kDark{42, 29, 21}, kWhite{243, 238, 232}, kSand{241, 197, 143};
constexpr Color kMuted{191, 178, 165}, kGold{232, 191, 138}, kBar{13, 10, 9};

// A PlayStation prompt glyph from assets/glyphs/playstation beside the launcher, which the
// runtime uses too; null when the file is missing.
SDL_Texture *loadGlyph(SDL_Renderer *renderer, const std::string &root, const char *name)
{
    SDL_Surface *picture = SDL_LoadPNG((root + "/assets/glyphs/playstation/" + name + ".png").c_str());
    if (picture == nullptr) return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, picture);
    SDL_DestroySurface(picture);
    return texture;
}

// Reads the flat object launcher.cs, launcher_macos.swift and this file write.
bool jsonValue(const std::string &text, const char *key, std::string &value)
{
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return false;
    auto start = text.find(':', at);
    if (start == std::string::npos) return false;
    start = text.find_first_not_of(" \t\r\n", start + 1);
    if (start == std::string::npos) return false;
    value = text.substr(start, text.find_first_of(",}\r\n", start) - start);
    value.erase(value.find_last_not_of(" \t") + 1);
    return true;
}

// A string value, with its backslash, quote and slash escapes undone; false on any other escape.
bool jsonText(const std::string &text, const char *key, std::string &value)
{
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return false;
    auto i = text.find(':', at);
    if (i == std::string::npos) return false;
    i = text.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos || text[i] != '"') return false;
    value.clear();
    for (++i; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\x5c' && i + 1 < text.size()) {
            ++i;
            // Ignored rather than misread (\n or \u00e9 would lose its backslash), so a save keeps the raw value.
            if (text[i] != '\x5c' && text[i] != '"' && text[i] != '/') { value.clear(); return false; }
        }
        value += text[i];
    }
    return true;
}

Settings loadSettings(const std::string &path)
{
    Settings settings;
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string value;
    if (jsonValue(text, "height", value)) settings.height = std::atoi(value.c_str());
    if (jsonValue(text, "msaa", value)) settings.msaa = std::atoi(value.c_str());
    if (jsonValue(text, "smaa", value)) settings.smaa = value == "true";
    std::string sharpen;
    if (jsonText(text, "sharpen", sharpen)) settings.sharpen = sharpen;
    if (jsonValue(text, "ambientOcclusion", value)) settings.ao = value == "true";
    // Settings saved before 16:10 hold only a widescreen flag.
    if (jsonValue(text, "widescreen", value)) settings.screen = value == "true" ? "16:9" : "4:3";
    if (jsonValue(text, "screen", value) && value.size() > 2) settings.screen = value.substr(1, value.size() - 2);
    if (jsonValue(text, "surround", value)) settings.surround = value == "true";
    if (jsonValue(text, "skipIntro", value)) settings.skip_intro = value == "true";
    if (jsonValue(text, "checkUpdates", value)) settings.check_updates = value == "true";
    if (jsonValue(text, "unlockAll", value)) settings.unlock_all = value == "true";
    if (jsonValue(text, "freeCamera", value)) settings.free_camera = value == "true";
    if (jsonValue(text, "randomCpuCostumes", value)) settings.random_cpu_costumes = value == "true";
    if (jsonValue(text, "dedicationCard", value)) settings.dedication = value == "true";
    jsonText(text, "textureDump", settings.texture_dump);
    jsonText(text, "displayMode", settings.display_mode); // the game's PC SETTINGS row, kept as saved
    // Only its three values are saved back; anything else is borderless, as the game reads it.
    if (settings.display_mode != "windowed" && settings.display_mode != "fullscreen") settings.display_mode = "borderless";
    jsonText(text, "game_source", settings.game_source);
    return settings;
}

// One past a string token at at (a quote), or false when it never ends.
bool jsonString(const std::string &text, size_t &at)
{
    if (at >= text.size() || text[at] != '"') return false;
    for (++at; at < text.size(); ++at) {
        if (text[at] == '\\') { ++at; continue; }
        if (text[at] == '"') { ++at; return true; }
    }
    return false;
}

// The object's top-level keys and raw values, in file order. Values are copied
// verbatim, so nested objects and arrays survive; false means malformed.
bool jsonEntries(const std::string &text, std::vector<std::pair<std::string, std::string>> &entries)
{
    size_t at = text.find_first_not_of(" \t\r\n");
    if (at == std::string::npos || text[at] != '{') return false;
    ++at;
    for (;;) {
        at = text.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos) return false;
        if (text[at] == '}') return true;
        const size_t key_start = at;
        if (!jsonString(text, at)) return false;
        const std::string key = text.substr(key_start + 1, at - key_start - 2);
        at = text.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos || text[at] != ':') return false;
        at = text.find_first_not_of(" \t\r\n", ++at);
        if (at == std::string::npos) return false;
        const size_t value_start = at;
        int depth = 0;
        bool quoted = false;
        for (; at < text.size(); ++at) {
            const char c = text[at];
            if (quoted) {
                if (c == '\\') ++at;
                else if (c == '"') quoted = false;
            } else if (c == '"') {
                quoted = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (depth == 0) break; // the object's own brace
                --depth;
            } else if (c == ',' && depth == 0) {
                break;
            }
        }
        if (at >= text.size()) return false;
        size_t value_end = at;
        while (value_end > value_start && std::string(" \t\r\n").find(text[value_end - 1]) != std::string::npos)
            --value_end;
        entries.emplace_back(key, text.substr(value_start, value_end - value_start));
        if (text[at] == '}') return true;
        ++at; // the comma
    }
}

bool saveSettings(const std::string &path, const Settings &settings)
{
    // The file is shared: the Windows launcher, the in-game PC SETTINGS page
    // and run_game.py read keys this launcher does not write (vulkan, vrr,
    // frameRate, game_source, window...), so keep every entry it does not own.
    std::vector<std::pair<std::string, std::string>> entries;
    std::ifstream existing(path, std::ios::binary);
    if (existing.is_open()) {
        const std::string text{std::istreambuf_iterator<char>(existing), std::istreambuf_iterator<char>()};
        existing.close();
        // Never rewrite what this cannot read: the file is shared, and a save
        // that drops the others' keys is worse than no save. A blank file
        // holds no keys to lose, so it is rewritten.
        if (text.find_first_not_of(" \t\r\n") != std::string::npos && !jsonEntries(text, entries))
            return false;
    } else {
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) return false;
    }
    const auto put = [&entries](const char *key, const std::string &value) {
        for (auto &entry : entries)
            if (entry.first == key) { entry.second = value; return; }
        entries.emplace_back(key, value);
    };
    put("height", std::to_string(settings.height));
    put("msaa", std::to_string(settings.msaa));
    put("smaa", settings.smaa ? "true" : "false");
    put("sharpen", std::string("\"") + settings.sharpen + "\"");
    put("ambientOcclusion", settings.ao ? "true" : "false");
    put("widescreen", settings.screen != "4:3" ? "true" : "false");
    put("screen", std::string("\"") + settings.screen + "\"");
    put("skipIntro", settings.skip_intro ? "true" : "false");
    put("checkUpdates", settings.check_updates ? "true" : "false");
    put("unlockAll", settings.unlock_all ? "true" : "false");
    put("surround", settings.surround ? "true" : "false");
    put("freeCamera", settings.free_camera ? "true" : "false");
    put("randomCpuCostumes", settings.random_cpu_costumes ? "true" : "false");
    put("dedicationCard", settings.dedication ? "true" : "false");
    put("textureDump", std::string("\"") + settings.texture_dump + "\"");
    put("displayMode", std::string("\"") + settings.display_mode + "\"");
    if (!settings.game_source.empty()) {
        std::string quoted = "\"";
        for (const char c : settings.game_source) {
            if (c == '"' || c == '\x5c') quoted += '\x5c';
            quoted += c;
        }
        put("game_source", quoted + '"');
    }
    // Written beside the file and renamed over it, so a torn save keeps the
    // old file instead of leaving one the next save refuses to read.
    const std::string temp = path + ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        file << "{\n";
        for (size_t i = 0; i < entries.size(); ++i)
            file << (i == 0 ? "  \"" : ",\n  \"") << entries[i].first << "\": " << entries[i].second;
        file << "\n}\n";
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

float tabWidth(int m) { return static_cast<float>(SDL_strlen(kPages[m].name) * 8); }
// The tabs span the panel's inner width, first at its left edge and last at its right, evenly spaced.
float tabGap()
{
    float total = 0;
    for (int m = 0; m < kPageCount; ++m) total += tabWidth(m);
    return (kPanelW - 2 * kPad - total) / (kPageCount - 1);
}
float tabX(int m)
{
    float x = kPad;
    for (int i = 0; i < m; ++i) x += tabWidth(i) + tabGap();
    return x;
}

int hitTab(float x, float y)
{
    if (y < kTopH || y >= kRowY - 6) return -1;
    for (int m = 0; m < kPageCount; ++m)
        if (x >= tabX(m) - tabGap() / 2 && x < tabX(m) + tabWidth(m) + tabGap() / 2) return m;
    return -1;
}

int hitRow(float x, float y, size_t rows)
{
    if (x >= kPanelW || y < kRowY) return -1;
    const int i = static_cast<int>((y - kRowY) / kRowH);
    return i < static_cast<int>(rows) ? i : -1;
}

// Word-wraps text into the information area's two lines; anything longer ends in "...".
std::vector<std::string> wrap(const std::string &value)
{
    const size_t width = static_cast<size_t>((kPanelW - 2 * kPad) / 8);
    std::vector<std::string> lines;
    size_t at = 0;
    while (lines.size() < 2) {
        at = std::min(value.find_first_not_of(' ', at), value.size());
        if (at == value.size()) break;
        if (value.size() - at <= width) { lines.push_back(value.substr(at)); at = value.size(); break; }
        size_t cut = value.rfind(' ', at + width);
        if (cut == std::string::npos || cut <= at) cut = at + width; // a word longer than a line
        lines.push_back(value.substr(at, cut - at));
        at = cut;
    }
    if (value.find_first_not_of(' ', at) != std::string::npos) {
        std::string &last = lines.back();
        last.erase(std::min(last.size(), width - 3));
        last.erase(last.find_last_not_of(' ') + 1);
        last += "...";
    }
    return lines;
}

struct Launcher {
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *background = nullptr;
    // The PlayStation prompts' art (assets/glyphs/playstation, beside the launcher). A missing
    // file keeps that prompt spelled out, so the launcher works from a bare checkout.
    SDL_Texture *cross = nullptr, *circle = nullptr, *l1 = nullptr, *r1 = nullptr;
    int choice[ITEM_COUNT] = {};
    // menu is the chosen page. The focus is on its row cursor while in_page, else on the tab strip.
    int menu = kGamePage, row = 0;
    bool in_page = true;
    bool built = false;
    SDL_Window *window = nullptr;
    // The build runs build_game.sh (build_steamdeck.sh on SteamOS) as a child that logs to private/build.log.
    // build_info is the GAME page's notice: build progress or a refused action, kept until the next input.
    std::string game_source, build_info;
    SDL_Process *build = nullptr;
    int build_pid = 0;
    bool stop_armed = false; // the first exit request during a build asks for a second, for 3 seconds
    Uint64 build_start = 0, next_log = 0;
    // tools/update.py runs as a child process. pending is the newer version a check found.
    std::string root, pending, status;
    SDL_Process *update = nullptr;
    bool installing = false, quiet = false;

    const std::vector<int> &rows() const { return kPages[menu].rows; }

    std::string valueText(int item) const
    {
        switch (item) {
        case RESOLUTION: return std::to_string(kHeights[choice[item]]) + "P";
        case MSAA: return kSamples[choice[item]] == 1 ? "OFF" : std::to_string(kSamples[choice[item]]) + "X";
        case SHARPEN: return kSharpenLabels[choice[item]];
        case AO: return choice[item] ? "ON" : "OFF";
        case WIDESCREEN: return kScreens[choice[item]];
        case SURROUND: return choice[item] ? "5.1" : "STEREO";
        case SKIP_INTRO: return choice[item] ? "SKIP" : "PLAY";
        case UPDATE: return update != nullptr ? "WORKING" : pending.empty() ? "CHECK" : "INSTALL";
        case CHOOSE_ISO: return "CHOOSE";
        case BUILD: return build != nullptr ? "WORKING" : "BUILD";
        case PLAY: return "START";
        case UNLOCKS: return choice[item] ? "EARN" : "EVERYTHING";
        case CPU_COSTUMES: return choice[item] ? "RANDOM" : "DEFAULT";
        case DEDICATION: return choice[item] ? "HIDE" : "SHOW";
        case TEXTURE_DUMP: return kDumpLabels[choice[item]];
        default: return choice[item] ? "ON" : "OFF";
        }
    }

    void change(int item, int step)
    {
        choice[item] = std::clamp(choice[item] + step, 0, kRows[item].count - 1);
    }

    // Where a value row's "< VALUE >" starts; it ends at the panel's inner right edge.
    float valueLeft(int item) const { return kPanelW - kPad - 24 - valueText(item).size() * 16.f; }

    std::string rowInfo(int item) const
    {
        if (item == CHOOSE_ISO) return buildStatus();
        if (item == UPDATE && !status.empty()) return status;
        return kRows[item].info;
    }

    // A refused action's notice lasts until the next input; a running build keeps its progress up.
    void clearNotice()
    {
        if (build == nullptr) build_info.clear();
    }

    // Checks for a newer release, or installs the one found. A quiet check says nothing unless it finds one.
    void startUpdate(bool quietly)
    {
        if (update != nullptr) return;
        // An install rewrites the scripts and tools a running build reads.
        if (!pending.empty() && build != nullptr) { status = "Wait for the build to finish"; return; }
        installing = !pending.empty();
        quiet = quietly;
        const std::string script = root + "/tools/update.py";
        // The interpreter run_game.sh and build_game.sh prefer, else python3 on PATH.
        std::string python = root + "/private/venv/bin/python3";
        if (!SDL_GetPathInfo(python.c_str(), nullptr)) python = "python3";
        const char *args[] = {python.c_str(), script.c_str(), installing ? "install" : "check", nullptr};
        update = SDL_CreateProcess(args, true);
        if (!quiet) status = update == nullptr ? std::string("Cannot run python3: ") + SDL_GetError()
            : installing ? "Downloading " + pending : "Checking for updates";
    }

    // Collects the update process once it has exited; its output is one line.
    void pollUpdate()
    {
        int code = 0;
        if (update == nullptr || !SDL_WaitProcess(update, false, &code)) return;
        char *data = static_cast<char *>(SDL_ReadProcess(update, nullptr, nullptr));
        SDL_DestroyProcess(update);
        update = nullptr;
        std::string output(data != nullptr ? data : "");
        SDL_free(data);
        output.erase(output.find_last_not_of(" \r\n") + 1);
        output.erase(0, output.rfind('\n') + 1);
        if (code == 0 && installing) {
            // An install prints nothing when the release it found is no longer newer.
            pending.clear();
            status = output.empty() ? "You have the latest version"
                : "Updated to " + output + ". Run ./build_game.sh, then reopen the launcher";
        } else if (code == 0) {
            pending = output;
            status = pending.empty() ? "You have the latest version" : "Version " + pending + " is available. Choose Install";
        } else if (!quiet) {
            status = output.empty() ? "Update failed (exit " + std::to_string(code) + ")" : output.substr(0, 75);
        }
    }

    // Play and exit wait for an install, which replaces the tools; this shows why on the update page.
    bool waitForInstall()
    {
        if (update == nullptr || !installing) return false;
        menu = kUpdatePage;
        in_page = false;
        status = "Wait for the update to finish";
        return true;
    }

    // An action row does its job; Start Game returns 0 once the game may start. A setting does nothing.
    int activate(int item)
    {
        if (item == PLAY) return play();
        if (item == CHOOSE_ISO) chooseDisc();
        else if (item == BUILD) startBuild();
        else if (item == UPDATE) startUpdate(false);
        return -1;
    }

    static bool isButton(int item) { return item == UPDATE || item == CHOOSE_ISO || item == BUILD || item == PLAY; }

    std::string buildStatus() const
    {
        const size_t slash = game_source.find_last_of('/');
        return (game_source.empty() ? std::string("No disc chosen. ") : "Disc: " + game_source.substr(slash == std::string::npos ? 0 : slash + 1) + ". ")
            + (built ? "Built. Build again after an update" : "Not built yet: choose your Disc 2 ISO, then Build");
    }

    // Takes an ISO from the file dialog or a drop; anything else is refused.
    void useDisc(const std::string &path)
    {
        if (build != nullptr) { build_info = "Wait for the build to finish"; return; }
        SDL_PathInfo path_info;
        const bool iso = path.size() > 4 && SDL_strcasecmp(path.c_str() + path.size() - 4, ".iso") == 0;
        if (!iso || !SDL_GetPathInfo(path.c_str(), &path_info) || path_info.type != SDL_PATHTYPE_FILE) {
            build_info = "Choose your Disc 2 .iso image";
            menu = kGamePage;
            in_page = true;
            row = kDiscRow;
            return;
        }
        game_source = path;
        menu = kGamePage;
        in_page = true;
        row = kBuildRow;
        build_info = buildStatus();
    }

    // The dialog may call back from another thread, so the path goes through the event queue.
    static void SDLCALL discChosen(void *, const char *const *files, int)
    {
        if (files == nullptr || files[0] == nullptr) return;
        SDL_Event event{};
        event.type = SDL_EVENT_USER;
        event.user.data1 = SDL_strdup(files[0]);
        SDL_PushEvent(&event);
    }

    void chooseDisc()
    {
        static const SDL_DialogFileFilter iso[] = {{"Xbox disc image", "iso"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(discChosen, nullptr, window, iso, 2, nullptr, false);
    }

    static bool onSteamOS()
    {
        std::ifstream file("/etc/os-release");
        for (std::string line; std::getline(file, line);)
            if (line == "ID=steamos") return true;
        return false;
    }

    void startBuild()
    {
        if (build != nullptr) return;
        if (update != nullptr && installing) { build_info = "Wait for the update to finish"; return; }
        if (game_source.empty()) { build_info = "Choose your Disc 2 ISO first"; return; }
        SDL_CreateDirectory((root + "/private").c_str());
        // SteamOS has no compiler, so its script builds in a container. The child runs in its own session, so
        // stopping the build can signal the whole group; its output goes to the log, and it reads no input.
        const std::string script = root + (onSteamOS() ? "/build_steamdeck.sh" : "/build_game.sh");
        const std::string log = root + "/private/build.log";
        const char *shell = "export PYTHONUNBUFFERED=1 PYTHONIOENCODING=utf-8; log=$1; shift; "
                            "if command -v setsid >/dev/null; then exec setsid \"$@\" >\"$log\" 2>&1 </dev/null; fi; "
                            "exec \"$@\" >\"$log\" 2>&1 </dev/null";
        const char *args[] = {"/bin/sh", "-c", shell, "sh", log.c_str(), script.c_str(), game_source.c_str(), nullptr};
        build = SDL_CreateProcess(args, false);
        if (build == nullptr) { build_info = std::string("Cannot start the build: ") + SDL_GetError(); return; }
        build_pid = static_cast<int>(SDL_GetNumberProperty(SDL_GetProcessProperties(build), SDL_PROP_PROCESS_PID_NUMBER, 0));
        build_start = SDL_GetTicks();
        stop_armed = false;
        build_info = "Starting the build";
    }

    // The last non-empty line of the build log.
    std::string logTail() const
    {
        std::ifstream file(root + "/private/build.log", std::ios::binary | std::ios::ate);
        if (!file) return "";
        const std::streamoff size = file.tellg();
        file.seekg(std::max<std::streamoff>(0, size - 4096));
        std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        text.erase(text.find_last_not_of(" \n") + 1);
        text.erase(0, text.rfind('\n') + 1);
        return text;
    }

    // Shows the elapsed time and the build's latest line while it runs, and its outcome once it has exited.
    void pollBuild()
    {
        if (build == nullptr) return;
        int code = 0;
        if (!SDL_WaitProcess(build, false, &code)) {
            const Uint64 now = SDL_GetTicks();
            if (now >= next_log) {
                stop_armed = false;
                next_log = now + 500;
                const Uint64 seconds = (now - build_start) / 1000;
                char clock[16];
                std::snprintf(clock, sizeof clock, "%02d:%02d ", static_cast<int>(seconds / 60), static_cast<int>(seconds % 60));
                // The information area's two lines fit about 72 characters.
                build_info = (std::string("BUILDING ") + clock + logTail()).substr(0, 75);
            }
            return;
        }
        SDL_DestroyProcess(build);
        build = nullptr;
        built = SDL_GetPathInfo((root + "/build/recomp-program/recomp_program_runner").c_str(), nullptr);
        menu = kGamePage;
        in_page = true;
        if (code == 0) {
            build_info = "Build complete. Choose Start Game to play";
            row = kPlayRow;
        } else {
            // The build prints its own "Build failed: reason"; the two lines have no room to repeat it.
            const std::string tail = logTail();
            build_info = (tail.rfind("Build failed", 0) == 0 ? tail : "Build failed: " + tail).substr(0, 75);
            row = kBuildRow;
        }
    }

    // Signals the build's whole process group, then collects it.
    void stopBuild()
    {
        if (build == nullptr) return;
        if (build_pid > 0) {
            // The kill program, not the shell's: dash's builtin rejects "--".
            const std::string group = "-" + std::to_string(build_pid);
            const char *kill[] = {"kill", "-s", "TERM", "--", group.c_str(), nullptr};
            if (SDL_Process *signal = SDL_CreateProcess(kill, false)) {
                SDL_WaitProcess(signal, true, nullptr);
                SDL_DestroyProcess(signal);
            }
        }
        SDL_KillProcess(build, true);
        SDL_WaitProcess(build, true, nullptr);
        SDL_DestroyProcess(build);
        build = nullptr;
    }

    // Leaving needs an install to finish, and a running build to be stopped, which takes a second request.
    int exitRequested()
    {
        if (waitForInstall()) return -1;
        if (build != nullptr && !stop_armed) {
            stop_armed = true;
            next_log = SDL_GetTicks() + 3000; // keeps the question up until the confirmation expires
            menu = kGamePage;
            in_page = false;
            build_info = "A build is running. Exit again to stop it";
            return -1;
        }
        return 1;
    }

    // Start Game from the GAME page or the Start button: 0 once the game is built, else -1 and the row to fix it.
    int play()
    {
        if (waitForInstall()) return -1;
        if (build != nullptr || !built) {
            menu = kGamePage;
            in_page = true;
            row = build != nullptr || !game_source.empty() ? kBuildRow : kDiscRow;
            build_info = build != nullptr ? "Wait for the build to finish" : "Build the game first";
            return -1;
        }
        return 0;
    }

    // A on the tab strip enters the page; on a row it runs the row's action.
    int accept()
    {
        if (!in_page) { in_page = true; row = 0; return -1; }
        return activate(rows()[row]);
    }

    // B leaves the rows for the tab strip, and the tab strip closes the launcher.
    int back()
    {
        if (!in_page) return exitRequested();
        in_page = false;
        return -1;
    }

    // Up and down move between rows, up from the first one to the tab strip; left and right change a
    // value, or the page on the tab strip.
    void move(int dx, int dy)
    {
        if (!in_page) {
            if (dx != 0) menu = std::clamp(menu + dx, 0, kPageCount - 1);
            else if (dy > 0) accept();
        } else if (dx != 0) {
            if (!isButton(rows()[row])) change(rows()[row], dx);
        } else if (dy < 0 && row == 0) {
            in_page = false;
        } else {
            row = std::clamp(row + dy, 0, static_cast<int>(rows().size()) - 1);
        }
    }

    // LB/RB, Q/E and Page Up/Down switch pages from anywhere; in the rows the cursor starts at the top.
    void switchPage(int step)
    {
        const int m = std::clamp(menu + step, 0, kPageCount - 1);
        if (m != menu) { menu = m; row = 0; }
    }

    void fill(const SDL_FRect &rect, Color color, Uint8 alpha = 255)
    {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, alpha);
        SDL_RenderFillRect(renderer, &rect);
    }

    // scale 2 draws 16x16 glyphs; coordinates stay on the 640x400 canvas.
    void text(float x, float y, const std::string &value, Color color, float scale = 1)
    {
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
        SDL_RenderDebugText(renderer, x / scale, y / scale, value.c_str());
        SDL_SetRenderScale(renderer, 1, 1);
    }

    // Silver text over a dark drop shadow, as on the title logo.
    void chrome(float x, float y, const std::string &value, float scale)
    {
        text(x + scale, y + scale, value, kDark, scale);
        text(x, y, value, {214, 216, 217}, scale);
    }

    // A top-bar prompt's action name, right of its plate.
    void promptLabel(float &x, float y, const char *action)
    {
        x -= SDL_strlen(action) * 8;
        text(x, y, action, kMuted);
    }

    // The plate a prompt's button sits in: its gamepad glyphs when the art loaded, else the
    // debug font's spelled-out name.
    void promptPlate(float &x, float y, const char *name, SDL_Texture *first, SDL_Texture *second = nullptr)
    {
        if (first == nullptr) {
            const float w = static_cast<float>(SDL_strlen(name) * 8 + 8);
            x -= w + 6;
            fill({x, y - 2, w, 12}, {242, 227, 207});
            text(x + 4, y, name, kDark);
            return;
        }
        const float w = second == nullptr ? 20.0f : 34.0f;
        x -= w + 6;
        fill({x, y - 4, w, 16}, {242, 227, 207});
        SDL_FRect at{x + 4, y - 2, 12, 12};
        SDL_RenderTexture(renderer, first, nullptr, &at);
        if (second != nullptr) {
            at.x = x + 18;
            SDL_RenderTexture(renderer, second, nullptr, &at);
        }
    }

    void draw()
    {
        SDL_SetRenderDrawColor(renderer, 74, 60, 51, 255);
        SDL_RenderClear(renderer);
        if (background != nullptr) SDL_RenderTexture(renderer, background, nullptr, nullptr);
        // Letterbox: a solid top bar, and the backdrop fading into the same dark under the settings.
        fill({0, 0, kWidth, kTopH}, kBar);
        for (float y = kFadeY; y < kHeight; y += 2) {
            const float t = (y - kFadeY) / (kHeight - kFadeY);
            fill({0, y, kWidth, 2}, kBar, static_cast<Uint8>(t < .26f ? t / .26f * 230 : 230 + (t - .26f) / .74f * 25));
        }

        // Top bar: title and build state on the left, button prompts on the right. The short title
        // leaves the prompts room beside "NOT BUILT YET - UPDATE". A PlayStation pad shows Cross,
        // Circle and L1/R1 art; without the files those prompts keep their names.
        text(kSide, (kTopH - 8) / 2, std::string("DOA2U - ") + (built ? "READY TO PLAY" : "NOT BUILT YET")
            + (pending.empty() ? "" : " - UPDATE"), kMuted);
        const bool ps = playstationPad();
        const bool shoulders = ps && l1 != nullptr && r1 != nullptr;
        const float y = (kTopH - 8) / 2;
        float x = kWidth - kSide;
        promptLabel(x, y, "BACK");
        promptPlate(x, y, ps ? "CIRCLE" : "B", ps ? circle : nullptr);
        x -= 16;
        promptLabel(x, y, "SELECT");
        promptPlate(x, y, ps ? "CROSS" : "A", ps ? cross : nullptr);
        x -= 16;
        promptLabel(x, y, "CHANGE");
        promptPlate(x, y, "<>", nullptr);
        x -= 16;
        promptLabel(x, y, "PAGE");
        promptPlate(x, y, ps ? "L1/R1" : "LB/RB", shoulders ? l1 : nullptr, shoulders ? r1 : nullptr);

        // Logo in the sky at the right, as in launcher.cs.
        chrome(kWidth - kSide - 32 - 14 - 13 * 16, 56, "DEAD OR ALIVE", 2);
        chrome(kWidth - kSide - 32, 40, "2", 4);
        chrome(kWidth - kSide - 8 * 16, 82, "ULTIMATE", 2);

        // The panel on the left: page tabs, the chosen page's rows, then their information.
        fill({0, kTopH, kPanelW, kHeight - kTopH}, kBar, 200);
        // The current tab is underlined in sunset gold, and highlighted while the tab strip has the focus.
        for (int m = 0; m < kPageCount; ++m) {
            const bool current = m == menu;
            if (current && !in_page) fill({tabX(m) - 4, kTabY - 6, tabWidth(m) + 8, 22}, kGold, 45);
            text(tabX(m), kTabY, kPages[m].name, !current ? Color{169, 156, 144} : in_page ? Color{255, 243, 228} : Color{255, 255, 255});
            if (current) fill({tabX(m), kTabY + 12, tabWidth(m), 2}, kGold);
        }
        fill({kPad, kRowY - 8, kPanelW - 2 * kPad, 1}, kWhite, 36);

        // One row per setting: its label on the left, "< VALUE >" or an action's verb on the right.
        const Page &page = kPages[menu];
        const float right = kPanelW - kPad;
        for (size_t i = 0; i < page.rows.size(); ++i) {
            const int item = page.rows[i];
            const float y = kRowY + i * kRowH;
            const bool selected = in_page && static_cast<int>(i) == row;
            if (selected) fill({kPad - 6, y, kPanelW - 2 * kPad + 12, kRowH}, kGold, 45);
            text(kPad, y + 8, kRows[item].label, selected ? kWhite : Color{198, 182, 166});
            const std::string value = valueText(item);
            if (isButton(item)) {
                const float w = value.size() * 8 + 12.f;
                fill({right - w, y + 5, w, 14}, selected ? Color{242, 227, 207} : kSand, selected ? 255 : 40);
                text(right - w + 6, y + 8, value, selected ? kDark : kSand);
                continue;
            }
            // The arrows show which way the value can still step.
            const float left = valueLeft(item);
            const Color arrow = selected ? kSand : Color{180, 150, 112};
            if (choice[item] > 0) text(left, y + 8, "<", arrow);
            text(left + 12, y + 4, value, selected ? kWhite : Color{232, 222, 212}, 2);
            if (choice[item] < kRows[item].count - 1) text(right - 8, y + 8, ">", arrow);
        }

        // The GAME page's notice, else what the focused row or page does.
        fill({kPad, kInfoY - 10, kPanelW - 2 * kPad, 1}, kWhite, 36);
        const std::string info = menu == kGamePage && !build_info.empty() ? build_info
            : in_page ? rowInfo(page.rows[row])
            : menu == kUpdatePage && !status.empty() ? status : page.info;
        const std::vector<std::string> lines = wrap(info);
        for (size_t i = 0; i < lines.size(); ++i) text(kPad, kInfoY + i * 12, lines[i], {216, 202, 188});
        SDL_RenderPresent(renderer);
    }
};

// Direction from every open gamepad's d-pad and left stick.
void heldDirection(int &dx, int &dy)
{
    dx = dy = 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) continue;
        const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -20000) dy = -1;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 20000) dy = 1;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -20000) dx = -1;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 20000) dx = 1;
    }
    SDL_free(ids);
}

} // namespace

int main(int argc, char **argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string settings_path = root + "/private/launcher.json";
    Settings settings = loadSettings(settings_path);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (settings.height == 0) {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        settings.height = mode != nullptr ? mode->h : 720;
    }

    // Steam sets these in Game Mode and on the Deck, where a window would only
    // be letterboxed by gamescope anyway.
    const bool fullscreen = SDL_getenv("SteamTenfoot") != nullptr || SDL_getenv("SteamDeck") != nullptr;
    SDL_Window *window = nullptr;
    Launcher launcher;
    launcher.built = SDL_GetPathInfo((root + "/build/recomp-program/recomp_program_runner").c_str(), nullptr);
    launcher.root = root;
    launcher.game_source = settings.game_source;
    if (!SDL_CreateWindowAndRenderer("Dead or Alive 2 Ultimate", kWidth * 2, kHeight * 2,
            fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE, &window, &launcher.renderer)) {
        std::fprintf(stderr, "launcher: cannot open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    launcher.window = window;
    // The window icon, when private/launcher-icon.png is present (fan art, so it stays untracked).
    if (SDL_Surface *icon = SDL_LoadPNG((root + "/private/launcher-icon.png").c_str())) {
        SDL_SetWindowIcon(window, icon);
        SDL_DestroySurface(icon);
    }
    SDL_SetRenderLogicalPresentation(launcher.renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderVSync(launcher.renderer, 1);
    SDL_SetRenderDrawBlendMode(launcher.renderer, SDL_BLENDMODE_BLEND);
    // A private/launcher-background.png replaces the generated backdrop; without either the launcher works on a plain fill.
    for (const char *path : {"/private/launcher-background.png", "/tools/launcher-background.png"}) {
        if (SDL_Surface *picture = SDL_LoadPNG((root + path).c_str())) {
            launcher.background = SDL_CreateTextureFromSurface(launcher.renderer, picture);
            SDL_DestroySurface(picture);
            break;
        }
    }
    // The PlayStation prompts' art, beside the launcher as in a release.
    launcher.cross = loadGlyph(launcher.renderer, root, "playstation_button_color_cross");
    launcher.circle = loadGlyph(launcher.renderer, root, "playstation_button_color_circle");
    launcher.l1 = loadGlyph(launcher.renderer, root, "playstation_trigger_l1");
    launcher.r1 = loadGlyph(launcher.renderer, root, "playstation_trigger_r1");

    const int nearest = static_cast<int>(std::upper_bound(std::begin(kHeights), std::end(kHeights),
        settings.height) - std::begin(kHeights)) - 1;
    launcher.choice[RESOLUTION] = std::max(0, nearest);
    // Windows saves up to 32x; SDL_GPU stops at 8x.
    launcher.choice[MSAA] = static_cast<int>(std::upper_bound(std::begin(kSamples), std::end(kSamples),
        std::max(settings.msaa, 1)) - std::begin(kSamples)) - 1;
    launcher.choice[SMAA] = settings.smaa ? 1 : 0;
    for (int i = 0; i < static_cast<int>(std::size(kSharpens)); ++i) {
        if (settings.sharpen == kSharpens[i]) launcher.choice[SHARPEN] = i;
    }
    launcher.choice[AO] = settings.ao ? 1 : 0;
    launcher.choice[WIDESCREEN] = 3; // a saved 21:9 "W:H"
    for (int i = 0; i < static_cast<int>(std::size(kScreens)); ++i) {
        if (settings.screen == kScreens[i]) launcher.choice[WIDESCREEN] = i;
    }
    launcher.choice[SURROUND] = settings.surround ? 1 : 0;
    launcher.choice[SKIP_INTRO] = settings.skip_intro ? 1 : 0;
    launcher.choice[CHECK_UPDATES] = settings.check_updates ? 1 : 0;
    if (settings.check_updates) launcher.startUpdate(true);
    launcher.choice[UNLOCKS] = settings.unlock_all ? 0 : 1;
    launcher.choice[CPU_COSTUMES] = settings.random_cpu_costumes ? 1 : 0;
    launcher.choice[FREE_CAMERA] = settings.free_camera ? 1 : 0;
    launcher.choice[DEDICATION] = settings.dedication ? 0 : 1;
    for (int i = 0; i < static_cast<int>(std::size(kDumps)); ++i) {
        if (settings.texture_dump == kDumps[i]) launcher.choice[TEXTURE_DUMP] = i;
    }
    launcher.row = launcher.built ? kPlayRow : kDiscRow;
    // A plain exit keeps an untouched 16x/32x MSAA or 21:9 shape; play saves what the rows show.
    const Settings loaded = settings;
    const int shown_msaa = launcher.choice[MSAA], shown_screen = launcher.choice[WIDESCREEN];

    int result = -1;
    int held_dx = 0, held_dy = 0;
    Uint64 next_repeat = 0;
    while (result < 0) {
        SDL_Event event;
        while (result < 0 && SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(launcher.renderer, &event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                result = launcher.exitRequested();
                break;
            case SDL_EVENT_USER: // a chosen disc from the file dialog
                launcher.useDisc(static_cast<char *>(event.user.data1));
                SDL_free(event.user.data1);
                break;
            case SDL_EVENT_DROP_FILE:
                launcher.useDisc(event.drop.data);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                launcher.clearNotice();
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) result = launcher.accept();
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_START) result = launcher.play();
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST || event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
                    result = launcher.back();
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) launcher.switchPage(-1);
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) launcher.switchPage(1);
                break;
            case SDL_EVENT_KEY_DOWN:
                launcher.clearNotice();
                switch (event.key.key) {
                case SDLK_UP: launcher.move(0, -1); break;
                case SDLK_DOWN: launcher.move(0, 1); break;
                case SDLK_LEFT: launcher.move(-1, 0); break;
                case SDLK_RIGHT: launcher.move(1, 0); break;
                case SDLK_Q:
                case SDLK_PAGEUP: launcher.switchPage(-1); break;
                case SDLK_E:
                case SDLK_PAGEDOWN: launcher.switchPage(1); break;
                case SDLK_RETURN:
                case SDLK_SPACE: result = launcher.accept(); break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE: result = launcher.back(); break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                // Hovering a tab shows its page with the tab strip focused; hovering a row focuses it.
                const int m = hitTab(event.motion.x, event.motion.y);
                const int i = hitRow(event.motion.x, event.motion.y, launcher.rows().size());
                if (m >= 0) {
                    launcher.menu = m;
                    launcher.in_page = false;
                } else if (i >= 0) {
                    launcher.in_page = true;
                    launcher.row = i;
                }
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                // Touch arrives as mouse input. A tap on an action row runs it; on a setting, the left
                // half of its value steps down and the right half up.
                launcher.clearNotice();
                if (event.button.button == SDL_BUTTON_RIGHT) { result = launcher.back(); break; }
                const float x = event.button.x, y = event.button.y;
                const int m = hitTab(x, y);
                if (m >= 0) {
                    launcher.menu = m;
                    launcher.in_page = false;
                    break;
                }
                const int i = hitRow(x, y, launcher.rows().size());
                if (i < 0) break;
                launcher.in_page = true;
                launcher.row = i;
                const int item = launcher.rows()[i];
                const float left = launcher.valueLeft(item);
                if (Launcher::isButton(item)) result = launcher.activate(item);
                else if (x >= left) launcher.change(item, x < (left + kPanelW - kPad) / 2 ? -1 : 1);
                break;
            }
            default:
                break;
            }
        }

        // D-pad and stick: one step on press, then repeat while held.
        int dx, dy;
        heldDirection(dx, dy);
        if (dx != 0) dy = 0; // a diagonal changes a value without also moving the row
        const Uint64 now = SDL_GetTicks();
        if (dx != held_dx || dy != held_dy) {
            held_dx = dx;
            held_dy = dy;
            next_repeat = now + 350;
            if (dx || dy) {
                launcher.clearNotice();
                launcher.move(dx, dy);
            }
        } else if ((dx || dy) && now >= next_repeat) {
            next_repeat = now + 120;
            launcher.move(dx, dy);
        }
        launcher.pollUpdate();
        launcher.pollBuild();
        if (result < 0) launcher.draw();
    }

    // Settings hold after a plain exit too, so the game plays with them later.
    {
        settings.height = kHeights[launcher.choice[RESOLUTION]];
        settings.msaa = kSamples[launcher.choice[MSAA]];
        settings.smaa = launcher.choice[SMAA] != 0;
        settings.sharpen = kSharpens[launcher.choice[SHARPEN]];
        settings.ao = launcher.choice[AO] != 0;
        settings.screen = kScreens[launcher.choice[WIDESCREEN]];
        if (launcher.choice[WIDESCREEN] == 3) {
            // The primary screen's own shape when it is a 21:9 display (wider than 16:9,
            // narrower than 3:1, so not 32:9), else 21:9 as 2560x1080 (64:27).
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
            settings.screen = mode != nullptr && mode->w * 9 > mode->h * 16 && mode->w < mode->h * 3
                ? std::to_string(mode->w) + ":" + std::to_string(mode->h) : "64:27";
        }
        settings.surround = launcher.choice[SURROUND] != 0;
        settings.skip_intro = launcher.choice[SKIP_INTRO] != 0;
        settings.check_updates = launcher.choice[CHECK_UPDATES] != 0;
        settings.unlock_all = launcher.choice[UNLOCKS] == 0;
        settings.random_cpu_costumes = launcher.choice[CPU_COSTUMES] != 0;
        settings.free_camera = launcher.choice[FREE_CAMERA] != 0;
        settings.dedication = launcher.choice[DEDICATION] == 0;
        settings.texture_dump = kDumps[launcher.choice[TEXTURE_DUMP]];
        settings.game_source = launcher.game_source;
        if (result != 0) {
            if (launcher.choice[MSAA] == shown_msaa) settings.msaa = loaded.msaa;
            if (launcher.choice[WIDESCREEN] == shown_screen) settings.screen = loaded.screen;
        }
        if (!saveSettings(settings_path, settings)) {
            std::fprintf(stderr, "launcher: cannot write %s\n", settings_path.c_str());
            if (result == 0) result = 1;
        }
    }
    SDL_DestroyTexture(launcher.background);
    for (SDL_Texture *glyph : {launcher.cross, launcher.circle, launcher.l1, launcher.r1})
        if (glyph != nullptr) SDL_DestroyTexture(glyph);
    launcher.stopBuild(); // a second exit request while a build ran
    // Only a check can still be running here; stop it rather than leave it behind.
    if (launcher.update != nullptr) {
        SDL_KillProcess(launcher.update, true);
        SDL_WaitProcess(launcher.update, true, nullptr);
        SDL_DestroyProcess(launcher.update);
    }
    SDL_DestroyRenderer(launcher.renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
