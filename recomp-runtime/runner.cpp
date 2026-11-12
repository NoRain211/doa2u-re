/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_diagnostics.h"
#include "d3d8_adapter.h"
#include "d3d_presenter.h"
#include "dsound_api_adapter.h"
#include "dsound_buffer_adapter.h"
#include "guest_snapshot.h"
#include "fiber_adapter.h"
#include "free_camera_adapter.h"
#include "combo_hud.h"
#include "cpu_costume.h"
#include "unlock_profile.h"
#include "pc_settings.h"
#include "runner_assets.h"
#include "menu_quit.h"
#include "pc_settings_store.h"
#include "input_adapter.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "netplay_menu.h"
#include "netplay_session.h"
#include "perf_overlay.h"
#include "rollback.h"
#include "runner_load.h"
#include "save_transaction.h"
#include "synctest.h"
#include "d3d_vblank.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
extern "C" {
#include "kernel_abi.h"
#include "native_fiber.h"
#include "overlay_text.h"
#include "program_manual.h"
#include "quit_prompt.h"
#include "stop_report.h"
RecompFunction recomp_program_lookup(std::uint32_t address);
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <charconv>
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
#endif

static RecompInputPulseSource inputPulses;
static bool scriptedInputOnly;
// SyncTest's RAM snapshots read this; write tracking is fixed when guest RAM is allocated.
static bool guestRamWriteWatch;
static std::uint8_t *guestRam;

static void quitPrompt(std::uint32_t port, RecompInputGamepad *gamepad)
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    RecompOverlayText text;
    recomp_overlay_clear(&text);
    switch (recomp_quit_prompt_filter(port, gamepad, static_cast<std::uint64_t>(now))) {
    case RECOMP_QUIT_OPENED:
        recomp_overlay_set_title(&text, "Quit to the desktop?");
        recomp_overlay_set_hint(&text, "A quit   B keep playing");
        recomp_overlay_publish(RECOMP_OVERLAY_QUIT_PROMPT, &text);
        break;
    case RECOMP_QUIT_CANCELLED:
        recomp_overlay_publish(RECOMP_OVERLAY_QUIT_PROMPT, &text);
        break;
    case RECOMP_QUIT_CHOSEN:
        recomp_stop(0, "player-quit");
        break;
    case RECOMP_QUIT_NONE:
        break;
    }
}

// A host pad goes through the quit prompt, then the netplay menu. The open
// prompt owns the pads, so the room cannot inject its A behind it.
static void filterHostPad(std::uint32_t port, RecompInputGamepad *gamepad, bool sampled)
{
    quitPrompt(port, sampled ? gamepad : nullptr);
    if (sampled && !recomp_quit_prompt_open()) recomp_netplay_menu_filter(port, gamepad);
}

static bool sampleInput(std::uint32_t port, RecompInputGamepad *gamepad)
{
    bool present = false;
    if (recomp_netplay_session_pad(port, gamepad, &present)) return present;
    if (port != 0u) {
        const bool sampled = !scriptedInputOnly && recomp_input_host_sample(port, gamepad);
        filterHostPad(port, gamepad, sampled);
        return sampled;
    }
    const bool sampled = recomp_input_pulse_source_sample(&inputPulses, gamepad);
    static bool startHeld;
    const bool start = sampled && (gamepad->buttons & RECOMP_INPUT_BUTTON_START) != 0u;
    if (start != startHeld) {
        std::fprintf(stderr, "recomp input: Start %s poll=%llu\n", start ? "down" : "up",
            static_cast<unsigned long long>(inputPulses.sample_count));
        startHeld = start;
    }
    filterHostPad(0u, gamepad, sampled);
    return recomp_netplay_session_back_pad(gamepad) || sampled;
}

/* Offer every costume and character without saving them (unlock_profile.c):
   the live profile is unlocked in memory each frame, while its saves and
   awards keep to what was earned, so turning this off gives it back. */
static bool unlockAll = true; // --no-unlock-all leaves unlocks to play

/* PC SETTINGS in the game's Options menu (pc_settings.c) edits the launchers'
   private/launcher.json, which run_game.py names in RECOMP_LAUNCHER_SETTINGS.
   Unlocks and CPU Costumes change at once (offline only); the other rows
   apply at the next launch. */
static std::string launcherSettings;
static RecompPcSettingsStore pcStore;

static void pcSettingChanged(unsigned setting, unsigned choice, void *)
{
    switch (setting) {
    case RECOMP_PC_UNLOCKS: unlockAll = choice == 0u; break;
    case RECOMP_PC_CPU_COSTUMES: recomp_cpu_costume_set_enabled(choice == 1u); break;
    case RECOMP_PC_FREE_CAMERA: recomp_free_camera_set_enabled(choice == 1u); break;
    case RECOMP_PC_LIFE_BAR: recomp_combo_hud_set_life_bar_colour(choice); break;
    case RECOMP_PC_RESOLUTION:
    case RECOMP_PC_MSAA:
    case RECOMP_PC_SMAA: {
        const unsigned height = RECOMP_PC_HEIGHTS[recomp_pc_settings_choice(RECOMP_PC_RESOLUTION)];
        const unsigned samples = RECOMP_PC_SAMPLES[recomp_pc_settings_choice(RECOMP_PC_MSAA)];
        recomp_d3d_presenter_set_quality(static_cast<float>(height) / 480.0f, samples,
            recomp_pc_settings_choice(RECOMP_PC_SMAA) == 1u);
        break;
    }
    default: break;
    }
}

static void pcSettingsClosed(void *)
{
    if (launcherSettings.empty()) return;
    std::uint8_t choices[RECOMP_PC_SETTING_COUNT];
    for (unsigned i = 0u; i < RECOMP_PC_SETTING_COUNT; ++i) choices[i] = recomp_pc_settings_choice(i);
    if (!recomp_pc_settings_write(launcherSettings, pcStore, choices))
        std::fprintf(stderr, "recomp runner: PC settings not saved; launcher settings unreadable or unwritable\n");
}

static void installPcSettings(bool online)
{
    const char *path = std::getenv("RECOMP_LAUNCHER_SETTINGS");
    launcherSettings = path != nullptr ? path : "";
    pcStore = {};
#ifdef _WIN32
    // The launchers' defaults: the primary display's height, and its shape for 21:9
    // when it is a 21:9 display (wider than 16:9, narrower than 3:1).
    const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    if (height > 0) pcStore.primaryHeight = height;
    if (width * 9 > height * 16 && width < height * 3) pcStore.ultrawideShape = std::to_string(width) + ":" + std::to_string(height);
#endif
    std::uint8_t choices[RECOMP_PC_SETTING_COUNT];
    recomp_pc_settings_read(launcherSettings, pcStore, choices);
    // The rows that apply at once show what is running. Online turns the free camera
    // and CPU costumes off for the session, so those rows keep the saved choice.
    choices[RECOMP_PC_UNLOCKS] = unlockAll ? 0u : 1u;
    if (!online) {
        choices[RECOMP_PC_CPU_COSTUMES] = recomp_cpu_costume_enabled() ? 1u : 0u;
        choices[RECOMP_PC_FREE_CAMERA] = recomp_free_camera_enabled() ? 1u : 0u;
    }
    recomp_combo_hud_set_life_bar_colour(choices[RECOMP_PC_LIFE_BAR]);
    const RecompPcSettingsHost host{pcSettingChanged, pcSettingsClosed, nullptr};
    recomp_pc_settings_install(&host, choices);
    // Both renderers rebuild their targets live (applyQuality in each presenter).
    const bool quality = recomp_d3d_presenter_quality_live();
    for (unsigned setting : {RECOMP_PC_RESOLUTION, RECOMP_PC_MSAA, RECOMP_PC_SMAA})
        recomp_pc_settings_set_live(setting, quality);
    // Online they show the page's next-start help; the choice applies at the next offline start.
    for (unsigned setting : {RECOMP_PC_CPU_COSTUMES, RECOMP_PC_FREE_CAMERA})
        recomp_pc_settings_set_live(setting, !online);
}

/* Dedication card shown each time the staff roll starts. The BMP is a private
   input named by RECOMP_DEDICATION_CARD. Music table entry 17 (0x6a0c3c,
   "Staffroll") points at its track's file name at +8; the guest's first read
   of that entry in an AFS archive starts the roll. */
static std::vector<std::uint32_t> cardPixels;
static std::uint32_t cardWidth, cardHeight;
static std::map<std::string, std::uint64_t> staffrollOffsets; // per archive; 0 if absent
static bool cardPending;

// 24- or 32-bit uncompressed BMP to top-down opaque BGRA.
static bool loadCard(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    const std::vector<unsigned char> b{std::istreambuf_iterator<char>(file), {}};
    if (b.size() < 54u || b[0] != 'B' || b[1] != 'M') return false;
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(b[at] | b[at + 1] << 8 | b[at + 2] << 16) |
            static_cast<std::uint32_t>(b[at + 3]) << 24;
    };
    const std::uint32_t data = u32(10u), width = u32(18u), bits = b[28] | b[29] << 8;
    const auto rawHeight = static_cast<std::int32_t>(u32(22u));
    const std::uint32_t height = rawHeight < 0 ? 0u - static_cast<std::uint32_t>(rawHeight)
                                               : static_cast<std::uint32_t>(rawHeight);
    if (width == 0u || height == 0u || width > 4096u || height > 4096u ||
        (bits != 24u && bits != 32u) || u32(30u) != 0u) return false;
    const std::size_t stride = (width * bits / 8u + 3u) & ~3u;
    if (data > b.size() || stride * height > b.size() - data) return false;
    cardPixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const unsigned char *row = &b[data + stride * (rawHeight < 0 ? y : height - 1u - y)];
        for (std::uint32_t x = 0; x < width; ++x, row += bits / 8u)
            cardPixels[static_cast<std::size_t>(y) * width + x] =
                0xff000000u | row[2] << 16 | row[1] << 8 | row[0];
    }
    cardWidth = width;
    cardHeight = height;
    return true;
}

// Offset of a named entry from the AFS archive's own directory; zero if absent.
static std::uint64_t afsEntryOffset(const char *path, const char *name)
{
    std::ifstream file(path, std::ios::binary);
    std::uint32_t header[2]{};
    if (!file.read(reinterpret_cast<char *>(header), sizeof header) ||
        header[0] != 0x00534641u || header[1] == 0u || header[1] > 65536u) return 0u;
    std::vector<std::uint32_t> toc(header[1] * 2u + 2u);
    if (!file.read(reinterpret_cast<char *>(toc.data()), toc.size() * 4u)) return 0u;
    std::vector<char> names(header[1] * 48u);
    if (toc[header[1] * 2u] == 0u || toc[header[1] * 2u + 1u] < names.size() ||
        !file.seekg(toc[header[1] * 2u]) || !file.read(names.data(), names.size())) return 0u;
    for (std::uint32_t i = 0; i < header[1]; ++i)
        if (std::strncmp(&names[i * 48u], name, 32u) == 0) return toc[i * 2u];
    return 0u;
}

static void noteRead(const char *hostPath, std::uint64_t offset)
{
    const std::size_t length = std::strlen(hostPath);
    if (cardPixels.empty() || length < 4u || _stricmp(hostPath + length - 4u, ".afs") != 0) return;
    const auto [entry, added] = staffrollOffsets.try_emplace(hostPath, 0u);
    if (added) entry->second = afsEntryOffset(hostPath,
        reinterpret_cast<const char *>(recomp_memory(*recomp_memory_u32(0x006a0c44u), 32u)));
    if (entry->second != 0u && offset == entry->second) cardPending = true;
}

/* --story-finale debug shortcut: every live Story CPU is held at 1 life, and
   the first CPU KO sets the stage to 5 so the game's own advance loads the
   final fight. Measured on this image's Kasumi Story: mode 0x87cfa0 is 3,
   stage 0x8783ec runs 0..6 with Tengu last, and each side's block (stride
   0x94) keeps its human flag at 0x87ae30 and 16-bit life (300 at Normal; 320
   between fights) at 0x87ae54. Changing either during fight setup crashed. */
static bool storyFinale;

static void applyStoryFinale()
{
    static bool fightLive;
    if (*recomp_memory(0x0087cfa0u, 1u) != 3u) {
        fightLive = false;
        return;
    }
    for (std::uint32_t side = 0; side < 2u; ++side) {
        if (*recomp_memory(0x0087ae30u + side * 0x94u, 1u) != 0u) continue;
        std::uint8_t *life = recomp_memory(0x0087ae54u + side * 0x94u, 2u);
        const auto value = static_cast<std::int16_t>(life[0] | life[1] << 8);
        if (value >= 1 && value <= 300) fightLive = true;
        if (value >= 2 && value <= 300) {
            life[0] = 1u;
            life[1] = 0u;
        } else if (value == 0 && fightLive) {
            fightLive = false;
            std::uint8_t *stage = recomp_memory(0x008783ecu, 1u);
            if (*stage < 5u) *stage = 5u;
        }
    }
}

// Any pad button or key held now, ignoring thumbsticks and light trigger pressure.
static bool anyButtonHeld()
{
    if (scriptedInputOnly) return false;
    for (std::uint32_t port = 0; port < RECOMP_INPUT_PORT_COUNT; ++port) {
        RecompInputGamepad pad{};
        if (!recomp_input_host_sample(port, &pad)) continue;
        if (pad.buttons != 0u) return true;
        for (const std::uint8_t analog : pad.analog_buttons)
            if (analog >= 0x80u) return true;
    }
    return false;
}

/* Holds the guest at a Swap: 1.5 s fade in, hold, 1.5 s fade out over 10 s.
   A new button press starts the fade out early; one held from the fight must
   be released first. */
static void showCard()
{
    using namespace std::chrono;
    constexpr auto fade = milliseconds(1500), total = milliseconds(10000);
    const float aspect = (recomp_kernel_video_flags() & 0x00010000u) != 0u
        ? recomp_kernel_widescreen_aspect() : 4.0f / 3.0f;
    std::fprintf(stderr, "recomp runner: staff roll starts; dedication card shown\n");
    // Guest time stops for the card, so the output pump must not play past it.
    recomp_dsound_output_lock();
    const auto start = steady_clock::now();
    steady_clock::duration end = total;
    bool held = true;
    for (auto next = start;;) {
        const auto elapsed = steady_clock::now() - start;
        const bool pressed = anyButtonHeld();
        // Fade out from the current level: as long as the fade in took so far.
        if (pressed && !held) end = std::min(end, elapsed + std::min<steady_clock::duration>(elapsed, fade));
        held = pressed;
        if (elapsed >= end) break;
        const auto edge = std::min<steady_clock::duration>(elapsed, end - elapsed);
        const auto level = static_cast<std::uint8_t>(edge >= fade ? 255 : 255 * edge / fade);
        recomp_d3d8_present_card(cardPixels.data(), cardWidth, cardHeight, aspect, level);
        next += microseconds(16667);
        recomp_d3d_sleep_until(duration_cast<nanoseconds>(next.time_since_epoch()).count());
    }
    recomp_xapi_hold_time(static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now() - start).count()));
    recomp_dsound_output_unlock();
}

static void onSwap(std::uint32_t frame)
{
    recomp_unlock_profile_frame(recomp_netplay_session_unlock_all(unlockAll));
    if (storyFinale) applyStoryFinale();
    recomp_netplay_session_on_swap(frame);
    if (!cardPending) return;
    cardPending = false;
    showCard();
}

static void saveInputPulses(void *state)
{
    std::memcpy(state, &inputPulses, sizeof inputPulses);
}

static void restoreInputPulses(const void *state)
{
    std::memcpy(&inputPulses, state, sizeof inputPulses);
}

static void onFrameBoundary(std::uint32_t frame)
{
    /* A snapshot from before the main fiber exists cannot be restored. */
    if (recomp_fiber_adapter_model()->current_handle == 0u) return;
    const RecompSyncTestResult result = recomp_synctest_frame(frame);
    if (result == RECOMP_SYNCTEST_MISMATCH) recomp_stop(2, "synctest:mismatch");
    if (result == RECOMP_SYNCTEST_SKIP) recomp_stop(3, "synctest:skip");
    if (result == RECOMP_SYNCTEST_MATCH && !recomp_synctest_loop())
        recomp_stop(0, "synctest:match");
}

static const doaxbv::XbeParseResult *xbe;

static bool loadImage()
{
    std::vector<std::uint8_t> image(RECOMP_XBOX_RAM_SIZE, 0u);
    std::string error;
    if (!recomp_load_xbe(*xbe, image, error)) {
        std::cerr << "recomp runner: " << error << '\n';
        return false;
    }
    std::memcpy(guestRam, image.data(), image.size());
    const auto imageEnd = static_cast<std::uint64_t>(xbe->metadata.header.baseAddress) + xbe->metadata.header.sizeImage;
    if (!xbox_HeapSetImageEnd(static_cast<std::uint32_t>(imageEnd))) {
        std::cerr << "recomp runner: invalid heap image boundary\n";
        return false;
    }
    return true;
}

static void bootGuest();

static VOID WINAPI rebootGuest(void *)
{
    recomp_reset_runtime_modules();
    if (!loadImage()) recomp_stop(2, "runner:reboot-image");
    bootGuest();
}

static void reboot()
{
    recomp_input_host_stop_vibration();
    // The guest's stacks are never resumed; the new fiber frees them.
    void *fiber = recomp_native_fiber_create(rebootGuest, nullptr);
    if (fiber == nullptr) recomp_stop(2, "runner:reboot-fiber");
    recomp_native_fiber_switch(fiber);
}

int main(int argc, char **argv)
{
    recomp_install_host_diagnostics();
    recomp_d3d_frame_breakdown = recomp_kernel_thread_times;
    std::string xbePath, discRoot, expectStop, milestoneLog;
    std::string replayPath;
    RecompNetplayLaunch launch;
    recomp_input_pulse_source_init(&inputPulses, recomp_input_host_sample, 0u);
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--input-script-only") {
            scriptedInputOnly = true;
            inputPulses.base = nullptr;
            continue;
        }
        if (argument == "--story-finale") {
            storyFinale = true;
            continue;
        }
        if (argument == "--no-unlock-all") {
            unlockAll = false;
            continue;
        }
        if (argument == "--skip-intro") {
            recomp_skip_intro = true;
            continue;
        }
        if ((argument == "--netplay-host" || argument == "--netplay-delay") && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint32_t number = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            const bool host = argument == "--netplay-host";
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                (host ? number == 0u || number > 65535u : number > RECOMP_ROLLBACK_WINDOW)) {
                std::cerr << "recomp runner: --netplay-host needs a UDP port and --netplay-delay at most "
                          << RECOMP_ROLLBACK_WINDOW << " frames\n";
                return 64;
            }
            (host ? launch.hostPort : launch.delay) = number;
            continue;
        }
        if (argument == "--netplay-join" && i + 1 < argc) {
            launch.join = argv[++i];
            continue;
        }
        if (argument == "--replay" && i + 1 < argc) {
            replayPath = argv[++i];
            continue;
        }
        if (argument == "--input-start-pulse-at" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint64_t poll = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                !recomp_input_pulse_source_add_poll(&inputPulses, poll)) {
                std::cerr << "recomp runner: invalid Start poll or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-a-pulse-at" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint64_t poll = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                !recomp_input_pulse_source_add_a_poll(&inputPulses, poll)) {
                std::cerr << "recomp runner: invalid A poll or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-buttons-pulse-at" && i + 2 < argc) {
            const std::string value = argv[++i], maskValue = argv[++i];
            std::uint64_t poll = 0u;
            std::uint16_t mask = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            const auto parsedMask = std::from_chars(maskValue.data(), maskValue.data() + maskValue.size(), mask);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                parsedMask.ec != std::errc() || parsedMask.ptr != maskValue.data() + maskValue.size() ||
                mask == 0u || (mask & 0xff00u) != 0u ||
                !recomp_input_pulse_source_add_buttons_poll(&inputPulses, poll, mask)) {
                std::cerr << "recomp runner: invalid button poll/mask or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-analog-pulse-at" && i + 2 < argc) {
            const std::string value = argv[++i], indexValue = argv[++i];
            std::uint64_t poll = 0u;
            std::uint32_t index = 0u;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), poll);
            const auto parsedIndex = std::from_chars(indexValue.data(), indexValue.data() + indexValue.size(), index);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
                parsedIndex.ec != std::errc() || parsedIndex.ptr != indexValue.data() + indexValue.size() ||
                index >= RECOMP_INPUT_ANALOG_BUTTON_COUNT ||
                !recomp_input_pulse_source_add_analog_poll(&inputPulses, poll,
                    static_cast<std::uint8_t>(index), RECOMP_INPUT_ANALOG_PRESSED)) {
                std::cerr << "recomp runner: invalid analog poll/index or too many pulses\n";
                return 64;
            }
            continue;
        }
        if (argument == "--input-analog-pulse-every" && i + 4 < argc) {
            const std::string firstValue = argv[++i], periodValue = argv[++i], lastValue = argv[++i],
                indexValue = argv[++i];
            std::uint64_t first = 0u, period = 0u, last = 0u;
            std::uint32_t index = 0u;
            const auto parsedFirst = std::from_chars(firstValue.data(), firstValue.data() + firstValue.size(), first);
            const auto parsedPeriod = std::from_chars(periodValue.data(), periodValue.data() + periodValue.size(), period);
            const auto parsedLast = std::from_chars(lastValue.data(), lastValue.data() + lastValue.size(), last);
            const auto parsedIndex = std::from_chars(indexValue.data(), indexValue.data() + indexValue.size(), index);
            if (parsedFirst.ec != std::errc() || parsedFirst.ptr != firstValue.data() + firstValue.size() ||
                parsedPeriod.ec != std::errc() || parsedPeriod.ptr != periodValue.data() + periodValue.size() ||
                parsedLast.ec != std::errc() || parsedLast.ptr != lastValue.data() + lastValue.size() ||
                parsedIndex.ec != std::errc() || parsedIndex.ptr != indexValue.data() + indexValue.size() ||
                index >= RECOMP_INPUT_ANALOG_BUTTON_COUNT ||
                !recomp_input_pulse_source_add_analog_repeat(&inputPulses, first, period, last,
                    static_cast<std::uint8_t>(index))) {
                std::cerr << "recomp runner: invalid analog repeat or too many rules\n";
                return 64;
            }
            continue;
        }
        if ((argument != "--xbe" && argument != "--disc-root" &&
             argument != "--expect-stop" && argument != "--milestone-log") || i + 1 >= argc) {
            std::cerr << "usage: recomp_program_runner --xbe <path> [--disc-root <path>]"
                " [--expect-stop <id>] [--milestone-log <path>]"
                " [--input-start-pulse-at <poll>] [--input-a-pulse-at <poll>]"
                " [--input-buttons-pulse-at <poll> <decimal-mask>]"
                " [--input-analog-pulse-at <poll> <button-index>]"
                " [--input-analog-pulse-every <first-poll> <period> <last-poll> <button-index>]"
                " [--input-script-only] [--story-finale] [--skip-intro] [--no-unlock-all]"
                " [--netplay-host <port> [--netplay-delay <frames>] | --netplay-join <host>:<port> | --replay <file>]\n";
            return 64;
        }
        const std::string value = argv[++i];
        if (argument == "--xbe") xbePath = value;
        else if (argument == "--disc-root") discRoot = value;
        else if (argument == "--expect-stop") expectStop = value;
        else milestoneLog = value;
    }
    if (xbePath.empty()) {
        std::cerr << "recomp runner: --xbe is required\n";
        return 64;
    }
    if ((launch.hostPort != 0u) + !launch.join.empty() + !replayPath.empty() > 1) {
        std::cerr << "recomp runner: --netplay-host, --netplay-join and --replay cannot be combined\n";
        return 64;
    }
    recomp_stop_configure(expectStop.empty() ? nullptr : expectStop.c_str(),
        milestoneLog.empty() ? nullptr : milestoneLog.c_str());
    const auto parsed = doaxbv::XbeParser().parse(std::filesystem::u8path(xbePath));
    if (!parsed.ok()) {
        std::cerr << "recomp runner: " << parsed.errors.front() << '\n';
        return 1;
    }
    if (parsed.metadata.fileSha256 != RECOMP_XBE_SHA256) {
        std::cerr << "recomp runner: XBE SHA-256 differs from the authenticated program target\n";
        return 1;
    }
    xbe = &parsed;
    std::uint8_t *memory = recomp_guest_ram_allocate(RECOMP_XBOX_RAM_SIZE, &guestRamWriteWatch);
    guestRam = memory;
    if (memory == nullptr) {
        std::cerr << "recomp runner: guest RAM allocation failed\n";
        return 1;
    }
    if (!loadImage()) return 1;
    if (discRoot.empty()) discRoot = std::filesystem::absolute(std::filesystem::u8path(xbePath)).parent_path().string();
    const auto discPath = std::filesystem::u8path(discRoot);
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(discPath, filesystemError)) {
        std::cerr << "recomp runner: disc root is not a readable directory\n";
        return 1;
    }
    recomp_disc_root_path = discRoot.c_str();
    recomp_netplay_session_init({discPath, RECOMP_NETPLAY_BUILD, &inputPulses, scriptedInputOnly, guestRam, unlockAll, reboot});
    if ((launch.hostPort != 0u || !launch.join.empty()) && !recomp_netplay_session_connect(launch)) return 1;
    if (!replayPath.empty() && !recomp_netplay_session_load_replay(replayPath)) return 1;
    bootGuest();
}

static void bootGuest()
{
    static bool booted;
    const bool first = !booted;
    booted = true;
    if (!recomp_save_initialize(recomp_disc_root_path)) {
        std::cerr << "recomp runner: save recovery failed; guest not started\n";
        std::exit(2);
    }
    static RecompMemoryRegion region;
    region = {0u, RECOMP_XBOX_RAM_SIZE, guestRam};
    recomp_runtime_init(&region, 1u, nullptr, 0u, nullptr, 0u);
    recomp_runtime_set_lookup(recomp_program_lookup);
    if (!recomp_netplay_session_read_env()) {
        std::cerr << "recomp runner: RECOMP_ROLLBACK must be loopback or udp:<local port>:<peer host>:<peer port>, "
                  << "with decimal frame counts, a lag or delay of at most " << RECOMP_ROLLBACK_WINDOW
                  << " frames, and the UDP port must open\n";
        std::exit(1);
    }
    const bool rollbackEnabled = recomp_netplay_session_enabled();
    // Each boot sets every hook, so nothing from a session that ended carries over.
    recomp_d3d_frame_boundary_hook = nullptr;
    recomp_kernel_profile_mutation_hook = nullptr;
    recomp_xapi_frame_clock_set_enabled(false);
    recomp_kernel_file_set_synchronous_reads(false);
    recomp_dsound_buffer_set_game_thread_pump(false);
    if (recomp_synctest_enabled() || rollbackEnabled) {
        const char *clock = std::getenv("RECOMP_FRAME_CLOCK");
        if (clock == nullptr || std::strcmp(clock, "0") != 0)
            recomp_xapi_frame_clock_set_enabled(true);
        recomp_synctest_set_guest_ram(guestRam, RECOMP_XBOX_RAM_SIZE, guestRamWriteWatch);
        recomp_kernel_file_set_synchronous_reads(true);
        recomp_synctest_install_default_modules();
        if (!rollbackEnabled) {
            recomp_synctest_set_extra_state(sizeof inputPulses, saveInputPulses, restoreInputPulses);
            recomp_d3d_frame_boundary_hook = onFrameBoundary;
        }
    }
    recomp_input_adapter_reset();
    recomp_input_adapter_set_source(sampleInput);
    recomp_d3d_present_hook = onSwap;
    recomp_d3d_overlay_hooks[RECOMP_D3D_OVERLAY_PERF] = recomp_perf_overlay;
    recomp_netplay_session_install(recomp_synctest_enabled());
    /* The free camera and the scene freeze run only with RECOMP_FREE_CAMERA=1,
       and never online; off, input samples and task switches go unhooked.
       Random CPU costumes are off online too, where both games must agree. */
    const bool online = recomp_netplay_session_enabled() || recomp_synctest_enabled();
    recomp_free_camera_install(online);
    recomp_cpu_costume_install(online);
    recomp_combo_hud_install(online);
    recomp_unlock_profile_reset();
    recomp_runner_assets_reset();
    installPcSettings(online);
    recomp_menu_quit_install();
    if (const char *card = std::getenv("RECOMP_DEDICATION_CARD"); first && card != nullptr && *card != '\0') {
        if (loadCard(card)) recomp_kernel_read_hook = noteRead;
        else std::fprintf(stderr, "recomp runner: dedication card is not a 24/32-bit BMP; skipped\n");
    }
    if (!scriptedInputOnly) recomp_input_adapter_set_feedback_sink(recomp_netplay_session_set_vibration);
    if (first) {
#ifdef _WIN32
        /* Guest frame waits that lost a 15.6 ms scheduler tick dropped frames on
           a power-managed handheld (issue #69): ask for a 1 ms tick, keep Windows
           from throttling the process, and run the game thread above normal. */
        timeBeginPeriod(1u);
        PROCESS_POWER_THROTTLING_STATE throttling{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0u};
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);
        // Separate call: Windows 10 rejects the Windows 11 timer flag.
        throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        std::atexit(recomp_netplay_session_exit);
        std::atexit(recomp_input_host_stop_vibration);
        std::atexit(recomp_d3d8_shutdown);
        std::atexit(recomp_dsound_api_shutdown);
        recomp_watchdog_start();
    }
    recomp_runtime.registers.esp = XBOX_STARTUP_STACK_TOP;
    std::fprintf(stderr, "recomp runner: entry=0x%08x image_end=0x%08x native-d3d8-dsound\n",
        xbe->metadata.selectedEntryPoint,
        static_cast<unsigned>(xbe->metadata.header.baseAddress + xbe->metadata.header.sizeImage));
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    recomp_dispatch_indirect_site(xbe->metadata.selectedEntryPoint,
        XBOX_STARTUP_STACK_TOP, __FILE__, __LINE__);
    recomp_kernel_run_threads();
    recomp_stop(0, "program-returned");
}
