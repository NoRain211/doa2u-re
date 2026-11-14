/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rebind.h"

#include "input_host_win32.h"

#include "settings_file.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Set on the game's thread, taken where pads are sampled.
// A request packs binding + 1 (0 for none) with PAD_REQUEST for a host pad button.
constexpr int PAD_REQUEST = 0x100;
int makeRequest(unsigned binding, bool pad) { return static_cast<int>(binding) + 1 + (pad ? PAD_REQUEST : 0); }
unsigned requestBinding(int request) { return static_cast<unsigned>((request & ~PAD_REQUEST) - 1); }
bool requestPad(int request) { return (request & PAD_REQUEST) != 0; }
std::atomic<int> listening{0}; // the binding to give the next press
std::atomic<int> resetting{0}; // the device to reset, as a request of binding 0
std::atomic<bool> changed{false};
std::atomic<bool> armed{false}; // the press that started listening is not the new one
// Only where pads are sampled.
bool settling;
bool held[RECOMP_INPUT_PORT_COUNT];

bool anyHeld(const RecompInputGamepad &pad)
{
    if (pad.buttons != 0u) return true;
    for (const std::uint8_t value : pad.analog_buttons)
        if (value > RECOMP_INPUT_ANALOG_DOWN) return true;
    return pad.thumb_lx > 16000 || pad.thumb_lx < -16000 || pad.thumb_ly > 16000 || pad.thumb_ly < -16000;
}

// Port 0's sample takes the keyboard's and every pad's fresh presses.
void listen(int request)
{
    const int key = recomp_input_host_take_press(false), button = recomp_input_host_take_press(true);
    if (!armed) {
        armed = true; // the press that started listening, or anything still held
        return;
    }
    const bool pad = requestPad(request);
    const int source = pad ? button : key, other = pad ? key : button;
    if (source >= 0) {
        recomp_input_host_bind(requestBinding(request), pad, source);
        changed = true;
    }
    if (source >= 0 || other >= 0) {
        listening = 0;
        settling = true;
    }
}

bool save(const char *variable, bool pad)
{
    const char *path = std::getenv(variable);
    if (path == nullptr || *path == '\0') {
        std::fprintf(stderr, "recomp rebind: %s is not set; the change lasts until the game closes\n", variable);
        return true;
    }
    std::string out = "{";
    for (unsigned binding = 0u; binding < RECOMP_INPUT_BIND_COUNT; ++binding) {
        const char *name = recomp_rebind_name(binding, pad);
        out += std::string(binding == 0u ? "\n" : ",\n") + "  \"" + recomp_input_host_binding_name(binding) +
            "\": \"" + (name != nullptr ? name : "") + "\"";
    }
    out += "\n}\n";
    if (recomp_settings_write(path, out)) return true;
    std::fprintf(stderr, "recomp rebind: cannot save %s\n", path);
    return false;
}

void load(const char *variable, bool pad)
{
    const char *path = std::getenv(variable);
    std::string content;
    if (path == nullptr || *path == '\0' || !recomp_settings_read(path, &content)) return; // the defaults
    RecompSettingsEntries entries;
    if (!recomp_settings_parse(content, &entries)) {
        std::fprintf(stderr, "recomp rebind: %s is not a JSON object; the defaults stay\n", path);
        return;
    }
    int sources[RECOMP_INPUT_BIND_COUNT] = {};
    bool given[RECOMP_INPUT_BIND_COUNT] = {};
    for (const auto &[key, value] : entries) {
        unsigned binding = 0u;
        while (binding < RECOMP_INPUT_BIND_COUNT && key != recomp_input_host_binding_name(binding)) ++binding;
        const bool unbound = value == "\"\"";
        std::string name;
        const int source = !unbound && recomp_settings_string(value, &name)
            ? recomp_input_host_source(pad, name.c_str()) : -1;
        if (binding == RECOMP_INPUT_BIND_COUNT || (!unbound && source < 0)) {
            std::fprintf(stderr, "recomp rebind: %s: cannot bind %s to %s\n", path, key.c_str(), value.c_str());
            continue;
        }
        sources[binding] = source;
        given[binding] = true;
    }
    recomp_input_host_load_bindings(pad, sources, given);
}

} // namespace

void recomp_rebind_load(void)
{
    load("RECOMP_KEYBOARD_SETTINGS", false);
    load("RECOMP_CONTROLLER_SETTINGS", true);
}

void recomp_rebind_listen(unsigned binding, bool pad)
{
    if (binding >= RECOMP_INPUT_BIND_COUNT) return;
    armed = false;
    listening = makeRequest(binding, pad);
}

bool recomp_rebind_listening(void) { return listening != 0; }

void recomp_rebind_reset(bool pad) { resetting = makeRequest(0u, pad); }

const char *recomp_rebind_name(unsigned binding, bool pad)
{
    return recomp_input_host_source_name(pad, recomp_input_host_bound(binding, pad));
}

void recomp_rebind_save(void)
{
    if (!changed.exchange(false)) return;
    const bool keyboard = save("RECOMP_KEYBOARD_SETTINGS", false);
    const bool controller = save("RECOMP_CONTROLLER_SETTINGS", true);
    if (!keyboard || !controller) changed = true; // retry at the next save, keeping changes made meanwhile
}

void recomp_rebind_filter(uint32_t port, RecompInputGamepad *pad)
{
    if (port >= RECOMP_INPUT_PORT_COUNT) return;
    if (const int reset = resetting.exchange(0); reset != 0) {
        for (unsigned binding = 0u; binding < RECOMP_INPUT_BIND_COUNT; ++binding)
            recomp_input_host_bind(binding, requestPad(reset), -1);
        changed = true;
    }
    held[port] = anyHeld(*pad);
    const int request = listening;
    if (request != 0 && port == 0u) listen(request);
    if (settling) {
        bool down = false;
        for (const bool h : held) down = down || h;
        settling = down;
    }
    if (request != 0 || settling) *pad = RecompInputGamepad{};
}

void recomp_rebind_release(uint32_t port)
{
    if (port < RECOMP_INPUT_PORT_COUNT) held[port] = false;
}
