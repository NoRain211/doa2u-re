/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_session.h"

#include "d3d8_adapter.h"
#include "d3d_vblank.h"
#include "dsound_buffer_adapter.h"
#include "fiber_adapter.h"
#include "game_state.h"
#include "input_host_win32.h"
#include "netplay.h"
#include "overlay.h"
#include "rollback.h"
#include "synctest.h"
#include "xbox_memory_layout.h"
extern "C" {
#include "kernel_abi.h"
#include "hit_voice.h"
#include "live_client.h"

// netplay_wire.h declares it but carries winsock; the Live pump runs on its clock.
extern "C" std::uint64_t netplay_now_ms(void);
#include "live_fight_rules.h"
#include "live_session.h"
#include "live_transport.h"
#include "live_xapi.h"
#include "stop_report.h"
}

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static RecompNetplaySessionHost host;

/* Rollback over a fight (docs/rollback-netcode.md), from swap
   RECOMP_ROLLBACK_START. RECOMP_ROLLBACK=loopback plays a generated remote
   player on port 0 whose pads arrive RECOMP_ROLLBACK_LAG frames late. At
   session frame RECOMP_ROLLBACK_FRAMES every pad is confirmed and guest RAM
   goes to RECOMP_ROLLBACK_DUMP. Live (RollbackMode::Live) runs the same
   session over live_transport.c's route, started by each match's round
   handshake (live_fight.c).
   Everything a session changes lives here, so ending one is one reset. */
enum class RollbackMode { Off, Loopback, Live };
struct RollbackRun {
    RollbackMode mode = RollbackMode::Off;
    bool active = false;
    bool hidden = false;
    bool interrupted = false; // the other player went quiet (stalled)
    std::uint32_t start = 0u, stop = UINT32_MAX, lag = 4u, player = 0u;
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
    // Live: the round handshake's start request, and what the game's lockstep stop and a
    // vanished peer leave for the session's end.
    bool livePending = false, liveStopPending = false, liveLockstepStopped = false, liveAbandon = false;
    // Live: the last pads of a finished fight, repeated by the frame pump over
    // the frames that follow instead of sleeping on the frame thread.
    bool liveTail = false;
    std::chrono::steady_clock::time_point liveTailUntil{};
    RecompRollbackSession session;
    RecompNetplay net;
    RecompInputGamepad pads[2]{}; // the running frame's pads by guest port
    // The live-round second measureHitch is adding up, and when the last frontier wait ended.
    std::chrono::steady_clock::time_point hitchSince{}, frameLeft{};
    std::uint32_t hitchFrom = 0u, heldBack = 0u;
    double peerWaitMs = 0.0, longestWaitMs = 0.0, slowestFrameMs = 0.0;
    // Frames run again after rewinds this second, each timed from its load to the frontier.
    std::chrono::steady_clock::time_point rerunFrom{};
    std::uint32_t rerunDepth = 0u, rerunFrames = 0u;
    double rerunMs = 0.0;
    double rerunTotalMs = 0.0, saveMs = 0.0, loadMs = 0.0; // the session's totals for the summary
    // The last frontier wait: how late this side first saw its frame due, and how long it then lacked pads.
    double lateMs = 0.0, blockedMs = 0.0;
};
static RollbackRun rollback;

/* Live's transport is the only session with a peer; Loopback generates its
   remote player and compares the result locally. */
static bool networked()
{
    return rollback.mode == RollbackMode::Live;
}

// The Live client started; the frame pump then serves the matchmaker and the game.
static bool liveOn;

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
    // Scripted runs play a generated stream per player so two runners differ.
    if (host.scriptedInput) return generatedPad(frame * 2u + rollback.player);
    (void)recomp_input_pulse_source_sample(host.pulses, &pad);
    return pad;
}

static void receivePads()
{
    RecompRollbackSession &s = rollback.session;
    if (networked()) {
        recomp_netplay_receive(&rollback.net, &s);
        return;
    }
    const std::uint32_t lag = s.frame >= rollback.stop ? 0u : rollback.lag;
    while (s.remote_count + lag <= s.frame) {
        const RecompInputGamepad pad = generatedPad(s.remote_count);
        recomp_rollback_add_remote(&s, &pad);
    }
}

static void compareChecks();
/* A failed or answered session end: see "Session ends" below. */
static bool failSession(RecompLiveEnd end);
static void liveOpponentLeft();

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

static void presentNotice()
{
    notice.shown = std::chrono::steady_clock::now();
    recomp_d3d8_present_overlay(drawNotice);
}

/* A wait presents its panel again every 100 ms. The window answers only then: the SDL
   presenter reads keys and the close button when it presents, and a closed window ends the
   run at the next present. */
static bool noticeDue()
{
    return std::chrono::steady_clock::now() - notice.shown >= std::chrono::milliseconds(100);
}

// Presents the panel now. A hint adds a third line; an empty one keeps the taller panel, covering an older hint.
static void showNotice(const char *title, std::string detail, const char *hint = nullptr)
{
    notice.title = title;
    notice.detail = std::move(detail);
    notice.hint = hint;
    presentNotice();
}

/* A stalled session shows a notice over its last frame after 1 s, which also
   keeps the window answering, and ends after 15 s without the other player:
   both games are already running, so a fight that starts on one only waits for
   the other's stage to load. A goodbye from the other runner ends it at once;
   a killed runner sends none. */
static void stalled(std::chrono::steady_clock::time_point began)
{
    using namespace std::chrono;
    // A goodbye from the other runner ends the fight here instead of at the timeout.
    if (rollback.net.peer_left) {
        liveOpponentLeft();
        return;
    }
    /* Live: the game's own Live messages end a fight, so one side can leave it a frame or
       two before the other and stop sending; outside a live round that ends this session gently. */
    if (rollback.session.window == 0u && rollback.session.frame > 60u &&
        steady_clock::now() - began > seconds(2)) {
        std::fprintf(stderr, "recomp live: the other side's session ended at frame %u\n", rollback.session.frame);
        rollback.liveAbandon = true;
        return;
    }
    const auto now = steady_clock::now();
    const bool heard = rollback.net.peer_seen;
    if (heard && rollback.contact == steady_clock::time_point{}) rollback.contact = now;
    const seconds grace(15);
    // Contact heard during this wait starts its 15 s afresh.
    const auto waited = now - (heard ? std::max(began, rollback.contact) : began);
    if (waited > grace) {
        failSession(RECOMP_LIVE_END_TIMEOUT);
        return;
    }
    if (waited < seconds(1) || !noticeDue()) return;
    if (!rollback.interrupted && heard)
        std::fprintf(stderr, "recomp netplay: the other player went quiet at frame %u\n", rollback.session.frame);
    rollback.interrupted = true;
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

/* Repeats this side's pads while a wait goes on: every 4 ms, or every 16 ms through the
   matchmaker's relay. Each repeat costs the relay bandwidth, and it drops a pair's packets
   past 250 a second. */
static void repeatPads(std::chrono::steady_clock::time_point &sent)
{
    // Live: the transport owns the route, so take its relay state before choosing a pace.
    const bool relay = rollback.mode == RollbackMode::Live &&
        recomp_live_transport_relay(rollback.net.peer_address);
    const auto now = std::chrono::steady_clock::now();
    if (now - sent < std::chrono::milliseconds(relay ? 16 : 4)) return;
    recomp_netplay_send(&rollback.net, &rollback.session, rollback.advantage);
    sent = now;
}

/* At the newest frame: gathers pads until the frame may run or a rewind is
   due, and returns the frame to load or RECOMP_ROLLBACK_NONE. */
static std::uint32_t awaitFrontier(bool last)
{
    using Clock = std::chrono::steady_clock;
    RecompRollbackSession &s = rollback.session;
    const bool udp = networked();
    Clock::time_point sent{};
    const Clock::time_point began = Clock::now();
    // For measureHitch: when this side first saw its frame due, and first found it lacked the other's pads.
    Clock::time_point due{}, blocked{};
    const auto measured = [&] {
        const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        rollback.lateMs = due == Clock::time_point{} ? 0.0 : ms(due - std::max(began, rollback.next_frame));
        rollback.blockedMs = blocked == Clock::time_point{} ? 0.0 : ms(Clock::now() - blocked);
    };
    for (;;) {
        receivePads();
        // Live: the other runner said goodbye, so its fight is over and this one ends at once.
        if (rollback.mode == RollbackMode::Live && rollback.net.peer_left) {
            liveOpponentLeft();
            return RECOMP_ROLLBACK_NONE;
        }
        // A side stalled on the other may still hear the other's desync check.
        if (udp) compareChecks();
        // Live: a desync hands the fight back to the game instead of stopping the runner.
        if (rollback.liveAbandon) return RECOMP_ROLLBACK_NONE;
        // The local pad is read at the frame's start, so pacing adds no input lag.
        // Rewinds wait for it too, so one replay covers every pad that arrived.
        const Clock::time_point checked = Clock::now();
        const bool started = checked >= rollback.next_frame;
        if (started && due == Clock::time_point{}) due = checked;
        const std::uint32_t target = started ? recomp_rollback_take_rewind(&s) : RECOMP_ROLLBACK_NONE;
        if (target != RECOMP_ROLLBACK_NONE) {
            resumed(began);
            measured();
            return target;
        }
        if (started) {
            while (s.local_count <= s.frame) {
                RecompInputGamepad pad = localPad(s.local_count);
                // Live: the game asked to stop its lockstep, so the request rides this side's next pad.
                if (rollback.mode == RollbackMode::Live && rollback.liveStopPending) {
                    pad.buttons = recomp_live_pad_add_stop(pad.buttons);
                    rollback.liveStopPending = false;
                }
                recomp_rollback_add_local(&s, &pad);
            }
        }
        const bool ready = last
            ? s.remote_count >= rollback.stop && (!udp || rollback.net.peer_ack >= rollback.stop)
            : started && recomp_rollback_can_run(&s) && (!udp || rollback.net.peer_seen);
        if (ready) {
            resumed(began);
            measured();
            return RECOMP_ROLLBACK_NONE;
        }
        if (started && blocked == Clock::time_point{}) blocked = checked;
        if (!udp) recomp_stop(4, "rollback:stalled");
        repeatPads(sent);
        const Clock::time_point now = Clock::now();
        stalled(began);
        if (rollback.liveAbandon) return RECOMP_ROLLBACK_NONE;
        // sleep_for can round up to the 15.6 ms Windows timer tick; this timer does not.
        Clock::time_point until = now + std::chrono::milliseconds(2);
        if (rollback.next_frame > now && rollback.next_frame < until) until = rollback.next_frame;
        recomp_d3d_sleep_until(std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
    }
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

static void finishLiveSession();

static void finishRollback()
{
    if (rollback.mode == RollbackMode::Live) {
        finishLiveSession();
        return;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - rollback.started).count();
    std::fprintf(stderr, "recomp rollback: frame %u reached after %u rollbacks loading %.0f ms "
        "(%u frames run again in %.0f ms, %u waited, %u saves in %.0f ms) in %.1f s\n",
        rollback.session.frame, rollback.rollbacks, rollback.loadMs, rollback.replayed, rollback.rerunTotalMs,
        rollback.waited, rollback.saves, rollback.saveMs, seconds);
    writeDump();
    recomp_stop(0, "rollback:done");
}

/* Live: the fight is over and the game's own Live messages carry what follows
   (results, win poses, the lobby). Nothing is recorded, and the runner keeps
   running: the session's suppression of present, GPU work and audio goes, and
   the transport is unbound so the game's own datagrams flow again. */
static void finishLiveSession()
{
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - rollback.started).count();
    std::fprintf(stderr, "recomp live: frame %u reached after %u rollbacks "
        "(%u frames run again, %u waited, %u saves) in %.1f s\n",
        rollback.session.frame, rollback.rollbacks, rollback.replayed, rollback.waited, rollback.saves, seconds);
    writeDump();
    // The other side may still need this side's last pads to reach the stop frame.
    // The frame pump repeats them from here (pumpLiveTail), so the frame thread is
    // not held for half a second.
    rollback.liveTail = true;
    rollback.liveTailUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    rollback.active = false;
    rollback.hidden = false;
    recomp_hit_voice_set_rollback_frame(UINT32_MAX, false);
    rollback.stop = UINT32_MAX;
    rollback.frontier = 0u;
    rollback.checkFrame = 0u;
    rollback.livePending = rollback.liveStopPending = rollback.liveLockstepStopped = rollback.liveAbandon = false;
    rollback.pads[0] = rollback.pads[1] = RecompInputGamepad{};
    recomp_live_transport_session_end();
    recomp_d3d8_set_present_suppressed(false);
    recomp_d3d8_set_gpu_work_suppressed(false);
    recomp_dsound_buffer_set_output_suppressed(false);
    std::fprintf(stderr, "recomp live: session over, the game's own Live code continues\n");
}

/* Live: the last pads finishLiveSession kept, repeated by the frame pump. The
   transport's rollback route stays bound until the tail ends, so the peer hears
   them (and this side's goodbye) without the frame thread sleeping. */
static void pumpLiveTail()
{
    if (!rollback.liveTail) return;
    recomp_netplay_receive(&rollback.net, &rollback.session);
    recomp_netplay_send(&rollback.net, &rollback.session, 0);
    if (std::chrono::steady_clock::now() < rollback.liveTailUntil) return;
    rollback.liveTail = false;
    recomp_live_transport_unbind_rollback();
}

/* Session ends: a desync, a peer timeout, a failed rewind or
   the other player's goodbye. A Live fight hands the fight back to the game, whose own
   lockstep and Live messages carry it from there, so the runner keeps running; a Loopback
   test stops the runner as it always has. live_fight_rules.c owns that decision, so the
   fight test pins it without the game; these supply what each end does. */
static void tellOpponent(void *)
{
    // Over the bound rollback route, before finishLiveSession unbinds it.
    recomp_netplay_send_goodbye(&rollback.net);
}

static void stopRunner(void *, const char *stop_id)
{
    recomp_stop(4, "%s", stop_id);
}

static void handBackToGame(void *, const char *stop_id)
{
    std::fprintf(stderr, "recomp live: session ends (%s); the game's own Live code continues\n", stop_id);
    // The next frame boundary restores the game's lockstep and unbinds the route.
    rollback.liveAbandon = true;
}

/* Ends the session where it failed. True when a Live fight went back to the game, so
   the caller returns without running another frame; otherwise a Loopback test stopped
   the runner and this never returns. */
static bool failSession(RecompLiveEnd end)
{
    static const RecompLiveEndOps ops = {nullptr, tellOpponent, stopRunner, handBackToGame};
    return recomp_live_end_session(rollback.mode == RollbackMode::Live, end, &ops);
}

/* Live: the other runner said goodbye over the session's route. */
static void liveOpponentLeft()
{
    std::fprintf(stderr, "recomp live: the other player left at frame %u\n", rollback.session.frame);
    showNotice("OPPONENT LEFT", "ENDING THE SESSION");
    failSession(RECOMP_LIVE_END_PEER_LEFT);
}

/* A Live end was requested: finishes the session now and returns true, so the frame
   handler leaves the fight to the game's own Live code. */
static bool liveSessionEnded()
{
    if (!rollback.liveAbandon) return false;
    rollback.liveAbandon = false;
    finishLiveSession();
    return true;
}

/* A Live fight's session starts at the frame boundary after the game's
   round-start handshake returned (recomp_live_round_started). The transport's
   route replaces a socket of our own, and both sides begin from the same
   cleared pads. */
static void resetNetplayCounters()
{
    RecompNetplay &net = rollback.net;
    net.peer_seen = false;
    net.peer_ack = net.peer_frame = 0u;
    net.peer_advantage = 0;
    std::memset(net.local_samples, 0, sizeof net.local_samples);
    std::memset(net.peer_samples, 0, sizeof net.peer_samples);
    net.local_count = net.peer_count = net.cooldown = 0u;
    std::memset(net.pad_sent_ms, 0, sizeof net.pad_sent_ms);
    net.pads_stamped = 0u;
    net.rtt_ms = 0u;
    net.rtt_known = false;
    net.peer_left = false;
    std::memset(net.check_frames, 0, sizeof net.check_frames);
    std::memset(net.check_hashes, 0, sizeof net.check_hashes);
    net.check_count = net.peer_check_frame = 0u;
    net.peer_check_hash = 0u;
    // net.match stays: the transport's server and the server's cookie live there.
}

/* Both sides, at a session's start: the pad records still hold this side's
   last pad before the session (the A that picked the rematch) and its edges,
   and frame 0 would turn them into a press on one side only. Clear the ports'
   and the session slots' records in g_pad_state (0xa94a70, 0x30 each: buttons,
   held, pressed, released, analog and sticks, then at +0x28 the key-repeat
   latch, the last new press, and at +0x2c its hold count) and the four
   players' copies in g_player_pads (0x87d260). Left set, the latch keeps each
   side's last frontend press, such as the stage chooser's last direction
   against the other side's A. FUN_000e9c90 fills each
   fighter's 16 move slots as moves start, by the slot's type at 0x87d4a0:
   type 1 a vector (0x87a948, 0x10 each) and a float (0x878310), type 3 two
   vectors (0x877f00, 0x20 each) and a float (0x87ad80). The slots the last
   match's unsynced end (win poses, results) left differ per side, so both
   sides start clear. */
static void clearSessionPads()
{
    for (const std::uint32_t slot : {0u, 1u, 5u, 6u})
        std::memset(recomp_memory(0x00a94a70u + slot * 0x30u, 0x30u), 0, 0x30u);
    for (std::uint32_t player = 0u; player < 4u; ++player)
        std::memset(recomp_memory(0x0087d260u + player * 0x30u, 0x30u), 0, 0x30u);
    std::memset(recomp_memory(0x0087a948u, 0x200u), 0, 0x200u);
    std::memset(recomp_memory(0x00878310u, 0x80u), 0, 0x80u);
    std::memset(recomp_memory(0x00877f00u, 0x400u), 0, 0x400u);
    std::memset(recomp_memory(0x0087ad80u, 0x80u), 0, 0x80u);
}

static void startLiveSession(std::uint32_t swap)
{
    RecompRollbackSession &s = rollback.session;
    rollback.livePending = false;
    rollback.liveStopPending = false;
    rollback.liveLockstepStopped = false;
    rollback.liveAbandon = false;
    /* Live is the first mode where one runner plays several sessions in a row, so the
       match starts with its own counters, desync checks and pacing: a check frame that
       equals the last match's final comparison still compares, and the match's summary
       and its stall grace count only this match. */
    rollback.compared = rollback.matched = 0u;
    rollback.paced = UINT32_MAX;
    rollback.rollbacks = rollback.replayed = rollback.waited = rollback.saves = 0u;
    rollback.advantage = 0;
    rollback.interrupted = false;
    rollback.contact = std::chrono::steady_clock::time_point{};
    rollback.slowestHashMs = 0.0;
    rollback.next_frame = std::chrono::steady_clock::time_point{};
    // The last match's part second of hitch measurements must not print at this one's start.
    rollback.hitchSince = rollback.frameLeft = rollback.rerunFrom = std::chrono::steady_clock::time_point{};
    rollback.hitchFrom = rollback.heldBack = rollback.rerunFrames = 0u;
    rollback.peerWaitMs = rollback.longestWaitMs = rollback.slowestFrameMs = rollback.rerunMs = 0.0;
    rollback.lateMs = rollback.blockedMs = 0.0;
    // A check hashed but never confirmed and the last frame's wait belong to the last match:
    // this one confirms only its own checks and paces only its own frames. The end of a
    // session clears these too, so whichever side of the gap the match starts from agrees.
    rollback.checkFrame = 0u;
    rollback.checkHash = 0u;
    rollback.wait = 0u;
    // A later match counts its frames from 0 again, so the last session's packets must not
    // reach it: drop what the route still holds, then start the counters fresh.
    recomp_live_transport_session_begin();
    RecompRollbackSession stale;
    recomp_rollback_init(&stale, 0u);
    for (int i = 0; i < 8; ++i) recomp_netplay_receive(&rollback.net, &stale);
    resetNetplayCounters();
    recomp_rollback_init(&s, 0u);
    s.window = 0u; // lockstep until the frontier opens it at frame 10
    clearSessionPads();
    if (!recomp_rollback_ring_init(RECOMP_ROLLBACK_WINDOW)) recomp_stop(4, "rollback:ring");
    rollback.active = true;
    rollback.started = std::chrono::steady_clock::now();
    if (const char *flip = std::getenv("RECOMP_ROLLBACK_FLIP"); flip != nullptr)
        rollback.flip = static_cast<std::uint32_t>(std::strtoul(flip, nullptr, 10));
    std::fprintf(stderr, "recomp live: session starts at swap %u as player %u\n", swap, rollback.player);
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

/* In live rounds, a second with a visible hitch gets one log line that says
   whose side it came from: time this side lacked the other player's pads once
   its frame was due, its own slowest frame (the guest, any rerun frames and
   present between two frontier waits, or a frame it noticed late), and frames
   that frame sync held back so the other side could catch up. Quiet seconds
   log nothing. */
static void measureHitch(std::chrono::steady_clock::time_point entered)
{
    using namespace std::chrono;
    RollbackRun &r = rollback;
    const steady_clock::time_point left = steady_clock::now();
    const bool live = r.session.window != 0u;
    // frameLeft is only set after a live wait, so a round's last frame counts as its window closes.
    if (r.frameLeft != steady_clock::time_point{})
        r.slowestFrameMs = std::max(r.slowestFrameMs, duration<double, std::milli>(entered - r.frameLeft).count());
    if (r.rerunFrom != steady_clock::time_point{}) {
        r.rerunMs += duration<double, std::milli>(entered - r.rerunFrom).count();
        r.rerunFrames += r.rerunDepth;
        r.rerunFrom = steady_clock::time_point{};
    }
    if (live && r.frameLeft != steady_clock::time_point{}) {
        r.slowestFrameMs = std::max(r.slowestFrameMs, r.lateMs);
        r.peerWaitMs += r.blockedMs;
        r.longestWaitMs = std::max(r.longestWaitMs, r.blockedMs);
    }
    r.frameLeft = live ? left : steady_clock::time_point{};
    // A round's end flushes its last part second.
    const bool measuring = r.hitchSince != steady_clock::time_point{};
    if (measuring && live && left - r.hitchSince < seconds(1)) return;
    if (measuring && (r.peerWaitMs >= 50.0 || r.slowestFrameMs >= 33.0 || r.heldBack >= 2u))
        std::fprintf(stderr, "recomp netplay: hitch in frames %u-%u: waited %.0f ms for the other player "
            "(longest %.0f ms), slowest frame here %.0f ms, ran %u frames again in %.0f ms, "
            "held back %u frames for them\n",
            r.hitchFrom, r.session.frame, r.peerWaitMs, r.longestWaitMs, r.slowestFrameMs,
            r.rerunFrames, r.rerunMs, r.heldBack);
    r.hitchSince = live ? left : steady_clock::time_point{};
    r.hitchFrom = r.session.frame;
    r.peerWaitMs = r.longestWaitMs = r.slowestFrameMs = r.rerunMs = 0.0;
    r.heldBack = r.rerunFrames = 0u;
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
    if (!rollback.active || !networked() || s.frame == 0u) return;
    const std::uint32_t running = s.frame - 1u;
    if (s.remote_count <= running)
        std::fprintf(stderr, "recomp netplay: save in frame %u waits for the other player's input\n", running);
    const auto began = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point sent{};
    while (s.remote_count <= running) {
        recomp_netplay_receive(&rollback.net, &s);
        repeatPads(sent);
        stalled(began);
        // Live: the other side's session is gone, so this save goes ahead unsynced.
        if (rollback.liveAbandon) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    resumed(began);
    if (s.rewind_to <= running) recomp_stop(4, "rollback:save-mispredicted");
}

static void sayGoodbye()
{
    /* Live: a running session's route replaces a socket of our own, so only then is
       there one to tell. The other runner ends its session at once instead of waiting
       for its timeout, and lets the game handle the empty seat itself. A Loopback run
       has no peer. */
    if (rollback.mode == RollbackMode::Live && rollback.active) recomp_netplay_send_goodbye(&rollback.net);
}

/* Every 30th frame of a Live session, each side hashes guest RAM as the frame
   starts, without SyncTest's host-scheduled ranges. Once the other player's
   pads before that frame have all arrived, no rewind reaches it again, so
   the hash is final and rides in each packet. A different hash from the
   other side for the same frame ends the session. */
constexpr std::uint32_t desyncCheckFrames = 30u;

static bool isCheckFrame(std::uint32_t frame)
{
    return networked() && frame != 0u && frame % desyncCheckFrames == 0u;
}

static void hashCheckFrame()
{
    const std::uint32_t frame = rollback.session.frame;
    if (!isCheckFrame(frame)) return;
    const auto began = std::chrono::steady_clock::now();
    // A Live fight hashes only the fight: each side's Live layer, profile, HUD, draw
    // state, cloth and stage effects differ by design (live_fight_rules.c).
    RecompRamRange ignored[RECOMP_SNAPSHOT_HASH_RANGES];
    const std::size_t count = recomp_live_hash_ignored(ignored, RECOMP_SNAPSHOT_HASH_RANGES);
    if (count == 0u || !recomp_rollback_ring_hash(ignored, count, &rollback.checkHash))
        recomp_stop(4, "rollback:hash");
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
    const auto began = std::chrono::steady_clock::now();
    const bool saved = recomp_rollback_ring_save(s.frame);
    rollback.saveMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    return saved;
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
    failSession(RECOMP_LIVE_END_DESYNC);
}

static void onRollbackFrame(std::uint32_t swap)
{
    RecompRollbackSession &s = rollback.session;
    if (!rollback.active) {
        if (rollback.mode == RollbackMode::Live) {
            // A Live session starts at the frame boundary after the round handshake.
            if (!rollback.livePending || recomp_fiber_adapter_model()->current_handle == 0u) return;
            startLiveSession(swap);
        } else {
            if (swap < rollback.start || recomp_fiber_adapter_model()->current_handle == 0u) return;
            recomp_rollback_init(&s, 0u);
            if (!recomp_rollback_ring_init(RECOMP_ROLLBACK_WINDOW)) recomp_stop(4, "rollback:ring");
            rollback.active = true;
            rollback.started = std::chrono::steady_clock::now();
            if (const char *flip = std::getenv("RECOMP_ROLLBACK_FLIP"); flip != nullptr)
                rollback.flip = static_cast<std::uint32_t>(std::strtoul(flip, nullptr, 10));
            std::fprintf(stderr, "recomp rollback: session starts at swap %u as player %u\n",
                swap, rollback.player);
        }
    }
    bool saved = true;
    if (s.frame == rollback.frontier) {
        if (rollback.mode == RollbackMode::Live) {
            // The fight holds still until lockstep batch 10 (game_mode_match_update), and a
            // rematch saves meanwhile. The match ends where the game stops its lockstep, on the
            // same frame on both sides; what follows (results, the return to the lobby) runs on
            // the game's own Live messages, unsynced.
            setWindow(recomp_live_window(recomp_game_round_live(), s.frame));
            if (rollback.liveLockstepStopped && rollback.stop > s.frame) {
                std::fprintf(stderr, "recomp live: lockstep stopped, session ends at frame %u\n", s.frame);
                rollback.stop = s.frame;
            }
            rollback.liveLockstepStopped = false;
            if (*recomp_memory(0x008a56d0u, 1u) != 5u && rollback.stop > s.frame) {
                std::fprintf(stderr, "recomp live: fight left at session frame %u\n", s.frame);
                rollback.stop = s.frame;
            }
        }
        if (networked() && rollback.paced != s.frame) {
            scheduleFrame();
            rollback.paced = s.frame;
        }
        const auto entered = std::chrono::steady_clock::now();
        if (rollback.rerunFrom != std::chrono::steady_clock::time_point{})
            rollback.rerunTotalMs += std::chrono::duration<double, std::milli>(entered - rollback.rerunFrom).count();
        const std::uint32_t target = awaitFrontier(s.frame == rollback.stop);
        if (networked()) measureHitch(entered);
        else rollback.rerunFrom = {};
        // Live: the other side left or this session failed; hand the fight back, runner running.
        if (liveSessionEnded()) return;
        if (target != RECOMP_ROLLBACK_NONE) {
            rollback.rerunFrom = std::chrono::steady_clock::now();
            rollback.rerunDepth = rollback.frontier - target;
            // The output pump must not play the buffers mid-rewind; the frames run again are muted anyway.
            recomp_dsound_buffer_set_output_suppressed(true);
            // shortcut: a save that cannot be rewound ends the run; netplay must drop to lockstep before the game saves.
            if (!recomp_rollback_ring_load(target) || recomp_kernel_file_state_lost()) {
                failSession(RECOMP_LIVE_END_REWIND);
                if (liveSessionEnded()) return;
            }
            rollback.loadMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rollback.rerunFrom).count();
            ++rollback.rollbacks;
            rollback.replayed += rollback.frontier - target;
        } else if (s.frame == rollback.stop) {
            finishRollback();
            return;
        } else {
            if (networked()) {
                confirmCheck();
                compareChecks();
                // Live: a desync found at the newest frame ends the session here.
                if (liveSessionEnded()) return;
            }
            if (rollback.flip != 0u && s.frame == rollback.flip) {
                *recomp_memory(0x0087ae54u, 1u) ^= 1u; // fighter 1's life, which persists
                std::fprintf(stderr, "recomp rollback: test flip of guest RAM at frame %u\n", s.frame);
            }
            saved = saveFrame();
            if (networked()) {
                rollback.advantage = static_cast<std::int32_t>(s.frame - rollback.net.peer_frame);
                recomp_netplay_send(&rollback.net, &s, rollback.advantage);
                const std::uint32_t wait = recomp_netplay_wait_frames(&rollback.net, rollback.advantage);
                rollback.waited += wait;
                if (s.window != 0u) rollback.heldBack += wait;
                rollback.wait = wait;
            }
        }
    } else {
        saved = saveFrame();
    }
    if (!saved) recomp_stop(4, "rollback:save");
    hashCheckFrame();
    // Frames run again were already shown and heard with the predicted pads.
    rollback.hidden = s.frame < rollback.frontier;
    recomp_d3d8_set_present_suppressed(rollback.hidden);
    recomp_d3d8_set_gpu_work_suppressed(rollback.hidden);
    recomp_dsound_buffer_set_output_suppressed(rollback.hidden);
    recomp_hit_voice_set_rollback_frame(s.frame, rollback.hidden);
    recomp_rollback_run(&s, &rollback.pads[rollback.player], &rollback.pads[1u - rollback.player]);
    if (rollback.mode == RollbackMode::Live &&
        recomp_live_pads_carry_stop(rollback.pads[0].buttons, rollback.pads[1].buttons)) {
        // Either side's stop request rides a pad; mask it out of both and reset both locksteps
        // at the start of this frame. The session then ends at the next frontier.
        rollback.pads[0].buttons = recomp_live_pad_mask_stop(rollback.pads[0].buttons);
        rollback.pads[1].buttons = recomp_live_pad_mask_stop(rollback.pads[1].buttons);
        recomp_live_stop_lockstep();
        rollback.liveLockstepStopped = true;
    }
    if (s.frame > rollback.frontier) rollback.frontier = s.frame;
}

static bool readRollbackConfig()
{
    const char *mode = std::getenv("RECOMP_ROLLBACK");
    if (mode == nullptr || *mode == '\0' || std::strcmp(mode, "0") == 0) return true;
    if (std::strcmp(mode, "loopback") != 0) return false;
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
    rollback.mode = RollbackMode::Loopback;
    rollback.player = 1u;
    if (const char *frames = std::getenv("RECOMP_ROLLBACK_FRAMES"); frames == nullptr || *frames == '\0')
        rollback.stop = 600u;
    number("RECOMP_ROLLBACK_LAG", rollback.lag);
    return valid && rollback.lag <= RECOMP_ROLLBACK_WINDOW;
}

void recomp_netplay_session_init(const RecompNetplaySessionHost &runner)
{
    host = runner;
}

void recomp_netplay_session_set_live(bool on)
{
    if (on) {
        if (rollback.mode != RollbackMode::Off) {
            std::fprintf(stderr, "recomp live: another session mode is set; Live stays off\n");
            return;
        }
        rollback.mode = RollbackMode::Live;
        return;
    }
    if (rollback.mode == RollbackMode::Live) rollback.mode = RollbackMode::Off;
}

/* Live starts unless RECOMP_LIVE=0, and never while a Loopback test session
   or a synctest runs: those own the frame clock, the saves and the runner's
   exit, and a synctest's rewinds would resend Live traffic. The run is
   offline until the game logs on in DOA ONLINE; the runner then switches it
   online (frame clock, synchronous reads, no mods that change the fight). */
void recomp_netplay_session_start_live()
{
    const char *on = std::getenv("RECOMP_LIVE");
    if (on != nullptr && std::strcmp(on, "0") == 0) return;
    if (const char *test = std::getenv("RECOMP_ROLLBACK"); test != nullptr && *test != '\0' &&
        std::strcmp(test, "0") != 0)
        return;
    if (recomp_synctest_enabled()) return;
    const std::string storage = (host.disc / ".recomp-storage").string();
    const RecompLiveBackend *backend = recomp_live_client_start(storage.c_str(), host.digest.c_str());
    if (backend == nullptr) {
        std::fprintf(stderr, "recomp live: could not start; Xbox Live stays offline\n");
        return;
    }
    recomp_live_backend_set(backend);
    recomp_netplay_session_set_live(true);
    if (!recomp_netplay_session_live()) return;
    liveOn = true;
    std::atexit(recomp_live_client_stop);
    std::fprintf(stderr, "recomp live: on\n");
}

bool recomp_netplay_session_live()
{
    return rollback.mode == RollbackMode::Live;
}

/* live_session.h: what live_transport.c and live_fight.c read. */
bool recomp_live_rollback_active(void)
{
    return rollback.active;
}

bool recomp_live_rollback_hidden(void)
{
    return rollback.hidden;
}

std::uint32_t recomp_live_rollback_frame(void)
{
    return rollback.session.frame;
}

unsigned recomp_live_rollback_player(void)
{
    return rollback.player;
}

/* live_fight.c: the round-start handshake returned, so the session starts at the
   next frame boundary. The player index comes from the two fighters' virtual
   addresses, which both sides read the same way. */
void recomp_live_round_started(std::uint32_t opponent_ina)
{
    if (rollback.mode != RollbackMode::Live) return;
    if (rollback.active) {
        std::fprintf(stderr, "recomp live: round start inside session frame %u\n", rollback.session.frame);
        return;
    }
    if (opponent_ina == 0u) {
        std::fprintf(stderr, "recomp live: the round start names no opponent\n");
        return;
    }
    std::uint32_t local = 0u;
    if (recomp_game_live_fight_pair(&local, nullptr)) rollback.player = recomp_live_player_index(local, opponent_ina);
    else
        std::fprintf(stderr, "recomp live: cannot tell the local fighter; player index stays %u\n", rollback.player);
    rollback.liveTail = false; // a new round supersedes the last fight's tail
    if (!recomp_live_transport_bind_rollback(opponent_ina, &rollback.net)) {
        std::fprintf(stderr, "recomp live: no route to the opponent; the game keeps its own lockstep\n");
        return;
    }
    resetNetplayCounters();
    rollback.livePending = true;
}

/* live_fight.c: the game asked its lockstep to stop. The request rides this side's
   next session pad; a frame run again drops it, since the match end runs in lockstep. */
void recomp_live_request_stop(void)
{
    if (rollback.mode != RollbackMode::Live || !rollback.active || rollback.hidden) return;
    rollback.liveStopPending = true;
}

bool recomp_netplay_session_read_env()
{
    // Live starts its sessions from the game's own pages, not from an env switch.
    if (rollback.mode == RollbackMode::Live) return true;
    return readRollbackConfig();
}

bool recomp_netplay_session_enabled()
{
    return rollback.mode != RollbackMode::Off;
}

void recomp_netplay_session_install(bool synctest)
{
    const bool enabled = recomp_netplay_session_enabled();
    if (enabled) {
        // Rollback input lives in the session, outside the saved state.
        recomp_d3d_frame_boundary_hook = onRollbackFrame;
        recomp_kernel_profile_mutation_hook = confirmSaveFrame;
    }
    (void)synctest; // the runner's setOnlineMode sets the DirectSound frame clock
    // Loopback compares whole guest RAM dumps, flare counts included; a session's checks skip them.
    recomp_d3d8_set_save_waits_for_visibility(!networked());
}

bool recomp_netplay_session_pad(std::uint32_t port, RecompInputGamepad *pad, bool *present)
{
    // Both runners must see the same controllers: the session's two ports, once it runs.
    // A host pad unplugged or plugged in mid-session never reaches the guest.
    if (rollback.active) {
        if (port >= 2u) {
            *present = false;
            return true;
        }
        // Loopback has no local player, so its port stays empty.
        *present = !(rollback.mode == RollbackMode::Loopback && port == rollback.player);
        if (*present) *pad = rollback.pads[port];
        return true;
    }
    return false;
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

void recomp_netplay_session_on_swap()
{
    pumpLiveTail();
    if (!liveOn) return;
    const std::uint64_t now = netplay_now_ms(); /* the clock the transport stamps its peers with */
    recomp_live_transport_pump(now);
    recomp_live_pump(now);
}

void recomp_netplay_session_exit()
{
    sayGoodbye();
}
