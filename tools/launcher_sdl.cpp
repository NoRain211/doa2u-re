// SPDX-License-Identifier: GPL-3.0-or-later
// Linux launcher: pick resolution, anti-aliasing, widescreen, 5.1 surround and
// the intro skip, remember them in private/launcher.json, then exit 0 so
// run_game.py starts the game. Mirrors tools/launcher.cs without the build page
// (./build_game.sh builds) and with MSAA up to SDL_GPU's 8x: the backdrop
// between letterbox bars, the page tabs and the chosen page's settings in the
// bottom one. Adapted from committed DOA3 tools/launcher_sdl.cpp at ce393e4.
// Gamepad, keyboard, mouse and touch all work, so it runs from Steam Game Mode.
// Exits 1 when closed without Play.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Everything is laid out on a 640x400 canvas and scaled by whole numbers,
// so SDL's 8x8 debug font stays sharp: 2x on the Steam Deck's 1280x800.
constexpr int kWidth = 640, kHeight = 400;
// Horizon Bar layout, as in tools/launcher.cs.
constexpr float kTopH = 30, kSide = 20, kFadeY = 230;
constexpr float kInfoY = 262, kLabelY = 296, kValueY = 310, kValueH = 16, kTabY = 362, kTabH = 12;

const int kHeights[] = {480, 720, 1080, 1440, 2160};
const int kSamples[] = {1, 2, 4, 8};

enum Item { RESOLUTION, MSAA, SMAA, WIDESCREEN, SURROUND, SKIP_INTRO, UNLOCKS, ITEM_COUNT };
const char *kLabels[] = {"RESOLUTION", "MSAA", "SMAA", "SCREEN", "OUTPUT", "INTRO MOVIES", "UNLOCKS"};

struct Page { const char *name, *info; std::vector<int> rows; };
const Page kPages[] = {
    {"VIDEO SETTINGS", "Sets resolution, anti-aliasing and screen shape", {RESOLUTION, MSAA, SMAA, WIDESCREEN}},
    {"AUDIO SETTINGS", "Chooses stereo or the game's own 5.1 surround mix", {SURROUND}},
    {"MOVIES", "Plays or skips the logo and opening movies at boot", {SKIP_INTRO}},
    {"GAMEPLAY", "Offers every character and costume, or leaves them to earn through play", {UNLOCKS}},
    {"START GAME", "Saves these settings and starts the game", {}},
    {"EXIT", "Closes the launcher", {}},
};
constexpr int kPageCount = static_cast<int>(std::size(kPages)), kPlayPage = 4, kExitPage = 5;

// launcher.cs's defaults: the screen height, the most MSAA, SMAA and widescreen on.
struct Settings {
    int height = 0;
    int msaa = 8;
    bool smaa = true;
    bool widescreen = true;
    bool surround = false;
    bool skip_intro = false;
    bool unlock_all = true;
};

struct Color { Uint8 r, g, b; };
constexpr Color kDark{42, 29, 21}, kWhite{243, 238, 232}, kSand{241, 197, 143};
constexpr Color kMuted{191, 178, 165}, kGold{232, 191, 138}, kBar{13, 10, 9};

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

Settings loadSettings(const std::string &path)
{
    Settings settings;
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string value;
    if (jsonValue(text, "height", value)) settings.height = std::atoi(value.c_str());
    if (jsonValue(text, "msaa", value)) settings.msaa = std::atoi(value.c_str());
    if (jsonValue(text, "smaa", value)) settings.smaa = value == "true";
    if (jsonValue(text, "widescreen", value)) settings.widescreen = value == "true";
    if (jsonValue(text, "surround", value)) settings.surround = value == "true";
    if (jsonValue(text, "skipIntro", value)) settings.skip_intro = value == "true";
    if (jsonValue(text, "unlockAll", value)) settings.unlock_all = value == "true";
    return settings;
}

bool saveSettings(const std::string &path, const Settings &settings)
{
    std::ofstream file(path, std::ios::trunc);
    file << "{\n  \"height\": " << settings.height << ",\n  \"msaa\": " << settings.msaa
         << ",\n  \"smaa\": " << (settings.smaa ? "true" : "false")
         << ",\n  \"widescreen\": " << (settings.widescreen ? "true" : "false")
         << ",\n  \"skipIntro\": " << (settings.skip_intro ? "true" : "false")
         << ",\n  \"unlockAll\": " << (settings.unlock_all ? "true" : "false")
         << ",\n  \"surround\": " << (settings.surround ? "true" : "false") << "\n}\n";
    return static_cast<bool>(file);
}

float tabWidth(int m) { return static_cast<float>(SDL_strlen(kPages[m].name) * 8); }
float tabX(int m)
{
    float x = kSide;
    for (int i = 0; i < m; ++i) x += tabWidth(i) + 16;
    return x;
}
float columnWidth(size_t rows) { return (kWidth - 2 * kSide) / static_cast<float>(std::max<size_t>(rows, 1)); }

int hitTab(float x, float y)
{
    if (y < kTabY - 4 || y >= kTabY + kTabH + 4) return -1;
    for (int m = 0; m < kPageCount; ++m)
        if (x >= tabX(m) - 4 && x < tabX(m) + tabWidth(m) + 4) return m;
    return -1;
}

int hitColumn(float x, float y, size_t rows)
{
    if (y < kLabelY - 6 || y >= kValueY + kValueH + 4 || x < kSide) return -1;
    const int i = static_cast<int>((x - kSide) / columnWidth(rows));
    return i < static_cast<int>(rows) ? i : -1;
}

struct Launcher {
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *background = nullptr;
    int choice[ITEM_COUNT] = {};
    // The menu cursor picks a page tab; inside a page the row cursor picks a setting column.
    int menu = kPlayPage, row = 0;
    bool in_page = false;
    bool built = false;

    const std::vector<int> &rows() const { return kPages[menu].rows; }

    std::string valueText(int item) const
    {
        switch (item) {
        case RESOLUTION: return std::to_string(kHeights[choice[item]]) + "P";
        case MSAA: return kSamples[choice[item]] == 1 ? "OFF" : std::to_string(kSamples[choice[item]]) + "X";
        case WIDESCREEN: return choice[item] ? "16:9" : "4:3";
        case SURROUND: return choice[item] ? "5.1" : "STEREO";
        case SKIP_INTRO: return choice[item] ? "SKIP" : "PLAY";
        case UNLOCKS: return choice[item] ? "EARN" : "EVERYTHING";
        default: return choice[item] ? "ON" : "OFF";
        }
    }

    void change(int item, int step)
    {
        static const int counts[] = {static_cast<int>(std::size(kHeights)), static_cast<int>(std::size(kSamples)), 2, 2, 2, 2, 2};
        choice[item] = (choice[item] + counts[item] + step) % counts[item];
    }

    // Returns the exit code once the player picks Start Game (0) or Exit (1), else -1.
    int accept()
    {
        if (in_page) { change(rows()[row], 1); return -1; }
        if (menu == kPlayPage) return 0;
        if (menu == kExitPage) return 1;
        in_page = true;
        row = 0;
        return -1;
    }

    int back()
    {
        if (!in_page) return 1;
        in_page = false;
        return -1;
    }

    // Left and right move along the tabs or settings; up and down change a value, and down enters a tab.
    void move(int dx, int dy)
    {
        if (dx != 0 && in_page) row = std::clamp(row + dx, 0, static_cast<int>(rows().size()) - 1);
        else if (dx != 0) menu = std::clamp(menu + dx, 0, kPageCount - 1);
        else if (in_page) change(rows()[row], -dy);
        else if (dy > 0 && !rows().empty()) accept();
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

        // Top bar: title and build state on the left, button prompts on the right.
        text(kSide, (kTopH - 8) / 2, std::string("DOA2 ULTIMATE - ") + (built ? "READY TO PLAY" : "NOT BUILT YET"), kMuted);
        float x = kWidth - kSide;
        for (const auto &hint : {std::pair<const char *, const char *>{"^v", "CHANGE"}, {"B", "BACK"}, {"A", "SELECT"}}) {
            x -= SDL_strlen(hint.second) * 8;
            text(x, (kTopH - 8) / 2, hint.second, kMuted);
            const float w = static_cast<float>(SDL_strlen(hint.first) * 8 + 8);
            x -= w + 6;
            fill({x, (kTopH - 12) / 2, w, 12}, {242, 227, 207});
            text(x + 4, (kTopH - 8) / 2, hint.first, kDark);
            x -= 16;
        }

        // Logo in the sky at the right, as in launcher.cs.
        chrome(kWidth - kSide - 32 - 14 - 13 * 16, 56, "DEAD OR ALIVE", 2);
        chrome(kWidth - kSide - 32, 40, "2", 4);
        chrome(kWidth - kSide - 8 * 16, 82, "ULTIMATE", 2);

        // The chosen page in the bottom bar: its information, then its settings side by side.
        const Page &page = kPages[menu];
        text(kSide, kInfoY, page.info, {216, 202, 188});
        if (menu == kPlayPage) chrome(kSide, kValueY, "PRESS START BUTTON", 2);
        const float column = columnWidth(page.rows.size());
        for (size_t i = 0; i < page.rows.size(); ++i) {
            const int item = page.rows[i];
            const float left = kSide + i * column;
            const bool selected = in_page && static_cast<int>(i) == row;
            if (selected) fill({left, kLabelY - 6, column, kValueY + kValueH + 10 - kLabelY}, kGold, 45);
            if (i > 0) fill({left, kLabelY - 2, 1, kValueY + kValueH + 2 - kLabelY}, kWhite, 36);
            text(left + 8, kLabelY, kLabels[item], selected ? kWhite : Color{198, 182, 166});
            const std::string value = valueText(item);
            const Color arrow = selected ? kSand : Color{180, 150, 112};
            text(left + 8, kValueY + 4, "<", arrow);
            text(left + 20, kValueY, value, selected ? kWhite : Color{232, 222, 212}, 2);
            text(left + 24 + value.size() * 16, kValueY + 4, ">", arrow);
        }

        // Page tabs along the bottom, the current one underlined in sunset gold.
        fill({kSide, kTabY - 10, kWidth - 2 * kSide, 1}, kWhite, 36);
        for (int m = 0; m < kPageCount; ++m) {
            text(tabX(m), kTabY + 2, kPages[m].name, m == menu ? Color{255, 243, 228} : Color{169, 156, 144});
            if (m == menu) fill({tabX(m), kTabY + kTabH + 2, tabWidth(m), 2}, kGold);
        }
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
    if (!SDL_CreateWindowAndRenderer("Dead or Alive 2 Ultimate", kWidth * 2, kHeight * 2,
            fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE, &window, &launcher.renderer)) {
        std::fprintf(stderr, "launcher: cannot open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
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

    const int nearest = static_cast<int>(std::upper_bound(std::begin(kHeights), std::end(kHeights),
        settings.height) - std::begin(kHeights)) - 1;
    launcher.choice[RESOLUTION] = std::max(0, nearest);
    // Windows saves up to 32x; SDL_GPU stops at 8x.
    launcher.choice[MSAA] = static_cast<int>(std::upper_bound(std::begin(kSamples), std::end(kSamples),
        std::max(settings.msaa, 1)) - std::begin(kSamples)) - 1;
    launcher.choice[SMAA] = settings.smaa ? 1 : 0;
    launcher.choice[WIDESCREEN] = settings.widescreen ? 1 : 0;
    launcher.choice[SURROUND] = settings.surround ? 1 : 0;
    launcher.choice[SKIP_INTRO] = settings.skip_intro ? 1 : 0;
    launcher.choice[UNLOCKS] = settings.unlock_all ? 0 : 1;

    int result = -1;
    int held_dx = 0, held_dy = 0;
    Uint64 next_repeat = 0;
    // A held Down key that entered a page waits for release before it changes a value.
    bool down_entered = false;
    bool pad_down_entered = false;
    while (result < 0) {
        SDL_Event event;
        while (result < 0 && SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(launcher.renderer, &event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                result = 1;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || event.gbutton.button == SDL_GAMEPAD_BUTTON_START)
                    result = launcher.accept();
                else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST || event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
                    result = launcher.back();
                break;
            case SDL_EVENT_KEY_DOWN:
                switch (event.key.key) {
                case SDLK_UP: launcher.move(0, -1); break;
                case SDLK_DOWN:
                    if (!event.key.repeat) down_entered = !launcher.in_page;
                    if (!(event.key.repeat && down_entered)) launcher.move(0, 1);
                    break;
                case SDLK_LEFT: launcher.move(-1, 0); break;
                case SDLK_RIGHT: launcher.move(1, 0); break;
                case SDLK_RETURN:
                case SDLK_SPACE: result = launcher.accept(); break;
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE: result = launcher.back(); break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                const int m = hitTab(event.motion.x, event.motion.y);
                const int i = hitColumn(event.motion.x, event.motion.y, launcher.rows().size());
                if (!launcher.in_page && m >= 0) launcher.menu = m;
                if (launcher.in_page && i >= 0) launcher.row = i;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                // Touch arrives as mouse input. The first tap on a setting enters its page;
                // then values step down on its left half and up on its right half.
                if (event.button.button == SDL_BUTTON_RIGHT) { result = launcher.back(); break; }
                const float x = event.button.x, y = event.button.y;
                const int m = hitTab(x, y);
                if (m >= 0) {
                    launcher.menu = m;
                    launcher.in_page = false;
                    result = launcher.accept();
                    break;
                }
                const int i = hitColumn(x, y, launcher.rows().size());
                if (i < 0) break;
                const bool entering = !launcher.in_page;
                launcher.in_page = true;
                launcher.row = i;
                const float column = columnWidth(launcher.rows().size());
                if (!entering) launcher.change(launcher.rows()[i], x < kSide + (i + .5f) * column ? -1 : 1);
                break;
            }
            default:
                break;
            }
        }

        // D-pad and stick: one step on press, then repeat while held.
        int dx, dy;
        heldDirection(dx, dy);
        // A pad Down that entered a page stays blocked until Down itself is released.
        if (dy != 1) pad_down_entered = false;
        if (dx != 0 || pad_down_entered) dy = 0;  // a diagonal moves a column without also changing a value
        const Uint64 now = SDL_GetTicks();
        if (dx != held_dx || dy != held_dy) {
            held_dx = dx;
            held_dy = dy;
            next_repeat = now + 350;
            const bool was_in_page = launcher.in_page;
            if (dx || dy) launcher.move(dx, dy);
            if (!was_in_page && launcher.in_page) pad_down_entered = true;
        } else if ((dx || dy) && now >= next_repeat) {
            next_repeat = now + 120;
            launcher.move(dx, dy);
        }
        if (result < 0) launcher.draw();
    }

    if (result == 0) {
        settings.height = kHeights[launcher.choice[RESOLUTION]];
        settings.msaa = kSamples[launcher.choice[MSAA]];
        settings.smaa = launcher.choice[SMAA] != 0;
        settings.widescreen = launcher.choice[WIDESCREEN] != 0;
        settings.surround = launcher.choice[SURROUND] != 0;
        settings.skip_intro = launcher.choice[SKIP_INTRO] != 0;
        settings.unlock_all = launcher.choice[UNLOCKS] == 0;
        if (!saveSettings(settings_path, settings)) {
            std::fprintf(stderr, "launcher: cannot write %s\n", settings_path.c_str());
            result = 1;
        }
    }
    SDL_DestroyTexture(launcher.background);
    SDL_DestroyRenderer(launcher.renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
