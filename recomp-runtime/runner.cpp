/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_diagnostics.h"
#include "d3d8_adapter.h"
#include "dsound_api_adapter.h"
#include "dsound_buffer_adapter.h"
#include "guest_snapshot.h"
#include "fiber_adapter.h"
#include "input_adapter.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "netplay.h"
#include "rollback.h"
#include "runner_load.h"
#include "save_transaction.h"
#include "sha.h"
#include "synctest.h"
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
#include <cctype>
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
#include <thread>
#include <vector>
#include <charconv>
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static RecompInputPulseSource inputPulses;
static bool scriptedInputOnly;
// SyncTest's RAM snapshots read this; write tracking is fixed when guest RAM is allocated.
static bool guestRamWriteWatch;
static std::uint8_t *guestRam;

/* Rollback over a fight (docs/rollback-netcode.md), from swap
   RECOMP_ROLLBACK_START. RECOMP_ROLLBACK=loopback plays a generated remote
   player on port 0 whose pads arrive RECOMP_ROLLBACK_LAG frames late.
   RECOMP_ROLLBACK=udp:<local port>:<peer host>:<peer port> plays a peer runner;
   RECOMP_ROLLBACK_PLAYER (0 or 1) picks the local port and
   RECOMP_ROLLBACK_DELAY the input delay, both matching on the two sides. At
   session frame RECOMP_ROLLBACK_FRAMES every pad is confirmed and guest RAM
   goes to RECOMP_ROLLBACK_DUMP. --netplay-host and --netplay-join start a
   lobby session from the first frame instead. */
enum class RollbackMode { Off, Loopback, Udp };
struct RollbackRun {
    RollbackMode mode = RollbackMode::Off;
    bool active = false;
    bool hidden = false;
    bool lobby = false; // lockstep menus, rollback over live rounds
    std::uint32_t start = 0u, stop = UINT32_MAX, lag = 4u, delay = 0u, player = 0u;
    std::uint32_t frontier = 0u; // first session frame never run
    std::uint32_t rollbacks = 0u, replayed = 0u, waited = 0u;
    std::uint32_t wait = 0u, paced = UINT32_MAX; // frame sync's next wait; last frame paced
    std::chrono::steady_clock::time_point next_frame{};
    // Measured once per new frame and repeated while stalled, so a stall's
    // inflated advantage never reaches the peer's frame sync.
    std::int32_t advantage = 0;
    std::chrono::steady_clock::time_point started;
    const char *dump = nullptr;
    RecompRollbackSession session;
    RecompNetplay net;
    RecompInputGamepad pads[2]{}; // the running frame's pads by guest port
};
static RollbackRun rollback;

static bool sampleInput(std::uint32_t port, RecompInputGamepad *gamepad)
{
    // Both runners must see the same controllers: the session's two ports, once it runs.
    if (rollback.lobby && (port >= 2u || !rollback.active)) return false;
    if (rollback.active && port < 2u) {
        // Loopback has no local player, so its port stays empty.
        if (rollback.mode == RollbackMode::Loopback && port == rollback.player) return false;
        *gamepad = rollback.pads[port];
        return true;
    }
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

// A new pad nearly every frame, so repeating the last pad mispredicts.
static RecompInputGamepad generatedPad(std::uint32_t frame)
{
    std::uint32_t x = frame * 2654435761u;
    x ^= x >> 13;
    RecompInputGamepad pad{};
    pad.buttons = static_cast<std::uint16_t>(x & 0x000fu); // d-pad
    pad.analog_buttons[(x >> 4) & 3u] = (x & 0x40u) != 0u ? 0xffu : 0u; // A, B, X or Y
    return pad;
}

static bool roundLive();

static RecompInputGamepad localPad(std::uint32_t frame)
{
    RecompInputGamepad pad{};
    if (rollback.mode == RollbackMode::Loopback) return pad;
    if (scriptedInputOnly) {
        // Scripted runs play a generated stream per player so two runners differ.
        if (!rollback.lobby) return generatedPad(frame * 2u + rollback.player);
        // A scripted lobby: each side walks the menus with its input script, one
        // sample per frame, and the joiner mashes in live rounds.
        if (rollback.player == 1u && roundLive()) pad = generatedPad(frame);
        else (void)recomp_input_pulse_source_sample(&inputPulses, &pad);
        return pad;
    }
    (void)recomp_input_host_sample(0u, &pad);
    return pad;
}

// The local player's port drives the local pad; the peer's port and hidden frames stay still.
static void setVibration(std::uint32_t port, std::uint16_t left, std::uint16_t right)
{
    if (!rollback.active) {
        recomp_input_host_set_vibration(port, left, right);
        return;
    }
    // shortcut: a motor change made only in a hidden frame is dropped until the next visible one.
    if (port == rollback.player && !rollback.hidden) recomp_input_host_set_vibration(0u, left, right);
}

static void receivePads()
{
    RecompRollbackSession &s = rollback.session;
    if (rollback.mode == RollbackMode::Udp) {
        recomp_netplay_receive(&rollback.net, &s);
        return;
    }
    const std::uint32_t lag = s.frame >= rollback.stop ? 0u : rollback.lag;
    while (s.remote_count + lag <= s.frame) {
        const RecompInputGamepad pad = generatedPad(s.remote_count);
        recomp_rollback_add_remote(&s, &pad);
    }
}

/* At the newest frame: gathers pads until the frame may run or a rewind is
   due, and returns the frame to load or RECOMP_ROLLBACK_NONE. */
static std::uint32_t awaitFrontier(bool last)
{
    using Clock = std::chrono::steady_clock;
    RecompRollbackSession &s = rollback.session;
    const bool udp = rollback.mode == RollbackMode::Udp;
    Clock::time_point sent{};
    const Clock::time_point began = Clock::now();
    for (;;) {
        receivePads();
        // The local pad is read at the frame's start, so pacing adds no input lag.
        // Rewinds wait for it too, so one replay covers every pad that arrived.
        const bool started = Clock::now() >= rollback.next_frame;
        const std::uint32_t target = started ? recomp_rollback_take_rewind(&s) : RECOMP_ROLLBACK_NONE;
        if (target != RECOMP_ROLLBACK_NONE) return target;
        if (started) {
            while (s.local_count <= s.frame + rollback.delay) {
                const RecompInputGamepad pad = localPad(s.local_count);
                recomp_rollback_add_local(&s, &pad);
            }
        }
        const bool ready = last
            ? s.remote_count >= rollback.stop && (!udp || rollback.net.peer_ack >= rollback.stop)
            : started && recomp_rollback_can_run(&s) && (!udp || rollback.net.peer_seen);
        if (ready) return RECOMP_ROLLBACK_NONE;
        if (!udp) recomp_stop(4, "rollback:stalled");
        const Clock::time_point now = Clock::now();
        if (now - sent >= std::chrono::milliseconds(4)) {
            recomp_netplay_send(&rollback.net, &s, rollback.advantage);
            sent = now;
        }
        // The first contact waits out the other runner's boot.
        if (now - began > std::chrono::seconds(rollback.net.peer_seen ? 5 : 120))
            recomp_stop(4, "rollback:peer-timeout");
        // sleep_for can round up to the 15.6 ms Windows timer tick; this timer does not.
        Clock::time_point until = now + std::chrono::milliseconds(2);
        if (rollback.next_frame > now && rollback.next_frame < until) until = rollback.next_frame;
        recomp_d3d_sleep_until(std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
    }
}

static void finishRollback()
{
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - rollback.started).count();
    std::fprintf(stderr, "recomp rollback: frame %u reached after %u rollbacks "
        "(%u frames run again, %u waited) in %.1f s\n",
        rollback.session.frame, rollback.rollbacks, rollback.replayed, rollback.waited, seconds);
    if (rollback.dump != nullptr) {
        std::vector<std::uint8_t> ram(guestRam, guestRam + RECOMP_XBOX_RAM_SIZE);
        recomp_synctest_mask_ignored(ram.data(), ram.size());
        std::ofstream out(rollback.dump, std::ios::binary);
        out.write(reinterpret_cast<const char *>(ram.data()), static_cast<std::streamsize>(ram.size()));
        if (!out) recomp_stop(4, "rollback:dump");
    }
    if (rollback.mode == RollbackMode::Udp) {
        // shortcut: the peer may still need our acknowledgement; repeat it briefly instead of a close handshake.
        for (int i = 0; i < 50; ++i) {
            recomp_netplay_send(&rollback.net, &rollback.session, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    recomp_stop(0, "rollback:done");
}

/* Schedules each new frame on a 60 Hz grid. Display backpressure alone let the
   guest regain a frame sync wait from the presenter queue; a wait moves the grid. */
static void scheduleFrame()
{
    using namespace std::chrono;
    constexpr steady_clock::duration interval = nanoseconds(1000000000 / 60);
    const steady_clock::time_point now = steady_clock::now();
    // Late frames may catch up by the two the presenter queues.
    rollback.next_frame = std::max(rollback.next_frame + interval, now - 2 * interval);
    if (rollback.wait != 0u) rollback.next_frame = std::max(rollback.next_frame, now) + rollback.wait * interval;
    rollback.wait = 0u;
}

/* A lobby session predicts only while a round is live: the game-mode task in
   its match state (g_game_mode_state, 0x8a56d0, is 5), time left
   (g_round_timer_seconds, 0x87ae08) and both fighters' life (0x87ae54,
   stride 0x94) above 0. A KO or time-over ends it seconds before the save
   at the end of a match, so menus and saves run in lockstep, which never
   rewinds. */
static bool roundLive()
{
    if (*recomp_memory(0x008a56d0u, 1u) != 5u || *recomp_memory(0x0087ae08u, 1u) == 0u) return false;
    for (std::uint32_t side = 0; side < 2u; ++side) {
        const std::uint8_t *life = recomp_memory(0x0087ae54u + side * 0x94u, 2u);
        if (static_cast<std::int16_t>(life[0] | life[1] << 8) <= 0) return false;
    }
    return true;
}

static void setWindow(std::uint32_t window)
{
    RecompRollbackSession &s = rollback.session;
    if (s.window == window) return;
    s.window = window;
    std::fprintf(stderr, "recomp netplay: %s from frame %u\n", window != 0u ? "rollback" : "lockstep", s.frame);
}

static void onRollbackFrame(std::uint32_t swap)
{
    RecompRollbackSession &s = rollback.session;
    if (!rollback.active) {
        if (swap < rollback.start || recomp_fiber_adapter_model()->current_handle == 0u) return;
        recomp_rollback_init(&s, rollback.delay);
        if (rollback.lobby) s.window = 0u;
        if (!recomp_rollback_ring_init(RECOMP_ROLLBACK_WINDOW)) recomp_stop(4, "rollback:ring");
        rollback.active = true;
        rollback.started = std::chrono::steady_clock::now();
        std::fprintf(stderr, "recomp rollback: session starts at swap %u as player %u\n",
            swap, rollback.player);
    }
    bool saved = true;
    if (s.frame == rollback.frontier) {
        if (rollback.lobby) setWindow(roundLive() ? RECOMP_ROLLBACK_WINDOW : 0u);
        if (rollback.mode == RollbackMode::Udp && rollback.paced != s.frame) {
            scheduleFrame();
            rollback.paced = s.frame;
        }
        const std::uint32_t target = awaitFrontier(s.frame == rollback.stop);
        if (target != RECOMP_ROLLBACK_NONE) {
            // The newest save is the frame before the frontier.
            // shortcut: a save that cannot be rewound ends the run; netplay must drop to lockstep before the game saves.
            if (!recomp_rollback_ring_load(rollback.frontier - 1u - target))
                recomp_stop(4, "rollback:unrestorable");
            ++rollback.rollbacks;
            rollback.replayed += rollback.frontier - target;
        } else if (s.frame == rollback.stop) {
            finishRollback();
            return;
        } else {
            saved = recomp_rollback_ring_save();
            if (rollback.mode == RollbackMode::Udp) {
                rollback.advantage = static_cast<std::int32_t>(s.frame - rollback.net.peer_frame);
                recomp_netplay_send(&rollback.net, &s, rollback.advantage);
                const std::uint32_t wait = recomp_netplay_wait_frames(&rollback.net, rollback.advantage);
                rollback.waited += wait;
                rollback.wait = wait;
            }
        }
    } else {
        saved = recomp_rollback_ring_save();
    }
    if (!saved) recomp_stop(4, "rollback:save");
    // Frames run again were already shown and heard with the predicted pads.
    rollback.hidden = s.frame < rollback.frontier;
    recomp_d3d8_set_present_suppressed(rollback.hidden);
    recomp_d3d8_set_gpu_work_suppressed(rollback.hidden);
    recomp_dsound_buffer_set_output_suppressed(rollback.hidden);
    recomp_rollback_run(&s, &rollback.pads[rollback.player], &rollback.pads[1u - rollback.player]);
    if (s.frame > rollback.frontier) rollback.frontier = s.frame;
}

static bool readRollbackConfig()
{
    const char *mode = std::getenv("RECOMP_ROLLBACK");
    if (mode == nullptr || *mode == '\0' || std::strcmp(mode, "0") == 0) return true;
    const auto number = [](const char *name, std::uint32_t &value) {
        if (const char *text = std::getenv(name); text != nullptr && *text != '\0')
            value = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
    };
    number("RECOMP_ROLLBACK_START", rollback.start);
    number("RECOMP_ROLLBACK_FRAMES", rollback.stop);
    rollback.dump = std::getenv("RECOMP_ROLLBACK_DUMP");
    if (std::strcmp(mode, "loopback") == 0) {
        rollback.mode = RollbackMode::Loopback;
        rollback.player = 1u;
        if (std::getenv("RECOMP_ROLLBACK_FRAMES") == nullptr) rollback.stop = 600u;
        number("RECOMP_ROLLBACK_LAG", rollback.lag);
        return rollback.lag <= RECOMP_ROLLBACK_WINDOW;
    }
    // udp:<local port>:<peer host>:<peer port>
    if (std::strncmp(mode, "udp:", 4u) != 0) return false;
    const std::string spec = mode + 4;
    const std::size_t hostAt = spec.find(':'), portAt = spec.rfind(':');
    if (hostAt == std::string::npos || portAt == hostAt) return false;
    const unsigned long localPort = std::strtoul(spec.substr(0u, hostAt).c_str(), nullptr, 10);
    const unsigned long peerPort = std::strtoul(spec.substr(portAt + 1u).c_str(), nullptr, 10);
    const std::string host = spec.substr(hostAt + 1u, portAt - hostAt - 1u);
    number("RECOMP_ROLLBACK_PLAYER", rollback.player);
    number("RECOMP_ROLLBACK_DELAY", rollback.delay);
    if (localPort == 0u || localPort > 65535u || peerPort == 0u || peerPort > 65535u ||
        rollback.player > 1u || rollback.delay > RECOMP_ROLLBACK_WINDOW)
        return false;
    rollback.mode = RollbackMode::Udp;
    return recomp_netplay_open(&rollback.net, static_cast<std::uint16_t>(localPort), host.c_str(),
        static_cast<std::uint16_t>(peerPort));
}

// --netplay-host <port> or --netplay-join <host>:<port>, and the host's --netplay-delay.
static std::uint32_t netplayHostPort = 0u, netplayDelay = 2u;
static std::string netplayJoin;
enum : std::uint8_t { LobbyUnlockAll = 1u, LobbySkipIntro = 2u };

// SHA-256 of this runner's executable, so both sides of a lobby run one build.
static std::string runnerDigest()
{
    std::filesystem::path path;
#ifdef _WIN32
    std::wstring buffer(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0u && length < buffer.size()) path = buffer.substr(0u, length);
#elif defined(__APPLE__)
    char buffer[4096];
    std::uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) == 0) path = buffer;
#else
    std::error_code error;
    path = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (path.empty() || !file) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
    return doaxbv::sha256Hex(bytes);
}

/* Partition 1 (UDATA profiles, TDATA title saves) as one blob: the SHA-256 of
   the rest, then per entry a kind (0 folder, 1 file), a 2-byte path length,
   the path below partition 1 and, for a file, a 4-byte size and its bytes. */
static bool packProfile(const std::filesystem::path &root, std::vector<std::uint8_t> &blob)
{
    namespace fs = std::filesystem;
    std::vector<std::uint8_t> body;
    const auto put = [&body](std::size_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) body.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    };
    std::error_code error;
    if (fs::exists(root, error)) {
        for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            const fs::file_status status = it->symlink_status(error);
            const bool folder = fs::is_directory(status);
            if (error || (!folder && !fs::is_regular_file(status))) return false;
            const std::string name = it->path().lexically_relative(root).generic_string();
            body.push_back(folder ? 0u : 1u);
            put(name.size(), 2u);
            body.insert(body.end(), name.begin(), name.end());
            if (folder) continue;
            const std::uintmax_t size = fs::file_size(it->path(), error);
            if (error || size > RECOMP_NETPLAY_MAX_PROFILE) return false;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::ifstream file(it->path(), std::ios::binary);
            if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
            put(bytes.size(), 4u);
            body.insert(body.end(), bytes.begin(), bytes.end());
        }
    }
    if (error) return false;
    const std::string hash = doaxbv::sha256Hex(body);
    blob.assign(hash.begin(), hash.end());
    blob.insert(blob.end(), body.begin(), body.end());
    return true;
}

// A path from the host: relative, below UDATA or TDATA, plain names, no Windows device names.
static bool safeProfilePath(const std::string &name)
{
    std::size_t start = 0u;
    for (bool first = true;; first = false) {
        const std::size_t slash = name.find('/', start);
        const std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (const char c : part)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
        if (first && part != "UDATA" && part != "TDATA") return false;
        std::string stem = part.substr(0u, part.find('.'));
        for (char &c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4u && (stem.rfind("COM", 0u) == 0u || stem.rfind("LPT", 0u) == 0u) &&
                std::isdigit(static_cast<unsigned char>(stem[3]))))
            return false;
        if (slash == std::string::npos) return true;
        start = slash + 1u;
    }
}

// Writes a packProfile blob below root; false for a damaged or unsafe one.
static bool unpackProfile(const std::uint8_t *blob, std::size_t size, const std::filesystem::path &root)
{
    namespace fs = std::filesystem;
    if (size < 64u) return false;
    const std::vector<std::uint8_t> body(blob + 64, blob + size);
    if (doaxbv::sha256Hex(body) != std::string(reinterpret_cast<const char *>(blob), 64u)) return false;
    std::size_t at = 0u;
    const auto get = [&](unsigned bytes, std::uint32_t &value) {
        if (body.size() - at < bytes) return false;
        value = 0u;
        for (unsigned i = 0; i < bytes; ++i) value |= static_cast<std::uint32_t>(body[at++]) << (8u * i);
        return true;
    };
    std::error_code error;
    fs::create_directories(root, error);
    for (unsigned entries = 0u; !error && at < body.size(); ++entries) {
        std::uint32_t kind, length, bytes;
        if (entries == 512u || !get(1u, kind) || kind > 1u || !get(2u, length) || length > 200u ||
            body.size() - at < length)
            return false;
        const std::string name(body.begin() + static_cast<std::ptrdiff_t>(at),
            body.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (!safeProfilePath(name)) return false;
        const fs::path path = root / fs::path(name);
        if (kind == 0u) {
            fs::create_directories(path, error);
            continue;
        }
        if (!get(4u, bytes) || body.size() - at < bytes) return false;
        fs::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char *>(body.data() + at), bytes)) return false;
        at += bytes;
    }
    return !error;
}

/* Connects the two runners before either boots. The host's input delay and
   options win, and both sides play from a fresh copy of the host's profile,
   so they start alike and neither player's own profile changes. */
static bool joinLobby(const std::filesystem::path &discPath)
{
    if (const char *mode = std::getenv("RECOMP_ROLLBACK"); mode != nullptr && *mode != '\0') {
        std::cerr << "recomp runner: RECOMP_ROLLBACK cannot be combined with --netplay-host or --netplay-join\n";
        return false;
    }
    RecompNetplayLobby lobby{};
    const std::string digest = runnerDigest();
    if (digest.size() != sizeof lobby.digest) {
        std::cerr << "recomp runner: cannot read this runner to identify its build\n";
        return false;
    }
    std::memcpy(lobby.digest, digest.data(), sizeof lobby.digest);
    lobby.delay = static_cast<std::uint8_t>(netplayDelay);
    lobby.flags = static_cast<std::uint8_t>((unlockAll ? LobbyUnlockAll : 0u) | (recomp_skip_intro ? LobbySkipIntro : 0u));
    const bool hosting = netplayHostPort != 0u;
    const std::filesystem::path storage = discPath / ".recomp-storage";
    std::vector<std::uint8_t> profile;
    if (hosting) {
        // Recovery first, so an interrupted save is never sent.
        if (!recomp_save_initialize(recomp_disc_root_path) || !packProfile(storage / "partition1", profile) ||
            profile.size() > RECOMP_NETPLAY_MAX_PROFILE) {
            std::cerr << "recomp runner: cannot read the profile to send, or it is over 1 MiB\n";
            return false;
        }
        lobby.profile_size = static_cast<std::uint32_t>(profile.size());
    } else {
        profile.resize(RECOMP_NETPLAY_MAX_PROFILE);
    }
    RecompNetplayLobbyResult result;
    if (hosting) {
        std::fprintf(stderr, "recomp netplay: hosting on UDP port %u; waiting for the other player\n", netplayHostPort);
        result = recomp_netplay_host(&rollback.net, static_cast<std::uint16_t>(netplayHostPort), &lobby,
            profile.data(), 10u * 60u * 1000u);
    } else {
        const std::size_t colon = netplayJoin.rfind(':');
        const unsigned long port = colon == std::string::npos ? 0u : std::strtoul(netplayJoin.c_str() + colon + 1u, nullptr, 10);
        if (colon == 0u || port == 0u || port > 65535u) {
            std::cerr << "recomp runner: --netplay-join needs <host>:<port>\n";
            return false;
        }
        std::fprintf(stderr, "recomp netplay: joining %s\n", netplayJoin.c_str());
        result = recomp_netplay_join(&rollback.net, netplayJoin.substr(0u, colon).c_str(),
            static_cast<std::uint16_t>(port), &lobby, profile.data(), 2u * 60u * 1000u);
    }
    if (result != RECOMP_NETPLAY_LOBBY_OK) {
        std::cerr << "recomp runner: netplay " << (result == RECOMP_NETPLAY_LOBBY_MISMATCH
            ? "refused: the other player runs a different build"
            : result == RECOMP_NETPLAY_LOBBY_TIMEOUT ? "timed out" : "could not open its UDP port") << '\n';
        return false;
    }
    if (lobby.delay > RECOMP_ROLLBACK_WINDOW) return false;
    unlockAll = (lobby.flags & LobbyUnlockAll) != 0u;
    recomp_skip_intro = (lobby.flags & LobbySkipIntro) != 0u;
    rollback.mode = RollbackMode::Udp;
    rollback.lobby = true;
    rollback.player = hosting ? 0u : 1u;
    rollback.delay = lobby.delay;
    rollback.start = 0u;
    // Test hooks, as for RECOMP_ROLLBACK.
    if (const char *frames = std::getenv("RECOMP_ROLLBACK_FRAMES"); frames != nullptr && *frames != '\0')
        rollback.stop = static_cast<std::uint32_t>(std::strtoul(frames, nullptr, 10));
    rollback.dump = std::getenv("RECOMP_ROLLBACK_DUMP");
    std::error_code error;
    std::filesystem::remove_all(storage / "netplay", error);
    if (error) {
        std::cerr << "recomp runner: cannot clear the netplay save folder\n";
        return false;
    }
    if (!unpackProfile(profile.data(), lobby.profile_size, storage / "netplay" / "partition1")) {
        std::cerr << "recomp runner: the host's profile is damaged or unsafe\n";
        return false;
    }
    recomp_save_session = "netplay";
    std::fprintf(stderr, "recomp netplay: connected as player %u with %u frames of input delay\n",
        rollback.player + 1u, rollback.delay);
    return true;
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
            (host ? netplayHostPort : netplayDelay) = number;
            continue;
        }
        if (argument == "--netplay-join" && i + 1 < argc) {
            netplayJoin = argv[++i];
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
                " [--netplay-host <port> [--netplay-delay <frames>] | --netplay-join <host>:<port>]\n";
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
    if (netplayHostPort != 0u && !netplayJoin.empty()) {
        std::cerr << "recomp runner: --netplay-host and --netplay-join cannot be combined\n";
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
    std::vector<std::uint8_t> image(RECOMP_XBOX_RAM_SIZE, 0u);
    std::string error;
    if (!recomp_load_xbe(parsed, image, error)) {
        std::cerr << "recomp runner: " << error << '\n';
        return 1;
    }
    std::uint8_t *memory = recomp_guest_ram_allocate(RECOMP_XBOX_RAM_SIZE, &guestRamWriteWatch);
    guestRam = memory;
    if (memory == nullptr) {
        std::cerr << "recomp runner: guest RAM allocation failed\n";
        return 1;
    }
    std::memcpy(memory, image.data(), image.size());
    std::vector<std::uint8_t>().swap(image);
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
    if ((netplayHostPort != 0u || !netplayJoin.empty()) && !joinLobby(discPath)) return 1;
    if (!recomp_save_initialize(recomp_disc_root_path)) {
        std::cerr << "recomp runner: save recovery failed; guest not started\n";
        return 2;
    }
    const RecompMemoryRegion region = {0u, RECOMP_XBOX_RAM_SIZE, memory};
    recomp_runtime_init(&region, 1u, nullptr, 0u, nullptr, 0u);
    recomp_runtime_set_lookup(recomp_program_lookup);
    if (!rollback.lobby && !readRollbackConfig()) {
        std::cerr << "recomp runner: RECOMP_ROLLBACK must be loopback or udp:<local port>:<peer host>:<peer port>, "
                  << "with a lag or delay of at most " << RECOMP_ROLLBACK_WINDOW
                  << " frames, and the UDP port must open\n";
        return 1;
    }
    const bool rollbackEnabled = rollback.mode != RollbackMode::Off;
    if (recomp_synctest_enabled() || rollbackEnabled) {
        const char *clock = std::getenv("RECOMP_FRAME_CLOCK");
        if (clock == nullptr || std::strcmp(clock, "0") != 0)
            recomp_xapi_frame_clock_set_enabled(true);
        recomp_synctest_set_guest_ram(memory, RECOMP_XBOX_RAM_SIZE, guestRamWriteWatch);
        recomp_kernel_file_set_synchronous_reads(true);
        recomp_synctest_install_default_modules();
        if (rollbackEnabled) {
            // Rollback input lives in the session, outside the saved state.
            recomp_d3d_frame_boundary_hook = onRollbackFrame;
        } else {
            recomp_synctest_set_extra_state(sizeof inputPulses, saveInputPulses, restoreInputPulses);
            recomp_d3d_frame_boundary_hook = onFrameBoundary;
        }
    }
    recomp_input_adapter_reset();
    recomp_input_adapter_set_source(sampleInput);
    recomp_d3d_present_hook = onSwap;
    if (const char *card = std::getenv("RECOMP_DEDICATION_CARD"); card != nullptr && *card != '\0') {
        if (loadCard(card)) recomp_kernel_read_hook = noteRead;
        else std::fprintf(stderr, "recomp runner: dedication card is not a 24/32-bit BMP; skipped\n");
    }
    if (!scriptedInputOnly) recomp_input_adapter_set_feedback_sink(setVibration);
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
