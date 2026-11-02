/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_menu.h"

#include "d3d8_adapter.h"
#include "input_pulse_source.h"
#include "netplay.h"
#include "overlay.h"
#include "rollback.h"
#include "runtime.h"
#include "stop_report.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

enum Key : std::uint16_t { Up = 1u, Down = 2u, Left = 4u, Right = 8u, A = 0x10u, B = 0x20u, X = 0x40u, Y = 0x80u };
enum class Screen { Closed, Versus, Settings, Join, Room };

constexpr std::uint32_t White = 0xffffffffu, Gray = 0xffa8a8b0u, Dim = 0xff686870u, Red = 0xffd8283cu,
    Green = 0xff48d070u, Yellow = 0xffffd040u, Panel = 0xe8181820u, Backdrop = 0xd8000000u;
constexpr std::uint32_t DefaultPort = 47701u;
constexpr std::size_t MaxAddress = 21u; // 255.255.255.255:65535
const std::string AddressCharacters = "0123456789.:";
const char *const DisplayNames[3] = {"off", "ping", "full"};

struct Menu {
    bool installed = false;
    std::filesystem::path storage;
    std::string digest;
    std::function<std::string()> ownName;
    float aspect = 4.0f / 3.0f;
    std::mutex lock; // the game may sample pads on another guest thread than it presents from
    Screen screen = Screen::Closed;
    std::uint16_t held[RECOMP_INPUT_PORT_COUNT]{};
    std::uint16_t pressed = 0u;
    unsigned repeat = 0u, frame = 0u;
    bool swallow = false; // after closing, the game waits until every button is up
    int injectA = 0;      // polls of A the game still gets, so LOCAL enters Versus
    std::uint32_t openedBy = 0u;
    int cursor = 0;
    std::string message;
    std::uint32_t delay = 2u, port = DefaultPort;
    int display = 1; // the session's network display: off, ping, full
    std::string recent;
    std::string address;
    std::size_t caret = 0u;
    bool hosting = false, ready = false, starting = false;
    std::string name, peerAddress;
    RecompNetplay net{};
    RecompNetplayRoom room{};
    Clock::time_point opened, startAt;
};
Menu menu;

// Router forward, asked on its own thread: 0 not asked, 1 asking, 2 forwarded, 3 not forwarded.
std::atomic<int> upnpState{0};
char upnpAddress[80];
std::string lanAddress;

std::uint16_t keysOf(const RecompInputGamepad &pad)
{
    std::uint16_t keys = pad.buttons & 0x0fu; // the d-pad bits match Up, Down, Left, Right
    if (pad.thumb_ly > 16000) keys |= Up;
    if (pad.thumb_ly < -16000) keys |= Down;
    if (pad.thumb_lx < -16000) keys |= Left;
    if (pad.thumb_lx > 16000) keys |= Right;
    const std::uint16_t face[4] = {A, B, X, Y};
    for (unsigned i = 0u; i < 4u; ++i)
        if (pad.analog_buttons[i] > 0x40u) keys |= face[i];
    if ((pad.buttons & 0x20u) != 0u) keys |= B; // Back
    return keys;
}

// The main menu (frontend scene 0) has Versus (mode 5) chosen while the front end (state 4) runs.
bool versusChosen()
{
    return *recomp_memory(0x00df5dd8u, 1u) == 0u && *recomp_memory(0x00df57b4u, 1u) == 5u &&
        *recomp_memory(0x008a56d0u, 1u) == 4u;
}

#ifdef _WIN32
std::string pasteText()
{
    std::string text;
    if (!OpenClipboard(nullptr)) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *chars = static_cast<const wchar_t *>(GlobalLock(data))) {
            for (const wchar_t *c = chars; *c != 0 && text.size() < 256u; ++c)
                if (*c < 128) text.push_back(static_cast<char>(*c));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

void copyText(const std::string &text)
{
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1u) * sizeof(wchar_t))) {
        auto *chars = static_cast<wchar_t *>(GlobalLock(data));
        for (std::size_t i = 0u; i < text.size(); ++i) chars[i] = static_cast<unsigned char>(text[i]);
        chars[text.size()] = 0;
        GlobalUnlock(data);
        if (SetClipboardData(CF_UNICODETEXT, data) == nullptr) GlobalFree(data);
    }
    CloseClipboard();
}
#else
// shortcut: the clipboard is Windows only; use SDL's when the Linux runner draws this menu.
std::string pasteText() { return {}; }
void copyText(const std::string &) {}
#endif

void loadSettings()
{
    std::ifstream file(menu.storage / "netplay.txt");
    std::string key, value;
    while (file >> key >> value) {
        const unsigned long number = std::strtoul(value.c_str(), nullptr, 10);
        if (key == "delay" && number <= RECOMP_ROLLBACK_WINDOW) menu.delay = static_cast<std::uint32_t>(number);
        else if (key == "port" && number >= 1024u && number <= 65535u) menu.port = static_cast<std::uint32_t>(number);
        else if (key == "recent" && value.size() <= MaxAddress) menu.recent = value;
        else if (key == "display")
            for (int i = 0; i < 3; ++i)
                if (value == DisplayNames[i]) menu.display = i;
    }
}

void saveSettings()
{
    std::ofstream file(menu.storage / "netplay.txt", std::ios::trunc);
    file << "delay " << menu.delay << "\nport " << menu.port << "\ndisplay " << DisplayNames[menu.display] << '\n';
    if (!menu.recent.empty()) file << "recent " << menu.recent << '\n';
}

void close(bool local)
{
    menu.screen = Screen::Closed;
    menu.swallow = true;
    menu.injectA = local ? 3 : 0;
}

void openRouterPort(std::uint16_t port)
{
    static bool registered;
    if (!registered) {
        std::atexit(recomp_netplay_close_router_port);
        registered = true;
    }
    char lan[64];
    lanAddress = recomp_netplay_route_address(lan, sizeof lan) ? lan : "";
    int idle = 0;
    if (!upnpState.compare_exchange_strong(idle, 1)) return; // a forward is open or still being asked for
    std::thread([port] {
        char address[sizeof upnpAddress];
        const bool forwarded = recomp_netplay_open_router_port(port, address, sizeof address);
        std::memcpy(upnpAddress, address, sizeof address);
        upnpState.store(forwarded ? 2 : 3);
    }).detach();
}

// Checks the player's own name before any socket opens.
bool nameAllowed()
{
    menu.name = menu.ownName();
    if (menu.name.empty()) menu.name = "PLAYER";
    if (!recomp_netplay_name_banned(menu.name.c_str())) return true;
    menu.message = "YOUR PROFILE NAME IS NOT ALLOWED ONLINE.";
    return false;
}

void enterRoom(bool hosting)
{
    menu.hosting = hosting;
    menu.ready = menu.starting = false;
    menu.room = RecompNetplayRoom{};
    menu.room.hosting = hosting;
    menu.opened = Clock::now();
    menu.message.clear();
    menu.screen = Screen::Room;
}

void host()
{
    if (!nameAllowed()) return;
    if (!recomp_netplay_listen(&menu.net, static_cast<std::uint16_t>(menu.port))) {
        recomp_netplay_close(&menu.net);
        menu.message = "UDP PORT " + std::to_string(menu.port) + " IS IN USE.";
        return;
    }
    openRouterPort(static_cast<std::uint16_t>(menu.port));
    enterRoom(true);
}

void join()
{
    const std::size_t colon = menu.address.rfind(':');
    const std::string address = menu.address.substr(0u, colon);
    const unsigned long port = colon == std::string::npos ? menu.port
                                                          : std::strtoul(menu.address.c_str() + colon + 1u, nullptr, 10);
    if (address.empty() || port == 0u || port > 65535u) {
        menu.message = "ENTER AN ADDRESS LIKE 203.0.113.7:47701";
        return;
    }
    if (!nameAllowed()) return;
    if (!recomp_netplay_open(&menu.net, 0u, address.c_str(), static_cast<std::uint16_t>(port))) {
        recomp_netplay_close(&menu.net);
        menu.message = "THAT ADDRESS CANNOT BE USED.";
        return;
    }
    menu.peerAddress = address + ':' + std::to_string(port);
    menu.recent = menu.peerAddress;
    saveSettings();
    enterRoom(false);
}

void leaveRoom()
{
    recomp_netplay_close(&menu.net);
    int forwarded = 2;
    if (menu.hosting && upnpState.compare_exchange_strong(forwarded, 0)) recomp_netplay_close_router_port();
    else if (upnpState.load() == 3) upnpState.store(0);
    menu.screen = Screen::Versus;
    menu.cursor = menu.hosting ? 1 : 2;
}

std::string shownAddress()
{
    if (upnpState.load() == 2 && upnpAddress[0] != '\0') return upnpAddress;
    return lanAddress.empty() ? "UNKNOWN" : lanAddress + ':' + std::to_string(menu.port);
}

std::string routerStatus()
{
    const std::string port = std::to_string(menu.port);
    switch (upnpState.load()) {
    case 1: return "OPENING UDP " + port + "...";
    case 2: return upnpAddress[0] != '\0' ? "UDP " + port + " IS OPEN" : "BEHIND CARRIER NAT; LAN ONLY";
    default: return "NO UPNP; FORWARD UDP " + port + " BY HAND";
    }
}

// Returns true when the runner should restart into the session.
bool updateRoom(std::uint16_t keys)
{
    RecompNetplayRoom &room = menu.room;
    recomp_netplay_room_tick(&menu.net, &room, menu.digest.data(), menu.name.c_str(), menu.ready, menu.starting);
    const bool peerAllowed = room.joined && !room.mismatch && !recomp_netplay_name_banned(room.name);
    if (keys & ~X) menu.message.clear();
    if (keys & B) {
        leaveRoom();
        return false;
    }
    if (!peerAllowed) menu.ready = false;
    else if ((keys & A) && !menu.starting) menu.ready = !menu.ready;
    if ((keys & X) && menu.hosting) {
        copyText(shownAddress());
        menu.message = "ADDRESS COPIED.";
    }
    const auto now = Clock::now();
    if (menu.hosting && !menu.starting && menu.ready && room.ready && peerAllowed) {
        menu.starting = true;
        menu.startAt = now;
    }
    // The host keeps saying start for a moment so the joiner hears it.
    if (menu.hosting && menu.starting && now - menu.startAt >= std::chrono::milliseconds(600)) {
        std::ofstream(menu.storage / "netplay-launch.txt", std::ios::trunc)
            << "host " << menu.port << ' ' << menu.delay << '\n';
        return true;
    }
    if (!menu.hosting && room.start && menu.ready && peerAllowed) {
        std::ofstream(menu.storage / "netplay-launch.txt", std::ios::trunc) << "join " << menu.peerAddress << '\n';
        return true;
    }
    return false;
}

void update(std::uint16_t keys, bool &launch)
{
    switch (menu.screen) {
    case Screen::Closed:
        return;
    case Screen::Versus:
        if (keys) menu.message.clear();
        if (keys & Up) menu.cursor = (menu.cursor + 3) % 4;
        if (keys & Down) menu.cursor = (menu.cursor + 1) % 4;
        if (keys & B) close(false);
        else if (keys & A) {
            if (menu.cursor == 0) close(true);
            else if (menu.cursor == 1) host();
            else if (menu.cursor == 2) {
                menu.address = menu.recent;
                menu.caret = menu.address.size();
                menu.screen = Screen::Join;
            } else {
                menu.cursor = 0;
                menu.screen = Screen::Settings;
            }
        }
        return;
    case Screen::Settings:
        if (keys & Up) menu.cursor = (menu.cursor + 2) % 3;
        if (keys & Down) menu.cursor = (menu.cursor + 1) % 3;
        if (keys & (Left | Right)) {
            const int step = (keys & Right) ? 1 : -1;
            if (menu.cursor == 0) {
                const int delay = static_cast<int>(menu.delay) + step;
                if (delay >= 0 && delay <= static_cast<int>(RECOMP_ROLLBACK_WINDOW)) menu.delay = static_cast<std::uint32_t>(delay);
            } else if (menu.cursor == 1) {
                const int port = static_cast<int>(menu.port) + step;
                if (port >= 1024 && port <= 65535) menu.port = static_cast<std::uint32_t>(port);
            } else {
                menu.display = (menu.display + 3 + step) % 3;
            }
        }
        if (keys & (A | B)) {
            saveSettings();
            menu.cursor = 3;
            menu.screen = Screen::Versus;
        }
        return;
    case Screen::Join: {
        std::string &address = menu.address;
        if (keys) menu.message.clear();
        if ((keys & Left) && menu.caret > 0u) --menu.caret;
        if ((keys & Right) && menu.caret < address.size()) ++menu.caret;
        if (keys & (Up | Down)) {
            const std::size_t count = AddressCharacters.size();
            if (menu.caret == address.size() && address.size() < MaxAddress)
                address.push_back((keys & Up) ? AddressCharacters.front() : AddressCharacters.back());
            else if (menu.caret < address.size()) {
                const std::size_t at = AddressCharacters.find(address[menu.caret]);
                address[menu.caret] = AddressCharacters[(at + ((keys & Up) ? 1u : count - 1u)) % count];
            }
        }
        if ((keys & Y) && !address.empty()) {
            if (menu.caret == address.size()) --menu.caret;
            address.erase(menu.caret, 1u);
        }
        if (keys & X) {
            std::string pasted;
            for (const char c : pasteText())
                if (AddressCharacters.find(c) != std::string::npos && pasted.size() < MaxAddress) pasted.push_back(c);
            if (pasted.empty()) menu.message = "THE CLIPBOARD HAS NO ADDRESS.";
            else {
                address = pasted;
                menu.caret = address.size();
            }
        }
        if (keys & B) {
            menu.cursor = 2;
            menu.screen = Screen::Versus;
        } else if (keys & A) {
            join();
        }
        return;
    }
    case Screen::Room:
        launch = updateRoom(keys);
        return;
    }
}

void title(const char *text)
{
    recomp_overlay_rect(-1000.0f, 0.0f, 2640.0f, 480.0f, Backdrop);
    recomp_overlay_rect(40.0f, 38.0f, 6.0f, 52.0f, Red);
    recomp_overlay_text(58.0f, 40.0f, 4.0f, White, text);
    recomp_overlay_text(60.0f, 78.0f, 2.0f, Gray, "DEAD OR ALIVE 2 ULTIMATE");
}

void footer(const char *text)
{
    if (!menu.message.empty()) recomp_overlay_text(40.0f, 396.0f, 2.0f, Yellow, menu.message.c_str());
    recomp_overlay_rect(40.0f, 428.0f, 560.0f, 2.0f, Dim);
    recomp_overlay_text(40.0f, 442.0f, 2.0f, Gray, text);
}

void item(float y, bool selected, const char *text)
{
    if (selected) recomp_overlay_rect(40.0f, y - 10.0f, 420.0f, 41.0f, Red);
    recomp_overlay_text(60.0f, y, 3.0f, selected ? White : Gray, text);
}

// A name at the largest size that fits a 218-pixel plate line.
void plateName(float x, float y, const std::string &name, std::uint32_t color)
{
    if (name.size() <= 12u) recomp_overlay_text(x, y, 3.0f, color, name.c_str());
    else recomp_overlay_text(x, y + 4.0f, 2.0f, color, name.substr(0u, 18u).c_str());
}

void plate(float x, const char *label, const std::string &name, bool present, const std::string &detail, bool ready)
{
    recomp_overlay_rect(x, 116.0f, 250.0f, 156.0f, Panel);
    recomp_overlay_rect(x, 116.0f, 250.0f, 4.0f, present && ready ? Green : Red);
    recomp_overlay_text(x + 16.0f, 134.0f, 2.0f, Gray, label);
    plateName(x + 16.0f, 164.0f, present ? name : "WAITING...", present ? White : Dim);
    recomp_overlay_text(x + 16.0f, 204.0f, 2.0f, Gray, detail.c_str());
    if (present) recomp_overlay_text(x + 16.0f, 240.0f, 2.0f, ready ? Green : Gray, ready ? "READY" : "NOT READY");
}

void drawRoom()
{
    const RecompNetplayRoom &room = menu.room;
    title("ONLINE LOBBY ROOM");
    const std::string ping = room.pinged ? std::to_string(room.ping_ms) + " MS" : "";
    const bool peer = room.joined;
    const std::string peerName = room.mismatch ? "OTHER BUILD" : room.name;
    if (menu.hosting) {
        plate(40.0f, "P1  HOST", menu.name, true, "YOU", menu.ready);
        plate(350.0f, "P2", peerName, peer, ping, room.ready);
    } else {
        plate(40.0f, "P1  HOST", peerName, peer, peer ? ping : "CONNECTING...", room.ready);
        plate(350.0f, "P2", menu.name, true, "YOU", menu.ready);
    }
    recomp_overlay_text(304.0f, 180.0f, 3.0f, Red, "VS");
    if (menu.hosting) {
        recomp_overlay_text(40.0f, 296.0f, 2.0f, Gray, ("YOUR ADDRESS  " + shownAddress()).c_str());
        recomp_overlay_text(40.0f, 322.0f, 2.0f, Gray, ("ROUTER  " + routerStatus()).c_str());
        recomp_overlay_text(40.0f, 348.0f, 2.0f, Gray, ("INPUT DELAY  " + std::to_string(menu.delay) + " FRAMES").c_str());
    } else {
        recomp_overlay_text(40.0f, 296.0f, 2.0f, Gray, ("HOST  " + menu.peerAddress).c_str());
        recomp_overlay_text(40.0f, 322.0f, 2.0f, Gray, "INPUT DELAY  SET BY THE HOST");
    }
    if (menu.message.empty()) {
        const char *status = nullptr;
        std::string waiting;
        if (room.mismatch) status = "THE OTHER PLAYER RUNS ANOTHER BUILD.";
        else if (peer && recomp_netplay_name_banned(room.name)) status = "THE OTHER PLAYER'S NAME IS NOT ALLOWED.";
        else if (menu.starting || (room.start && menu.ready)) status = "BOTH READY: STARTING THE SESSION...";
        else if (peer && !menu.ready) status = "PRESS A WHEN YOU ARE READY.";
        else if (peer) status = (waiting = "WAITING FOR " + std::string(room.name) + " TO READY UP.").c_str();
        else if (menu.hosting) status = "SEND YOUR ADDRESS TO THE OTHER PLAYER.";
        else if (Clock::now() - menu.opened > std::chrono::seconds(5)) status = "NO ANSWER YET. CHECK THE ADDRESS.";
        if (status != nullptr) recomp_overlay_text(40.0f, 396.0f, 2.0f, Yellow, status);
    }
    footer(menu.hosting ? "A READY   B LEAVE   X COPY ADDRESS" : "A READY   B LEAVE");
}

void draw()
{
    static const char *const items[4] = {"LOCAL", "HOST ONLINE", "JOIN ONLINE", "NETPLAY SETTINGS"};
    static const char *const details[4] = {
        "TWO CONTROLLERS ON THIS MACHINE.",
        "WAIT HERE FOR A PLAYER TO JOIN YOUR GAME.",
        "CONNECT TO A PLAYER WHO IS HOSTING.",
        "INPUT DELAY AND THE PORT YOU HOST ON.",
    };
    switch (menu.screen) {
    case Screen::Closed:
        return;
    case Screen::Versus:
        title("VERSUS");
        for (int i = 0; i < 4; ++i) item(134.0f + 52.0f * static_cast<float>(i), menu.cursor == i, items[i]);
        recomp_overlay_text(40.0f, 362.0f, 2.0f, White, details[menu.cursor]);
        footer("A SELECT   B BACK");
        return;
    case Screen::Settings: {
        title("NETPLAY SETTINGS");
        const std::string delay = "< " + std::to_string(menu.delay) + " FRAMES >";
        const std::string port = "< " + std::to_string(menu.port) + " >";
        item(134.0f, menu.cursor == 0, "INPUT DELAY");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(delay.c_str(), 2.0f), 138.0f, 2.0f, White, delay.c_str());
        item(186.0f, menu.cursor == 1, "PORT");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(port.c_str(), 2.0f), 190.0f, 2.0f, White, port.c_str());
        const std::string display = std::string("< ") + DisplayNames[menu.display] + " >";
        item(238.0f, menu.cursor == 2, "NETWORK DISPLAY");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(display.c_str(), 2.0f), 242.0f, 2.0f, White,
            display.c_str());
        static const char *const details[3] = {
            "LOWER DELAY FEELS SHARPER, MORE ROLLBACK.",
            "THE HOST'S ROUTER MUST LET THIS PORT IN.",
            "PING, OR FULL STATS, IN A SESSION'S CORNER.",
        };
        recomp_overlay_text(40.0f, 362.0f, 2.0f, White, details[menu.cursor]);
        footer("LEFT/RIGHT CHANGE   B BACK");
        return;
    }
    case Screen::Join: {
        title("JOIN ONLINE");
        recomp_overlay_text(60.0f, 132.0f, 2.0f, Gray, "HOST ADDRESS");
        recomp_overlay_rect(56.0f, 154.0f, 528.0f, 44.0f, Panel);
        recomp_overlay_text(68.0f, 165.0f, 3.0f, White, menu.address.c_str());
        if ((menu.frame / 20u) % 2u == 0u)
            recomp_overlay_rect(68.0f + 18.0f * static_cast<float>(menu.caret), 189.0f, 15.0f, 3.0f, Red);
        recomp_overlay_text(60.0f, 222.0f, 2.0f, Gray, "UP/DOWN CHANGE   LEFT/RIGHT MOVE");
        recomp_overlay_text(60.0f, 248.0f, 2.0f, Gray, "Y DELETE   X PASTE FROM CLIPBOARD");
        if (!menu.recent.empty())
            recomp_overlay_text(60.0f, 296.0f, 2.0f, Dim, ("LAST HOST  " + menu.recent).c_str());
        footer("A CONNECT   B BACK");
        return;
    }
    case Screen::Room:
        drawRoom();
        return;
    }
}

void onOverlay(const RecompD3dPresenterTarget *target)
{
    bool launch = false;
    {
        std::lock_guard<std::mutex> guard(menu.lock);
        if (menu.screen == Screen::Closed) return;
        ++menu.frame;
        std::uint16_t down = 0u;
        for (const std::uint16_t held : menu.held) down |= held;
        std::uint16_t keys = menu.pressed;
        menu.pressed = 0u;
        // A held direction repeats after 300 ms, then every 67 ms.
        const std::uint16_t directions = down & (Up | Down | Left | Right);
        if (directions == 0u || (keys & directions) != 0u) menu.repeat = 0u;
        else if (++menu.repeat >= 18u && (menu.repeat - 18u) % 4u == 0u) keys |= directions;
        update(keys, launch);
        draw();
        recomp_overlay_submit(target, menu.aspect);
    }
    if (launch) recomp_stop(4, "netplay:room-start");
}

} // namespace

void recomp_netplay_menu_install(const std::filesystem::path &storage, const std::string &digest,
    std::function<std::string()> ownName)
{
    menu.installed = true;
    menu.storage = storage;
    menu.digest = digest;
    menu.digest.resize(RECOMP_NETPLAY_DIGEST_BYTES, '0');
    menu.ownName = std::move(ownName);
    menu.aspect = recomp_overlay_display_aspect();
    loadSettings();
    recomp_d3d_overlay_hook = onOverlay;
}

void recomp_netplay_menu_filter(std::uint32_t port, RecompInputGamepad *pad)
{
    if (!menu.installed || port >= RECOMP_INPUT_PORT_COUNT) return;
    std::lock_guard<std::mutex> guard(menu.lock);
    const std::uint16_t keys = keysOf(*pad);
    const std::uint16_t edges = keys & ~menu.held[port];
    menu.held[port] = keys;
    if (menu.screen == Screen::Closed) {
        if (menu.swallow) {
            bool down = false;
            for (const std::uint16_t held : menu.held) down = down || held != 0u;
            if (down) {
                *pad = RecompInputGamepad{};
                return;
            }
            menu.swallow = false;
        }
        if (menu.injectA > 0 && port == menu.openedBy) {
            --menu.injectA;
            pad->analog_buttons[RECOMP_INPUT_ANALOG_A] = RECOMP_INPUT_ANALOG_PRESSED;
            return;
        }
        if ((edges & A) == 0u || !versusChosen()) return;
        menu.screen = Screen::Versus;
        menu.cursor = 0;
        menu.message.clear();
        menu.openedBy = port;
        menu.pressed = 0u;
    } else {
        menu.pressed |= edges;
    }
    *pad = RecompInputGamepad{};
}

bool recomp_netplay_menu_take_launch(const std::filesystem::path &storage, std::uint32_t &hostPort,
    std::uint32_t &delay, std::string &join)
{
    const std::filesystem::path path = storage / "netplay-launch.txt";
    std::string kind, value;
    unsigned long frames = 0u;
    {
        std::ifstream file(path);
        if (!file) return false;
        file >> kind >> value;
        if (kind == "host") file >> frames;
    }
    std::error_code error;
    std::filesystem::remove(path, error); // one start per note, even a failed one
    if (kind == "host") {
        const unsigned long port = std::strtoul(value.c_str(), nullptr, 10);
        if (port == 0u || port > 65535u || frames > RECOMP_ROLLBACK_WINDOW) return false;
        hostPort = static_cast<std::uint32_t>(port);
        delay = static_cast<std::uint32_t>(frames);
        return true;
    }
    if (kind != "join" || value.empty()) return false;
    join = value;
    return true;
}

int recomp_netplay_menu_display(const std::filesystem::path &storage)
{
    menu.storage = storage;
    loadSettings();
    return menu.display;
}
