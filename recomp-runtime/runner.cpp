/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_diagnostics.h"
#include "d3d8_adapter.h"
#include "dsound_api_adapter.h"
#include "free_camera_adapter.h"
#include "input_adapter.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "runner_load.h"
#include "save_transaction.h"
#include "d3d_vblank.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
extern "C" {
#include "kernel_abi.h"
#include "program_manual.h"
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

static RecompInputPulseSource inputPulses;
static bool scriptedInputOnly;

static bool sampleInput(std::uint32_t port, RecompInputGamepad *gamepad)
{
    if (port != 0u) return !scriptedInputOnly && recomp_input_host_sample(port, gamepad);
    const bool sampled = recomp_input_pulse_source_sample(&inputPulses, gamepad);
    static bool startHeld;
    const bool start = sampled && (gamepad->buttons & RECOMP_INPUT_BUTTON_START) != 0u;
    if (start != startHeld) {
        std::fprintf(stderr, "recomp input: Start %s poll=%llu\n", start ? "down" : "up",
            static_cast<unsigned long long>(inputPulses.sample_count));
        startHeld = start;
    }
    return sampled;
}

/* Offer every costume and character. 0x87cfa8 points at the live profile.
   The game's own award loops (0xdc640, 0xdc100) set each costume byte at
   +0x2ed8 to that character's costume count from 0x6e0f08 (the select screen
   shows min(byte, count)), and the three character flags at +0x2ed2 to 1.
   Repeating them each frame also covers profiles loaded later. */
static bool unlockAll = true; // --no-unlock-all leaves unlocks to play
static void unlockProfile(std::uint32_t)
{
    const std::uint32_t profile = *recomp_memory_u32(0x0087cfa8u);
    if (profile == 0u) return;
    for (std::uint32_t i = 0; i < 15u; ++i) {
        const std::uint8_t character = *recomp_memory(0x006959dcu + i, 1u);
        const std::uint8_t slot = *recomp_memory(0x006959c0u + character, 1u);
        *recomp_memory(profile + 0x2ed8u + slot, 1u) = *recomp_memory(0x006e0f08u + character, 1u);
    }
    std::memset(recomp_memory(profile + 0x2ed2u, 3u), 1, 3u);
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

// Holds the guest at a Swap: 1.5 s fade in, hold, 1.5 s fade out over 10 s.
static void showCard()
{
    using namespace std::chrono;
    constexpr auto fade = milliseconds(1500), total = milliseconds(10000);
    const char *wide = std::getenv("RECOMP_D3D_WIDESCREEN");
    const float aspect = wide != nullptr && std::strcmp(wide, "0") != 0 ? 16.0f / 9.0f : 4.0f / 3.0f;
    std::fprintf(stderr, "recomp runner: staff roll starts; dedication card shown\n");
    const auto start = steady_clock::now();
    for (auto next = start;;) {
        const auto elapsed = steady_clock::now() - start;
        if (elapsed >= total) break;
        const auto edge = std::min<steady_clock::duration>(elapsed, total - elapsed);
        const auto level = static_cast<std::uint8_t>(edge >= fade ? 255 : 255 * edge / fade);
        recomp_d3d8_present_card(cardPixels.data(), cardWidth, cardHeight, aspect, level);
        next += microseconds(16667);
        recomp_d3d_sleep_until(duration_cast<nanoseconds>(next.time_since_epoch()).count());
    }
    recomp_xapi_hold_time(static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now() - start).count()));
}

static void onSwap(std::uint32_t frame)
{
    if (unlockAll) unlockProfile(frame);
    if (storyFinale) applyStoryFinale();
    if (!cardPending) return;
    cardPending = false;
    showCard();
}

int main(int argc, char **argv)
{
    recomp_install_host_diagnostics();
    recomp_d3d_frame_breakdown = recomp_kernel_thread_times;
    std::string xbePath, discRoot, expectStop, milestoneLog;
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
                " [--input-script-only] [--story-finale] [--skip-intro] [--no-unlock-all]\n";
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
    std::vector<std::uint8_t> memory(RECOMP_XBOX_RAM_SIZE, 0u);
    std::string error;
    if (!recomp_load_xbe(parsed, memory, error)) {
        std::cerr << "recomp runner: " << error << '\n';
        return 1;
    }
    const auto imageEnd = static_cast<std::uint64_t>(parsed.metadata.header.baseAddress) +
        parsed.metadata.header.sizeImage;
    if (!xbox_HeapSetImageEnd(static_cast<std::uint32_t>(imageEnd))) {
        std::cerr << "recomp runner: invalid heap image boundary\n";
        return 1;
    }
    if (discRoot.empty()) discRoot = std::filesystem::absolute(std::filesystem::u8path(xbePath)).parent_path().string();
    const auto discPath = std::filesystem::u8path(discRoot);
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(discPath, filesystemError)) {
        std::cerr << "recomp runner: disc root is not a readable directory\n";
        return 1;
    }
    recomp_disc_root_path = discRoot.c_str();
    if (!recomp_save_initialize(recomp_disc_root_path)) {
        std::cerr << "recomp runner: save recovery failed; guest not started\n";
        return 2;
    }
    const RecompMemoryRegion region = {0u, memory.size(), memory.data()};
    recomp_runtime_init(&region, 1u, nullptr, 0u, nullptr, 0u);
    recomp_runtime_set_lookup(recomp_program_lookup);
    recomp_input_adapter_reset();
    recomp_input_adapter_set_source(sampleInput);
    recomp_d3d_present_hook = onSwap;
    /* The free camera and the scene freeze run only with RECOMP_FREE_CAMERA=1;
       off, input samples and task switches go unhooked. */
    recomp_free_camera_install();
    if (const char *card = std::getenv("RECOMP_DEDICATION_CARD"); card != nullptr && *card != '\0') {
        if (loadCard(card)) recomp_kernel_read_hook = noteRead;
        else std::fprintf(stderr, "recomp runner: dedication card is not a 24/32-bit BMP; skipped\n");
    }
    if (!scriptedInputOnly) recomp_input_adapter_set_feedback_sink(recomp_input_host_set_vibration);
    std::atexit(recomp_input_host_stop_vibration);
    std::atexit(recomp_d3d8_shutdown);
    std::atexit(recomp_dsound_api_shutdown);
    recomp_runtime.registers.esp = XBOX_STARTUP_STACK_TOP;
    std::fprintf(stderr, "recomp runner: entry=0x%08x image_end=0x%08x native-d3d8-dsound\n",
        parsed.metadata.selectedEntryPoint, static_cast<unsigned>(imageEnd));
    recomp_watchdog_start();
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    recomp_dispatch_indirect_site(parsed.metadata.selectedEntryPoint,
        XBOX_STARTUP_STACK_TOP, __FILE__, __LINE__);
    recomp_kernel_run_threads();
    recomp_stop(0, "program-returned");
}
