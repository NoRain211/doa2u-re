/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rebind.h"

#include "input_host_win32.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

// Set on the game's thread, taken where pads are sampled.
std::atomic<int> listening{0}; // binding + 1, plus 0x100 for a host pad button
std::atomic<int> resetting{0}; // 1 for the keyboard, 2 for the host pads
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
    const bool pad = (request & 0x100) != 0;
    const int source = pad ? button : key, other = pad ? key : button;
    if (source >= 0) {
        recomp_input_host_bind(static_cast<unsigned>((request & 0xff) - 1), pad, source);
        changed = true;
    }
    if (source >= 0 || other >= 0) {
        listening = 0;
        settling = true;
    }
}

/* Temps a crashed save left behind; a live save finishes in milliseconds. */
void removeStaleSaves(const std::string &target)
{
    const std::filesystem::path path(target), folder = path.parent_path();
    const std::string prefix = path.filename().string() + ".tmp.";
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
    std::error_code error;
    std::filesystem::directory_iterator it(
        folder.empty() ? std::filesystem::path(".") : folder, error), end;
    for (; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0u, prefix.size(), prefix) != 0) continue;
        const std::string suffix = name.substr(prefix.size());
        const size_t dot = suffix.find('.');
        if (dot == std::string::npos || dot == 0u || dot + 1u == suffix.size() ||
            suffix.find_first_not_of("0123456789", 0u) != dot ||
            suffix.find_first_not_of("0123456789", dot + 1u) != std::string::npos)
            continue; // not one of ours
        std::error_code age;
        if (it->last_write_time(age) < cutoff && !age) std::filesystem::remove(it->path(), age);
    }
}

bool save(const char *variable, bool pad)
{
    const char *path = std::getenv(variable);
    if (path == nullptr || *path == '\0') {
        std::fprintf(stderr, "recomp rebind: %s is not set; the change lasts until the game closes\n", variable);
        return true;
    }
    const std::string target(path);
    removeStaleSaves(target);
    // A per-save name keeps O_EXCL from colliding with a live writer; a name
    // that already exists is tried again with the next one.
    static std::atomic<unsigned> attempt{0};
    std::string temporary;
#ifdef _WIN32
    int fd = -1;
    int error = 0;
    for (unsigned tries = 0u; tries < 8u; ++tries) {
        temporary = target + ".tmp." + std::to_string(_getpid()) + "." + std::to_string(attempt.fetch_add(1u));
        error = _sopen_s(&fd, temporary.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
            _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (error != EEXIST) break;
    }
    std::FILE *file = error == 0 ? _fdopen(fd, "wb") : nullptr;
#else
    int fd = -1;
    for (unsigned tries = 0u; tries < 8u; ++tries) {
        temporary = target + ".tmp." + std::to_string(getpid()) + "." + std::to_string(attempt.fetch_add(1u));
        fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    std::FILE *file = fd >= 0 ? fdopen(fd, "wb") : nullptr;
#endif
    bool written = file != nullptr;
    const char *separator = "{\n";
    if (file != nullptr) {
        for (unsigned binding = 0u; binding < RECOMP_INPUT_BIND_COUNT; ++binding) {
            const char *name = recomp_rebind_name(binding, pad);
            std::fprintf(file, "%s  \"%s\": \"%s\"", separator, recomp_input_host_binding_name(binding),
                name != nullptr ? name : "");
            separator = ",\n";
        }
        std::fprintf(file, "%s", *separator == '{' ? "{}\n" : "\n}\n");
        written = std::ferror(file) == 0;
        written = std::fclose(file) == 0 && written;
    } else {
#ifdef _WIN32
        if (fd >= 0) _close(fd);
#else
        if (fd >= 0) close(fd);
#endif
    }
    if (written) {
        std::error_code error;
        std::filesystem::rename(temporary, target, error); // replaces an existing target
        if (!error) return true;
    }
    std::fprintf(stderr, "recomp rebind: cannot save %s\n", path);
    if (fd >= 0) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored); // only the temp this save made
    }
    return false;
}

} // namespace

void recomp_rebind_listen(unsigned binding, bool pad)
{
    if (binding >= RECOMP_INPUT_BIND_COUNT) return;
    armed = false;
    listening = static_cast<int>(binding) + 1 + (pad ? 0x100 : 0);
}

bool recomp_rebind_listening(void) { return listening != 0; }

void recomp_rebind_reset(bool pad) { resetting = pad ? 2 : 1; }

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
            recomp_input_host_bind(binding, reset == 2, -1);
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
