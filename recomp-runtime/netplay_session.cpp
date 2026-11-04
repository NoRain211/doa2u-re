/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_session.h"

#include "d3d8_adapter.h"
#include "d3d_vblank.h"
#include "dsound_buffer_adapter.h"
#include "fiber_adapter.h"
#include "game_state.h"
#include "input_host_win32.h"
#include "input_keyboard_win32.h"
#include "netplay.h"
#include "netplay_menu.h"
#include "netplay_profile.h"
#include "overlay.h"
#include "replay.h"
#include "rollback.h"
#include "save_transaction.h"
#include "synctest.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
extern "C" {
#include "kernel_abi.h"
#include "program_manual.h"
#include "stop_report.h"
}

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

static RecompNetplaySessionHost host;

/* Rollback over a fight (docs/rollback-netcode.md), from swap
   RECOMP_ROLLBACK_START. RECOMP_ROLLBACK=loopback plays a generated remote
   player on port 0 whose pads arrive RECOMP_ROLLBACK_LAG frames late.
   RECOMP_ROLLBACK=udp:<local port>:<peer host>:<peer port> plays a peer runner;
   RECOMP_ROLLBACK_PLAYER (0 or 1) picks the local port and
   RECOMP_ROLLBACK_DELAY the input delay, both matching on the two sides. At
   session frame RECOMP_ROLLBACK_FRAMES every pad is confirmed and guest RAM
   goes to RECOMP_ROLLBACK_DUMP. --netplay-host and --netplay-join start a
   lobby session from the first frame instead, and --replay plays one again.
   Everything a session changes lives here, so ending one is one reset. */
enum class RollbackMode { Off, Loopback, Udp, Replay };
enum : std::uint8_t { LobbyUnlockAll = 1u, LobbySkipIntro = 2u, LobbyVersus = 4u };
struct RollbackRun {
    RollbackMode mode = RollbackMode::Off;
    bool active = false;
    bool hidden = false;
    bool lobby = false; // lockstep menus, rollback over live rounds
    bool fromRoom = false; // started from the Versus menu's room, which it returns to
    bool landOnVersus = false; // a room's session walks to Versus character select unseen (driveToVersus)
    bool interrupted = false; // the other player went quiet (stalled)
    std::uint32_t start = 0u, stop = UINT32_MAX, lag = 4u, delay = 0u, player = 0u;
    std::uint32_t frontier = 0u; // first session frame never run
    std::uint32_t rollbacks = 0u, replayed = 0u, waited = 0u, saves = 0u;
    std::uint32_t wait = 0u, paced = UINT32_MAX; // frame sync's next wait; last frame paced
    std::chrono::steady_clock::time_point next_frame{};
    // Measured once per new frame and repeated while stalled, so a stall's
    // inflated advantage never reaches the peer's frame sync.
    std::int32_t advantage = 0;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point contact{}; // when a stall first saw the other player
    const char *dump = nullptr;
    // Desync checks: the newest check frame hashed but not yet confirmed (0 for none) and its hash,
    // the last frame compared, matches since the last log line and their slowest hash.
    std::uint32_t checkFrame = 0u, compared = 0u, matched = 0u;
    std::uint64_t checkHash = 0u;
    double slowestHashMs = 0.0;
    std::uint32_t flip = 0u; // RECOMP_ROLLBACK_FLIP: a test frame where this side alone changes guest RAM
    RecompRollbackSession session;
    RecompNetplay net;
    RecompInputGamepad pads[2]{}; // the running frame's pads by guest port
    // A lobby session's build, host options and the host's and joiner's profiles; a replay keeps them.
    std::string digest;
    std::uint8_t flags = 0u;
    std::vector<std::uint8_t> blobs[2];
    std::ofstream replayFile; // the replay being recorded (startReplay)
    RecompReplayWriter replayWriter{};
    std::uint32_t replayFrames = 0u;
    std::vector<std::uint8_t> replayData; // --replay's file and where it starts
    RecompReplayReader replayReader{};
    std::uint32_t replayStart = 0u;
    // The corner display (drawSessionOverlay).
    int networkDisplay = 0;
    std::uint32_t rewindDepth = 0u;
    std::chrono::steady_clock::time_point rewoundAt{};
    bool padSeen = false;
};
static RollbackRun rollback;

/* After a room session ends, the game boots offline and walks to the main
   menu's Versus entry behind the reopened room (recomp_game_walk_to_versus). */
static bool backToVersus;
static RecompInputGamepad backPad;

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

static RecompInputGamepad localPad(std::uint32_t frame)
{
    RecompInputGamepad pad{};
    if (rollback.mode == RollbackMode::Loopback) return pad;
    if (host.scriptedInput) {
        // Scripted runs play a generated stream per player so two runners differ.
        if (!rollback.lobby) return generatedPad(frame * 2u + rollback.player);
        // A scripted lobby: each side walks the menus with its input script, one
        // sample per frame, and the joiner mashes in live rounds.
        if (rollback.player == 1u && recomp_game_round_live()) pad = generatedPad(frame);
        else (void)recomp_input_pulse_source_sample(host.pulses, &pad);
        return pad;
    }
    (void)recomp_input_pulse_source_sample(host.pulses, &pad);
    return pad;
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

static void endSession(const char *reason);
static void compareChecks();

// The panel over the last frame while a session starts, stalls or ends.
static struct {
    const char *title = nullptr, *hint = nullptr;
    std::string detail;
    std::chrono::steady_clock::time_point shown{};
} notice;

static void drawNotice(const RecompD3dPresenterTarget *target)
{
    // opaque, so each redraw looks the same
    recomp_overlay_rect(120.0f, 194.0f, 400.0f, notice.hint != nullptr ? 118.0f : 92.0f, 0xff181820u);
    recomp_overlay_rect(120.0f, 194.0f, 400.0f, 4.0f, 0xffd8283cu);
    recomp_overlay_text(320.0f - recomp_overlay_text_width(notice.title, 3.0f) / 2.0f, 214.0f, 3.0f, 0xffffffffu,
        notice.title);
    recomp_overlay_text(320.0f - recomp_overlay_text_width(notice.detail.c_str(), 2.0f) / 2.0f, 252.0f, 2.0f,
        0xffa8a8b0u, notice.detail.c_str());
    if (notice.hint != nullptr)
        recomp_overlay_text(320.0f - recomp_overlay_text_width(notice.hint, 2.0f) / 2.0f, 282.0f, 2.0f, 0xff707078u,
            notice.hint);
    recomp_overlay_submit(target, recomp_overlay_display_aspect());
}

// Presents the panel now. A hint adds a third line; an empty one keeps the taller panel, covering an older hint.
static void showNotice(const char *title, std::string detail, const char *hint = nullptr)
{
    notice.title = title;
    notice.detail = std::move(detail);
    notice.hint = hint;
    recomp_d3d8_present_overlay(drawNotice);
}

/* A stalled session shows a notice over its last frame after 1 s, which also
   keeps the window answering, and ends after its grace: 15 s once the other
   player has been heard, 120 s while their runner may still be booting. A
   goodbye from the other runner ends it at once; a killed runner sends none. */
static void stalled(std::chrono::steady_clock::time_point began)
{
    using namespace std::chrono;
    if (rollback.net.peer_left) {
        std::fprintf(stderr, "recomp netplay: the other player left at frame %u\n", rollback.session.frame);
        showNotice("OPPONENT LEFT", "ENDING THE SESSION");
        std::this_thread::sleep_for(seconds(2));
        endSession("rollback:peer-left");
    }
    const auto now = steady_clock::now();
    const bool heard = rollback.net.peer_seen;
    if (heard && rollback.contact == steady_clock::time_point{}) rollback.contact = now;
    const seconds grace(heard ? 15 : 120);
    // Contact heard during this wait starts its 15 s afresh.
    const auto waited = now - (heard ? std::max(began, rollback.contact) : began);
    if (waited > grace) endSession("rollback:peer-timeout");
    if (waited < seconds(1) || now - notice.shown < milliseconds(100)) return;
    if (!rollback.interrupted && heard)
        std::fprintf(stderr, "recomp netplay: the other player went quiet at frame %u\n", rollback.session.frame);
    rollback.interrupted = true;
    notice.shown = now;
    showNotice(heard ? "WAITING FOR OPPONENT" : "CONNECTING",
        "ENDING THE SESSION IN " + std::to_string(duration_cast<seconds>(grace - waited).count() + 1) + " S");
}

static void resumed(std::chrono::steady_clock::time_point began)
{
    if (!rollback.interrupted) return;
    rollback.interrupted = false;
    std::fprintf(stderr, "recomp netplay: the other player is back after %.1f s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
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
        // A side stalled on the other may still hear the other's desync check.
        if (udp) compareChecks();
        // The local pad is read at the frame's start, so pacing adds no input lag.
        // Rewinds wait for it too, so one replay covers every pad that arrived.
        const bool started = Clock::now() >= rollback.next_frame;
        const std::uint32_t target = started ? recomp_rollback_take_rewind(&s) : RECOMP_ROLLBACK_NONE;
        if (target != RECOMP_ROLLBACK_NONE) {
            resumed(began);
            return target;
        }
        if (started) {
            while (s.local_count <= s.frame + rollback.delay) {
                const RecompInputGamepad pad = localPad(s.local_count);
                recomp_rollback_add_local(&s, &pad);
            }
        }
        const bool ready = last
            ? s.remote_count >= rollback.stop && (!udp || rollback.net.peer_ack >= rollback.stop)
            : started && recomp_rollback_can_run(&s) && (!udp || rollback.net.peer_seen);
        if (ready) {
            resumed(began);
            return RECOMP_ROLLBACK_NONE;
        }
        if (!udp) recomp_stop(4, "rollback:stalled");
        const Clock::time_point now = Clock::now();
        // Each repeat through the matchmaker's relay costs its bandwidth, so a relayed wait repeats less.
        if (now - sent >= std::chrono::milliseconds(rollback.net.match.relay ? 16 : 4)) {
            recomp_netplay_send(&rollback.net, &s, rollback.advantage);
            sent = now;
        }
        stalled(began);
        // sleep_for can round up to the 15.6 ms Windows timer tick; this timer does not.
        Clock::time_point until = now + std::chrono::milliseconds(2);
        if (rollback.next_frame > now && rollback.next_frame < until) until = rollback.next_frame;
        recomp_d3d_sleep_until(std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
    }
}

/* Each lobby session records a replay into the player's own
   .recomp-storage/replays. It stays on this computer: it holds the other
   player's profile. Frames go in once they have run and both pads have
   arrived, so the file holds only confirmed input. */
static void startReplay(std::uint32_t swap)
{
    namespace fs = std::filesystem;
    const fs::path folder = host.disc / ".recomp-storage" / "replays";
    char stamp[24], name[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    std::error_code error;
    fs::create_directories(folder, error);
    // A second session within the same second gets a suffix instead of truncating the first one's replay.
    std::snprintf(name, sizeof name, "%s.dr2r", stamp);
    for (unsigned n = 2u; n < 100u && fs::exists(folder / name, error); ++n)
        std::snprintf(name, sizeof name, "%s-%u.dr2r", stamp, n);
    rollback.replayFile.open(folder / name, std::ios::binary | std::ios::trunc);
    RecompReplayInfo info{};
    std::memcpy(info.digest, rollback.digest.data(), std::min(rollback.digest.size(), sizeof info.digest));
    info.player = static_cast<std::uint8_t>(rollback.player);
    info.delay = static_cast<std::uint8_t>(rollback.delay);
    info.flags = rollback.flags;
    info.start_swap = swap;
    info.blob_size[0] = static_cast<std::uint32_t>(rollback.blobs[0].size());
    info.blob_size[1] = static_cast<std::uint32_t>(rollback.blobs[1].size());
    std::vector<std::uint8_t> header(RECOMP_REPLAY_HEADER_BYTES + rollback.blobs[0].size() + rollback.blobs[1].size());
    header.resize(recomp_replay_header(&rollback.replayWriter, &info, rollback.blobs[0].data(),
        rollback.blobs[1].data(), header.data()));
    rollback.replayFile.write(reinterpret_cast<const char *>(header.data()),
        static_cast<std::streamsize>(header.size()));
    rollback.replayFile.flush();
    rollback.replayFrames = 0u;
    if (rollback.replayFile) {
        std::fprintf(stderr, "recomp netplay: recording replay %s\n", name);
        return;
    }
    std::fprintf(stderr, "recomp netplay: cannot write replay %s; playing without one\n", name);
    rollback.replayFile.close();
}

static void recordReplay()
{
    if (!rollback.replayFile.is_open()) return;
    const RecompRollbackSession &s = rollback.session;
    const std::uint32_t confirmed = std::min({s.local_count, s.remote_count, rollback.frontier});
    std::uint8_t bytes[RECOMP_REPLAY_FRAME_BYTES];
    for (; rollback.replayFrames < confirmed; ++rollback.replayFrames) {
        RecompInputGamepad pads[2];
        pads[rollback.player] = s.local[rollback.replayFrames % RECOMP_ROLLBACK_HISTORY];
        pads[1u - rollback.player] = s.remote[rollback.replayFrames % RECOMP_ROLLBACK_HISTORY];
        rollback.replayFile.write(reinterpret_cast<const char *>(bytes),
            static_cast<std::streamsize>(recomp_replay_frame(&rollback.replayWriter, pads, bytes)));
        // About once a second, so a crash loses little.
        if (rollback.replayFrames % 60u == 59u) {
            rollback.replayFile.write(reinterpret_cast<const char *>(bytes),
                static_cast<std::streamsize>(recomp_replay_flush(&rollback.replayWriter, bytes)));
            rollback.replayFile.flush();
        }
    }
}

static void closeReplay()
{
    if (!rollback.replayFile.is_open()) return;
    recordReplay();
    std::uint8_t bytes[RECOMP_REPLAY_FRAME_BYTES];
    rollback.replayFile.write(reinterpret_cast<const char *>(bytes),
        static_cast<std::streamsize>(recomp_replay_flush(&rollback.replayWriter, bytes)));
    rollback.replayFile.close();
    std::fprintf(stderr, "recomp netplay: replay %s with %u frames\n", rollback.replayFile ? "kept" : "cut short",
        rollback.replayFrames);
}

// Guest RAM without SyncTest's host-scheduled ranges, to RECOMP_ROLLBACK_DUMP when set.
static void writeDump()
{
    if (rollback.dump == nullptr) return;
    std::vector<std::uint8_t> ram(host.guestRam, host.guestRam + RECOMP_XBOX_RAM_SIZE);
    recomp_synctest_mask_ignored(ram.data(), ram.size());
    std::ofstream out(rollback.dump, std::ios::binary);
    out.write(reinterpret_cast<const char *>(ram.data()), static_cast<std::streamsize>(ram.size()));
    if (!out) recomp_stop(4, "rollback:dump");
}

static void finishRollback()
{
    closeReplay();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - rollback.started).count();
    std::fprintf(stderr, "recomp rollback: frame %u reached after %u rollbacks "
        "(%u frames run again, %u waited, %u saves) in %.1f s\n",
        rollback.session.frame, rollback.rollbacks, rollback.replayed, rollback.waited, rollback.saves, seconds);
    writeDump();
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

/* A session started from the Versus menu's room walks to character select.
   Both sides set the frame's pads the same way from the same guest state. */
static void driveToVersus(std::uint32_t frame)
{
    if (!rollback.landOnVersus) return;
    if (recomp_game_walk_to_versus(frame, true, &rollback.pads[0])) {
        rollback.landOnVersus = false;
        std::fprintf(stderr, "recomp netplay: Versus character select from frame %u\n", frame);
        return;
    }
    rollback.pads[1] = RecompInputGamepad{};
}

static void setWindow(std::uint32_t window)
{
    RecompRollbackSession &s = rollback.session;
    if (s.window == window) return;
    s.window = window;
    std::fprintf(stderr, "recomp netplay: %s from frame %u\n", window != 0u ? "rollback" : "lockstep", s.frame);
}

/* A profile write cannot be rewound (synctest_modules.c, save storage), so
   it waits until the peer's pads through the running frame have arrived.
   The window is normally closed long before, and this never waits. A pad
   that differs from the prediction means the frame is already wrong; the
   session then ends before the write. */
static void confirmSaveFrame()
{
    RecompRollbackSession &s = rollback.session;
    if (!rollback.active || rollback.mode != RollbackMode::Udp || s.frame == 0u) return;
    const std::uint32_t running = s.frame - 1u;
    if (s.remote_count <= running)
        std::fprintf(stderr, "recomp netplay: save in frame %u waits for the other player's input\n", running);
    const auto began = std::chrono::steady_clock::now();
    while (s.remote_count <= running) {
        recomp_netplay_receive(&rollback.net, &s);
        recomp_netplay_send(&rollback.net, &s, rollback.advantage);
        stalled(began);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    resumed(began);
    if (s.rewind_to <= running) endSession("rollback:save-mispredicted");
}

static void sayGoodbye()
{
    // Both sides of a scripted run stop at the same frame and need no goodbye.
    if (rollback.mode == RollbackMode::Udp && rollback.session.frame < rollback.stop)
        recomp_netplay_send_goodbye(&rollback.net);
}

/* Every 30th frame of a UDP session, each side hashes guest RAM as the frame
   starts, without SyncTest's host-scheduled ranges. Once the other player's
   pads before that frame have all arrived, no rewind reaches it again, so
   the hash is final and rides in each packet. A different hash from the
   other side for the same frame ends the session. */
constexpr std::uint32_t desyncCheckFrames = 30u;

static bool isCheckFrame(std::uint32_t frame)
{
    return rollback.mode == RollbackMode::Udp && frame != 0u && frame % desyncCheckFrames == 0u;
}

static void hashCheckFrame()
{
    const std::uint32_t frame = rollback.session.frame;
    if (!isCheckFrame(frame)) return;
    const auto began = std::chrono::steady_clock::now();
    RecompRamRange ignored[RECOMP_SNAPSHOT_HASH_RANGES];
    std::size_t count = recomp_synctest_ignored_ranges(ignored, RECOMP_SNAPSHOT_HASH_RANGES - 3u);
    if (count == SIZE_MAX) recomp_stop(4, "rollback:hash-ranges");
    /* draw_fight_name (program_manual.c) draws the local player's name in
       yellow and the other's in white, so the two sides' 2D draw state
       differs in glyph colors: in the sprite queue g_ui_sprite_queue, which
       ui_draw_lists_reset (0xae300) empties each frame and ui_draw_sprite
       (0xb1060) fills below 0x89bed9, and in the text color g_ui_text_color. */
    ignored[count++] = RecompRamRange{0x0088eed8u, 0x0089bed9u};
    ignored[count++] = RecompRamRange{0x008a01c8u, 0x008a01ccu};
    /* The lens flare's visibility-test flags and pixel counts (0xe5d628,
       read by flare_draw_ghosts 0x276ee0) come from each machine's own GPU and
       only scale the flare's alpha, so the two sides may differ there. */
    ignored[count++] = RecompRamRange{0x00e5d628u, 0x00e5d768u};
    if (!recomp_rollback_ring_hash(ignored, count, &rollback.checkHash)) recomp_stop(4, "rollback:hash");
    rollback.checkFrame = frame;
    rollback.slowestHashMs = std::max(rollback.slowestHashMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
}

/* A rewind goes back to the first frame whose guessed remote pad was wrong,
   so only frames starting without the other player's pad need a save, plus
   check frames, whose hash reads the newest save. Lockstep frames and frames
   whose pad came in time skip it, which saves most of a fight's copying. */
static bool saveFrame()
{
    const RecompRollbackSession &s = rollback.session;
    if (s.frame < s.remote_count && !isCheckFrame(s.frame)) return true;
    ++rollback.saves;
    return recomp_rollback_ring_save(s.frame);
}

// Only from the newest frame once its rewinds are done, so the hash already ran with every confirmed pad.
static void confirmCheck()
{
    if (rollback.checkFrame != 0u && rollback.session.remote_count >= rollback.checkFrame) {
        recomp_netplay_add_check(&rollback.net, rollback.checkFrame, rollback.checkHash);
        rollback.checkFrame = 0u;
    }
}

static void compareChecks()
{
    RecompNetplay &net = rollback.net;
    std::uint64_t mine = 0u;
    if (!recomp_netplay_own_check(&net, &mine) || net.peer_check_frame == rollback.compared) return;
    rollback.compared = net.peer_check_frame;
    if (mine == net.peer_check_hash) {
        if (++rollback.matched % 20u == 0u) {
            std::fprintf(stderr, "recomp netplay: guest RAM matches the other player's through frame %u "
                "(slowest check %.2f ms)\n", rollback.compared, rollback.slowestHashMs);
            rollback.slowestHashMs = 0.0;
        }
        return;
    }
    std::fprintf(stderr, "recomp netplay: desync at frame %u: guest RAM hash %016llx here, %016llx there\n",
        rollback.compared, static_cast<unsigned long long>(mine),
        static_cast<unsigned long long>(net.peer_check_hash));
    if (rollback.dump != nullptr)
        std::fprintf(stderr, "recomp netplay: guest RAM at frame %u goes to %s\n", rollback.session.frame,
            rollback.dump);
    writeDump();
    // The other side may not have this side's hash yet; it then finds the desync too.
    for (int i = 0; i < 3; ++i) recomp_netplay_send(&net, &rollback.session, rollback.advantage);
    showNotice("DESYNC DETECTED", "ENDING THE SESSION");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    endSession("rollback:desync");
}

/* The corner display from NETPLAY SETTINGS: ping, or also the input delay,
   the deepest rewind of the last second and how many frames this side runs
   ahead of the other. Over it, a notice while this player's pad is missing. */
static void drawSessionOverlay(const RecompD3dPresenterTarget *target)
{
    // Only this side sees its pad go missing; both guests keep the session's two pads.
    const bool connected = recomp_input_host_pad_connected(0u);
    rollback.padSeen = rollback.padSeen || connected;
    if (rollback.padSeen && !connected) {
        const char *missing = "CONTROLLER DISCONNECTED";
        const float width = recomp_overlay_text_width(missing, 2.0f);
        recomp_overlay_rect(320.0f - width / 2.0f - 6.0f, 410.0f, width + 12.0f, 24.0f, 0xa0000000u);
        recomp_overlay_text(320.0f - width / 2.0f, 415.0f, 2.0f, 0xffff5050u, missing);
    }
    if (rollback.networkDisplay == 0) {
        recomp_overlay_submit(target, recomp_overlay_display_aspect());
        return;
    }
    const RecompNetplay &net = rollback.net;
    std::string text = "PING " + (net.rtt_known ? std::to_string(net.rtt_ms) + " MS" : std::string("--"));
    if (rollback.networkDisplay == 2) {
        const bool recent = std::chrono::steady_clock::now() - rollback.rewoundAt < std::chrono::seconds(1);
        text += "  DELAY " + std::to_string(rollback.delay) + "  ROLLBACK " +
            std::to_string(recent ? rollback.rewindDepth : 0u) + "F  AHEAD " + std::to_string(rollback.advantage) + 'F';
    }
    const std::uint32_t color = !net.rtt_known || net.rtt_ms < 80u ? 0xff60e080u
        : net.rtt_ms < 150u ? 0xffffd040u : 0xffff5050u;
    const float width = recomp_overlay_text_width(text.c_str(), 2.0f);
    recomp_overlay_rect(628.0f - width - 12.0f, 446.0f, width + 12.0f, 24.0f, 0xa0000000u);
    recomp_overlay_text(628.0f - width - 6.0f, 451.0f, 2.0f, color, text.c_str());
    recomp_overlay_submit(target, recomp_overlay_display_aspect());
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
        if (const char *flip = std::getenv("RECOMP_ROLLBACK_FLIP"); flip != nullptr)
            rollback.flip = static_cast<std::uint32_t>(std::strtoul(flip, nullptr, 10));
        std::fprintf(stderr, "recomp rollback: session starts at swap %u as player %u\n",
            swap, rollback.player);
        if (rollback.lobby && rollback.mode == RollbackMode::Udp) startReplay(swap);
    }
    recordReplay();
    bool saved = true;
    if (s.frame == rollback.frontier) {
        if (rollback.lobby) setWindow(recomp_game_round_live() ? RECOMP_ROLLBACK_WINDOW : 0u);
        if (rollback.mode == RollbackMode::Udp && rollback.paced != s.frame) {
            scheduleFrame();
            rollback.paced = s.frame;
        }
        const std::uint32_t target = awaitFrontier(s.frame == rollback.stop);
        if (target != RECOMP_ROLLBACK_NONE) {
            // shortcut: a save that cannot be rewound ends the run; netplay must drop to lockstep before the game saves.
            if (!recomp_rollback_ring_load(target))
                endSession("rollback:unrestorable");
            ++rollback.rollbacks;
            rollback.replayed += rollback.frontier - target;
            const auto now = std::chrono::steady_clock::now();
            const std::uint32_t depth = rollback.frontier - target;
            const bool recent = now - rollback.rewoundAt < std::chrono::seconds(1);
            rollback.rewindDepth = recent ? std::max(rollback.rewindDepth, depth) : depth;
            rollback.rewoundAt = now;
        } else if (s.frame == rollback.stop) {
            finishRollback();
            return;
        } else {
            if (rollback.mode == RollbackMode::Udp) {
                confirmCheck();
                compareChecks();
            }
            if (rollback.flip != 0u && s.frame == rollback.flip) {
                *recomp_memory(0x0087ae54u, 1u) ^= 1u; // fighter 1's life, which persists
                std::fprintf(stderr, "recomp rollback: test flip of guest RAM at frame %u\n", s.frame);
            }
            saved = saveFrame();
            if (rollback.mode == RollbackMode::Udp) {
                rollback.advantage = static_cast<std::int32_t>(s.frame - rollback.net.peer_frame);
                recomp_netplay_send(&rollback.net, &s, rollback.advantage);
                const std::uint32_t wait = recomp_netplay_wait_frames(&rollback.net, rollback.advantage);
                rollback.waited += wait;
                rollback.wait = wait;
            }
        }
    } else {
        saved = saveFrame();
    }
    if (!saved) recomp_stop(4, "rollback:save");
    hashCheckFrame();
    // Frames run again were already shown and heard with the predicted pads; the walk to Versus stays unseen.
    rollback.hidden = s.frame < rollback.frontier;
    recomp_d3d8_set_present_suppressed(rollback.hidden || rollback.landOnVersus);
    recomp_d3d8_set_gpu_work_suppressed(rollback.hidden);
    recomp_dsound_buffer_set_output_suppressed(rollback.hidden || rollback.landOnVersus);
    recomp_rollback_run(&s, &rollback.pads[rollback.player], &rollback.pads[1u - rollback.player]);
    driveToVersus(s.frame);
    if (s.frame > rollback.frontier) rollback.frontier = s.frame;
}

/* --replay plays a recorded session's pads from the same first frame, as
   onRollbackFrame runs confirmed ones, and stops at the end of the file. */

static void onReplayFrame(std::uint32_t swap)
{
    RecompRollbackSession &s = rollback.session;
    if (!rollback.active) {
        if (swap < rollback.start || recomp_fiber_adapter_model()->current_handle == 0u) return;
        if (swap != rollback.replayStart) recomp_stop(4, "replay:start-mismatch");
        recomp_rollback_init(&s, 0u);
        rollback.active = true;
        rollback.started = std::chrono::steady_clock::now();
        std::fprintf(stderr, "recomp rollback: session starts at swap %u as player %u\n",
            swap, rollback.player);
    }
    if (s.frame == rollback.stop) {
        finishRollback();
        return;
    }
    if (!recomp_replay_next(&rollback.replayReader, rollback.pads)) {
        std::fprintf(stderr, "recomp netplay: replay ends at frame %u\n", s.frame);
        recomp_stop(0, "replay:end");
    }
    ++s.frame;
    recomp_d3d8_set_present_suppressed(rollback.landOnVersus);
    recomp_dsound_buffer_set_output_suppressed(rollback.landOnVersus);
    driveToVersus(s.frame);
}

static bool readRollbackConfig()
{
    const char *mode = std::getenv("RECOMP_ROLLBACK");
    if (mode == nullptr || *mode == '\0' || std::strcmp(mode, "0") == 0) return true;
    bool valid = true;
    const auto number = [&valid](const char *name, std::uint32_t &value) {
        const char *text = std::getenv(name);
        if (text == nullptr || *text == '\0') return;
        const char *end = text + std::strlen(text);
        const auto [stop, error] = std::from_chars(text, end, value);
        if (error != std::errc{} || stop != end) valid = false;
    };
    number("RECOMP_ROLLBACK_START", rollback.start);
    number("RECOMP_ROLLBACK_FRAMES", rollback.stop);
    rollback.dump = std::getenv("RECOMP_ROLLBACK_DUMP");
    if (std::strcmp(mode, "loopback") == 0) {
        rollback.mode = RollbackMode::Loopback;
        rollback.player = 1u;
        if (const char *frames = std::getenv("RECOMP_ROLLBACK_FRAMES"); frames == nullptr || *frames == '\0') rollback.stop = 600u;
        number("RECOMP_ROLLBACK_LAG", rollback.lag);
        return valid && rollback.lag <= RECOMP_ROLLBACK_WINDOW;
    }
    // udp:<local port>:<peer host>:<peer port>
    if (std::strncmp(mode, "udp:", 4u) != 0) return false;
    const std::string spec = mode + 4;
    const std::size_t hostAt = spec.find(':'), portAt = spec.rfind(':');
    if (hostAt == std::string::npos || portAt == hostAt) return false;
    const unsigned long localPort = std::strtoul(spec.substr(0u, hostAt).c_str(), nullptr, 10);
    const unsigned long peerPort = std::strtoul(spec.substr(portAt + 1u).c_str(), nullptr, 10);
    const std::string peerHost = spec.substr(hostAt + 1u, portAt - hostAt - 1u);
    number("RECOMP_ROLLBACK_PLAYER", rollback.player);
    number("RECOMP_ROLLBACK_DELAY", rollback.delay);
    if (!valid || localPort == 0u || localPort > 65535u || peerPort == 0u || peerPort > 65535u ||
        rollback.player > 1u || rollback.delay > RECOMP_ROLLBACK_WINDOW)
        return false;
    rollback.mode = RollbackMode::Udp;
    return recomp_netplay_open(&rollback.net, static_cast<std::uint16_t>(localPort), peerHost.c_str(),
        static_cast<std::uint16_t>(peerPort));
}

/* Starts a session from rollback.blobs and rollback.flags with the host's input
   delay. Both sides, and a replay, play from fresh copies of the two
   profiles, with the host's title saves, so they start alike and neither
   player's own profile changes. */
static bool enterSession(std::uint8_t delay, std::uint32_t player)
{
    if (delay > RECOMP_ROLLBACK_WINDOW) return false;
    recomp_skip_intro = (rollback.flags & LobbySkipIntro) != 0u;
    rollback.landOnVersus = (rollback.flags & LobbyVersus) != 0u;
    rollback.lobby = true;
    rollback.player = player;
    rollback.delay = delay;
    rollback.start = 0u;
    // Test hooks, as for RECOMP_ROLLBACK.
    if (const char *frames = std::getenv("RECOMP_ROLLBACK_FRAMES"); frames != nullptr && *frames != '\0')
        rollback.stop = static_cast<std::uint32_t>(std::strtoul(frames, nullptr, 10));
    rollback.dump = std::getenv("RECOMP_ROLLBACK_DUMP");
    const std::filesystem::path storage = host.disc / ".recomp-storage";
    std::error_code error;
    std::filesystem::remove_all(storage / "netplay", error);
    if (error) {
        std::cerr << "recomp runner: cannot clear the netplay save folder\n";
        return false;
    }
    // The host's blob first; a joiner's profile with the same name, and so the same folder, replaces the host's.
    const std::filesystem::path session = storage / "netplay" / "partition1";
    std::vector<std::string> names[2];
    if (!recomp_netplay_unpack_profile(rollback.blobs[0].data(), rollback.blobs[0].size(), session, false, names[0]) ||
        !recomp_netplay_unpack_profile(rollback.blobs[1].data(), rollback.blobs[1].size(), session, true, names[1])) {
        std::cerr << "recomp runner: the other player's profile is damaged or unsafe\n";
        return false;
    }
    for (const std::string &name : names[1u - player]) {
        if (recomp_netplay_name_banned(name.c_str())) {
            std::cerr << "recomp runner: the other player's profile name is not allowed online\n";
            return false;
        }
    }
    recomp_save_session = "netplay";
    const auto shown = [](const std::vector<std::string> &found) {
        return found.empty() ? std::string("no profile") : found.front();
    };
    std::fprintf(stderr, "recomp netplay: %s as player %u (%s) against %s with %u frames of input delay\n",
        rollback.mode == RollbackMode::Replay ? "replaying" : "connected", player + 1u,
        shown(names[player]).c_str(), shown(names[1u - player]).c_str(), delay);
    // No profile shows the side; two default profiles with one name get the side in front.
    std::string fight[2] = {shown(names[0]), shown(names[1])};
    const bool same = fight[0] == fight[1];
    for (unsigned side = 0u; side < 2u; ++side) {
        const std::string tag = side == 0u ? "P1" : "P2";
        if (names[side].empty()) fight[side] = tag;
        else if (same) fight[side] = tag + " " + fight[side];
    }
    recomp_program_set_fight_names(fight[0].c_str(), fight[1].c_str(), player);
    return true;
}

/* Connects the two runners on rollback.net before either boots. The host's
   input delay and options win. Each player sends the profile they saved last. */
static bool joinLobby(bool hosting, std::uint32_t delay, bool (*cancelled)())
{
    RecompNetplayLobby lobby{};
    if (host.digest.size() != sizeof lobby.digest) {
        std::cerr << "recomp runner: cannot read this runner to identify its build\n";
        return false;
    }
    std::memcpy(lobby.digest, host.digest.data(), sizeof lobby.digest);
    lobby.delay = static_cast<std::uint8_t>(delay);
    lobby.flags = static_cast<std::uint8_t>((host.unlockAll ? LobbyUnlockAll : 0u) |
        (recomp_skip_intro ? LobbySkipIntro : 0u) | (rollback.landOnVersus ? LobbyVersus : 0u));
    const std::filesystem::path partition1 = host.disc / ".recomp-storage" / "partition1";
    // Recovery first, so an interrupted save is never sent.
    const std::string own =
        recomp_save_initialize(recomp_disc_root_path) ? recomp_netplay_latest_profile(partition1) : "";
    if (recomp_netplay_name_banned(recomp_netplay_own_profile_name(partition1).c_str())) {
        std::cerr << "recomp runner: your profile name is not allowed online; rename the profile to play\n";
        return false;
    }
    std::vector<std::uint8_t> mine, theirs(RECOMP_NETPLAY_MAX_PROFILE);
    if (!recomp_save_ready() || !recomp_netplay_pack_profile(partition1, own, hosting, mine) ||
        mine.size() > RECOMP_NETPLAY_MAX_PROFILE) {
        std::cerr << "recomp runner: cannot read your profile to send, or it is over 1 MiB\n";
        return false;
    }
    RecompNetplayProfiles profiles{mine.data(), static_cast<std::uint32_t>(mine.size()), theirs.data(), 0u};
    const RecompNetplayLobbyResult result = hosting
        ? recomp_netplay_host(&rollback.net, &lobby, &profiles, 10u * 60u * 1000u, cancelled)
        : recomp_netplay_join(&rollback.net, &lobby, &profiles, 2u * 60u * 1000u, cancelled);
    if (result != RECOMP_NETPLAY_LOBBY_OK) {
        std::cerr << "recomp runner: netplay " << (result == RECOMP_NETPLAY_LOBBY_CANCELLED
            ? (rollback.net.peer_left ? "cancelled by the other player" : "cancelled")
            : result == RECOMP_NETPLAY_LOBBY_MISMATCH ? "refused: the other player runs a different build"
            : result == RECOMP_NETPLAY_LOBBY_TIMEOUT ? "timed out" : "could not open its UDP port") << '\n';
        return false;
    }
    rollback.digest = host.digest;
    rollback.flags = lobby.flags;
    rollback.blobs[hosting ? 0 : 1] = mine;
    rollback.blobs[hosting ? 1 : 0].assign(theirs.begin(), theirs.begin() + profiles.theirs_size);
    rollback.mode = RollbackMode::Udp;
    return enterSession(lobby.delay, hosting ? 0u : 1u);
}

bool recomp_netplay_session_connect(const RecompNetplayLaunch &launch)
{
    if (const char *mode = std::getenv("RECOMP_ROLLBACK"); mode != nullptr && *mode != '\0') {
        std::cerr << "recomp runner: RECOMP_ROLLBACK cannot be combined with --netplay-host or --netplay-join\n";
        return false;
    }
    const bool hosting = launch.hostPort != 0u;
    if (hosting) {
        const auto port = static_cast<std::uint16_t>(launch.hostPort);
        if (recomp_netplay_open_router_port(port, nullptr, 0u)) std::atexit(recomp_netplay_close_router_port);
        std::fprintf(stderr, "recomp netplay: hosting on UDP port %u; waiting for the other player\n", launch.hostPort);
        if (!recomp_netplay_listen(&rollback.net, port)) {
            std::cerr << "recomp runner: netplay could not open its UDP port\n";
            return false;
        }
    } else {
        const std::size_t colon = launch.join.rfind(':');
        const char *end = launch.join.data() + launch.join.size();
        std::uint32_t port = 0u;
        if (colon == std::string::npos || colon == 0u ||
            std::from_chars(launch.join.data() + colon + 1u, end, port).ptr != end || port == 0u || port > 65535u) {
            std::cerr << "recomp runner: --netplay-join needs <host>:<port>\n";
            return false;
        }
        std::fprintf(stderr, "recomp netplay: joining the host\n"); // no address: logs end up in public issues
        if (!recomp_netplay_open(&rollback.net, 0u, launch.join.substr(0u, colon).c_str(),
                static_cast<std::uint16_t>(port))) {
            std::cerr << "recomp runner: netplay could not open its UDP port\n";
            return false;
        }
    }
    return joinLobby(hosting, launch.delay, nullptr);
}

bool recomp_netplay_session_load_replay(const std::string &path)
{
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    rollback.replayData.assign(std::istreambuf_iterator<char>(file), {});
    RecompReplayInfo info;
    const std::uint8_t *hostBlob = nullptr, *joinerBlob = nullptr;
    if (!recomp_replay_open(&rollback.replayReader, rollback.replayData.data(), rollback.replayData.size(), &info,
            &hostBlob, &joinerBlob)) {
        std::cerr << "recomp runner: --replay needs a replay file this runner can read\n";
        return false;
    }
    if (std::string(info.digest, sizeof info.digest) != host.digest) {
        std::cerr << "recomp runner: the replay was recorded by another build\n";
        return false;
    }
    rollback.flags = info.flags;
    rollback.blobs[0].assign(hostBlob, hostBlob + info.blob_size[0]);
    rollback.blobs[1].assign(joinerBlob, joinerBlob + info.blob_size[1]);
    rollback.replayStart = info.start_swap;
    rollback.mode = RollbackMode::Replay;
    return enterSession(info.delay, info.player);
}

// B, Back or Esc, pressed while STARTING MATCH shows; one already held must be let go first.
static bool cancelHeld;
static bool cancelPressed()
{
    std::uint8_t keys[RECOMP_INPUT_KEY_COUNT];
    recomp_input_keyboard_sample(keys); // before the pad, which would take the press edges
    RecompInputGamepad pad{};
    (void)recomp_input_host_sample(0u, &pad);
    const bool down = keys[0x1bu] != 0u || pad.analog_buttons[RECOMP_INPUT_ANALOG_B] > 0x40u ||
        (pad.buttons & 0x20u) != 0u; // Esc, B, Back
    const bool pressed = down && !cancelHeld;
    cancelHeld = down;
    return pressed;
}

/* A room's launch: connect on the room's socket, so the router's path
   stays, while the menu's last frame stays up, then boot the game again in
   this process, so the window stays open. */
static void startRoomSession()
{
    std::uint32_t port = 0u, delay = 0u;
    std::string join;
    RecompNetplay socket;
    if (!recomp_netplay_menu_take_launch(port, delay, join, socket)) return;
    rollback.net = socket;
    rollback.fromRoom = true;
    // The room's players skip the intro and land on Versus character select. The
    // skip stays on, since every later boot is a session or the walk back to the room.
    recomp_skip_intro = true;
    rollback.landOnVersus = true;
    const bool hosting = port != 0u;
    if (hosting) std::fprintf(stderr, "recomp netplay: hosting on UDP port %u; waiting for the other player\n", port);
    else std::fprintf(stderr, "recomp netplay: joining the host\n");
    showNotice("STARTING MATCH", hosting ? "WAITING FOR THE OTHER PLAYER" : "CONNECTING TO THE HOST", "B / ESC CANCEL");
    cancelHeld = true;
    if (!joinLobby(hosting, delay, cancelPressed)) endSession("netplay:session-failed");
    showNotice("STARTING MATCH", "LOADING VERSUS", "");
    host.reboot();
}

/* A room session that ends boots the game offline again and reopens its
   room. Sessions started from the command line stop with exit code 4. */
static void endSession(const char *reason)
{
    closeReplay();
    if (!rollback.fromRoom) recomp_stop(4, "%s", reason);
    std::fprintf(stderr, "recomp netplay: session ended (%s); back to the room\n", reason);
    sayGoodbye();
    const RecompNetplay socket = rollback.net; // back to the room, so the router's path stays
    rollback = RollbackRun{};
    recomp_save_session = nullptr;
    recomp_program_set_fight_names(nullptr, nullptr, 0u);
    backToVersus = true;
    recomp_netplay_menu_return_to_room(socket);
    host.reboot();
}

void recomp_netplay_session_init(const RecompNetplaySessionHost &runner)
{
    host = runner;
}

bool recomp_netplay_session_read_env()
{
    return rollback.lobby || readRollbackConfig();
}

bool recomp_netplay_session_enabled()
{
    return rollback.mode != RollbackMode::Off;
}

void recomp_netplay_session_install(bool synctest)
{
    recomp_d3d_overlay_hook = nullptr;
    const bool enabled = recomp_netplay_session_enabled();
    if (enabled) {
        // Rollback input lives in the session, outside the saved state.
        recomp_d3d_frame_boundary_hook = rollback.mode == RollbackMode::Replay ? onReplayFrame : onRollbackFrame;
        recomp_kernel_profile_mutation_hook = confirmSaveFrame;
    }
    // The walks to Versus run unseen behind the last notice, and unheard (recomp_game_walk_to_versus).
    recomp_d3d8_set_present_suppressed(rollback.landOnVersus);
    recomp_dsound_buffer_set_output_suppressed(rollback.landOnVersus || backToVersus);
    if (rollback.lobby) {
        const std::filesystem::path storage = host.disc / ".recomp-storage";
        if (rollback.mode == RollbackMode::Udp) {
            rollback.networkDisplay = recomp_netplay_menu_display(storage);
            recomp_d3d_overlay_hook = drawSessionOverlay;
        }
    } else if (!enabled && !synctest) {
        const std::filesystem::path storage = host.disc / ".recomp-storage";
        recomp_netplay_menu_install(storage, host.digest,
            [storage] { return recomp_netplay_own_profile_name(storage / "partition1"); });
    }
}

bool recomp_netplay_session_pad(std::uint32_t port, RecompInputGamepad *pad, bool *present)
{
    // Both runners must see the same controllers: the session's two ports, once it runs.
    // A host pad unplugged or plugged in mid-session never reaches the guest.
    if (rollback.active ? port >= 2u : rollback.lobby) {
        *present = false;
        return true;
    }
    if (!rollback.active) return false;
    // Loopback has no local player, so its port stays empty.
    *present = !(rollback.mode == RollbackMode::Loopback && port == rollback.player);
    if (*present) *pad = rollback.pads[port];
    return true;
}

bool recomp_netplay_session_back_pad(RecompInputGamepad *pad)
{
    if (!backToVersus) return false;
    *pad = backPad;
    return true;
}

// The local player's port drives the local pad; the peer's port and hidden frames stay still.
void recomp_netplay_session_set_vibration(std::uint32_t port, std::uint16_t left, std::uint16_t right)
{
    if (!rollback.active) {
        recomp_input_host_set_vibration(port, left, right);
        return;
    }
    // shortcut: a motor change made only in a hidden frame is dropped until the next visible one.
    if (port == rollback.player && !rollback.hidden) recomp_input_host_set_vibration(0u, left, right);
}

void recomp_netplay_session_on_swap(std::uint32_t frame)
{
    if (backToVersus && recomp_game_walk_to_versus(frame, false, &backPad)) {
        backToVersus = false;
        recomp_dsound_buffer_set_output_suppressed(false);
        std::fprintf(stderr, "recomp netplay: back at the Versus menu from frame %u\n", frame);
    }
    startRoomSession();
}

bool recomp_netplay_session_unlock_all(bool own)
{
    return rollback.lobby ? (rollback.flags & LobbyUnlockAll) != 0u : own;
}

void recomp_netplay_session_exit()
{
    closeReplay();
    sayGoodbye();
}
