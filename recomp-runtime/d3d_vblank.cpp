#include "d3d_vblank.h"
#include "xapi_time_adapter.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <thread>

extern "C" void recomp_kernel_wait_for_vblank(uint64_t deadline_ns);
extern "C" uint32_t recomp_kernel_current_thread_id(void);

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point last_vblank;
// The refresh at which the last Present flips.
Clock::time_point presented_vblank;
std::map<uint32_t, unsigned> block_callers;
unsigned present_waits;
Clock::time_point next_report;
// RECOMP_PERF_COUNTER: this frame's waits, printed when its present misses a vblank.
struct WaitEvent { uint32_t caller; Clock::time_point arrive, deadline, end; };
WaitEvent frame_waits[8];
unsigned frame_wait_count;
uint32_t next_caller;
#ifdef _WIN32
// The guest and presenter threads both sleep, so each gets its own timer.
thread_local std::unique_ptr<void, decltype(&CloseHandle)>
    high_resolution_timer(nullptr, &CloseHandle);
#endif
// ponytail: the supported progressive NTSC mode; other video modes need their rate.
constexpr auto interval = std::chrono::nanoseconds(1000000000 / 60);
constexpr auto spin_window = std::chrono::microseconds(500);
/* The refresh each guest thread last waited for. CRI's sound server waits
   every refresh and refills its streams once per wake; the Xbox preempts the
   game at each vblank to run it, but these cooperative threads run only when
   the game yields. A game that yields late each frame halved its wakes, the
   rings ran dry and speech replayed stale audio (issue #145), so a late
   waiter is handed the refreshes it missed, a few at most. */
struct VblankWaiter { uint32_t thread; Clock::time_point seen; };
VblankWaiter waiters[16];
constexpr int max_catch_up = 4;

Clock::time_point guest_now()
{
    return Clock::time_point(std::chrono::nanoseconds(recomp_xapi_performance_counter()));
}

// The first refresh on the last_vblank grid at or after now.
Clock::time_point nextRefresh(Clock::time_point now)
{
    return last_vblank + interval * ((now - last_vblank + interval - Clock::duration(1)) / interval);
}

// Sleeps until deadline on the clock that now() reads. Guest time runs ahead
// of steady_clock after a skip, so every wait is measured against now().
template <typename Now>
void sleepUntil(Clock::time_point deadline, Now now)
{
#ifdef _WIN32
    if (high_resolution_timer == nullptr) {
        high_resolution_timer.reset(CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE));
    }
    if (high_resolution_timer == nullptr) {
        std::this_thread::sleep_for(deadline - now());
        return;
    }
    const auto timer_wait = deadline - spin_window - now();
    if (timer_wait > Clock::duration::zero()) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(
            timer_wait).count() / 100;
        if (due.QuadPart < 0 &&
            SetWaitableTimer(high_resolution_timer.get(), &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(high_resolution_timer.get(), INFINITE);
        } else {
            std::this_thread::sleep_for(deadline - now());
            return;
        }
    }
    while (now() < deadline) YieldProcessor();
#else
    const auto sleep_wait = deadline - spin_window - now();
    if (sleep_wait > Clock::duration::zero()) {
        std::this_thread::sleep_for(sleep_wait);
    }
    while (now() < deadline) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }
#endif
}
}

double recomp_d3d_vblank_wait_ms;
void (*recomp_d3d_frame_breakdown)(char *out, size_t size);
unsigned recomp_d3d_submit_count;
double recomp_d3d_submit_mb;

void recomp_d3d_vblank_reset(void)
{
    last_vblank = guest_now();
    presented_vblank = last_vblank;
    for (auto &waiter : waiters) waiter = {};
}

void recomp_d3d_vblank_state_save(uint64_t *state)
{
    state[0] = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        last_vblank.time_since_epoch()).count());
    state[1] = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        presented_vblank.time_since_epoch()).count());
}

void recomp_d3d_vblank_state_restore(const uint64_t *state)
{
    last_vblank = Clock::time_point(std::chrono::nanoseconds(state[0]));
    presented_vblank = Clock::time_point(std::chrono::nanoseconds(state[1]));
}

// Present's wait for a pending flip passes catch_up false: a missed refresh
// is already past, so it would queue a second frame on the pending flip.
static void waitVblank(bool catch_up)
{
    const auto wait_start = Clock::now();
    // Every waiter in this refresh interval observes the same vblank.
    // Keep the deadline local across guest switches and nested waits.
    const auto now = guest_now();
    // A refresh exactly at now counts as reached. The frame clock reads every
    // frame's end, which sits on the grid, so rounding past it doubled each frame.
    if (last_vblank < now) last_vblank = nextRefresh(now);
    auto deadline = last_vblank;
    // The frame clock keeps netplay and synctest deterministic, so it keeps the plain wait.
    if (!recomp_xapi_frame_clock_enabled()) {
        const uint32_t thread = recomp_kernel_current_thread_id();
        VblankWaiter *waiter = nullptr;
        for (auto &entry : waiters) {
            if (entry.thread == thread) { waiter = &entry; break; }
            if (waiter == nullptr && entry.thread == 0u) waiter = &entry;
        }
        if (waiter != nullptr) {
            const auto missed = waiter->seen + interval;
            if (catch_up && waiter->thread == thread && missed < deadline && deadline - missed <= interval * max_catch_up)
                deadline = missed;
            *waiter = {thread, deadline};
        }
    }
    const auto deadline_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count());
    recomp_kernel_wait_for_vblank(deadline_ns);
    if (!recomp_xapi_skip_wait(deadline_ns)) {
        // Deadlines are guest time, which runs ahead of host time after a skip.
        sleepUntil(deadline, guest_now);
    }
    if (frame_wait_count < 8u) frame_waits[frame_wait_count++] = {next_caller, now, deadline, guest_now()};
    next_caller = 0u;
    recomp_d3d_vblank_wait_ms +=
        std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
}

void recomp_d3d_wait_vblank(void)
{
    waitVblank(true);
}

void recomp_d3d_wait_present(void)
{
    // Like Swap on hardware: block only while the previous flip is pending, then
    // queue this one for the next refresh. Waiting for that refresh here made a
    // guest that presented just before it miss the next one in its own wait.
    if (presented_vblank > guest_now()) {
        waitVblank(false);
        ++present_waits;
    }
    const auto now = guest_now();
    if (last_vblank < now) last_vblank = nextRefresh(now);
    const auto flip = last_vblank;
    static const bool enabled = std::getenv("RECOMP_PERF_COUNTER") != nullptr;
    char breakdown[512] = "";
    if (enabled && recomp_d3d_frame_breakdown != nullptr) recomp_d3d_frame_breakdown(breakdown, sizeof breakdown);
    if (enabled && flip - presented_vblank >= interval * 2) {
        // Times are ms after the previous flip; caller 0 is Present's own wait.
        const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::fprintf(stderr, "recomp vblank miss: gap_ms=%.1f", ms(flip - presented_vblank));
        for (unsigned i = 0u; i < frame_wait_count; ++i) {
            const WaitEvent &e = frame_waits[i];
            std::fprintf(stderr, " [%08x arrive=%.1f deadline=%.1f end=%.1f]", e.caller,
                ms(e.arrive - presented_vblank), ms(e.deadline - presented_vblank), ms(e.end - presented_vblank));
        }
        std::fprintf(stderr, " submit=%u/%.1fMB%s\n", recomp_d3d_submit_count,
            recomp_d3d_submit_mb, breakdown);
    }
    recomp_d3d_submit_count = 0u;
    frame_wait_count = 0u;
    presented_vblank = flip;
}

void recomp_d3d_note_block(uint32_t return_address)
{
    static const bool enabled = std::getenv("RECOMP_PERF_COUNTER") != nullptr;
    if (!enabled) return;
    next_caller = return_address;
    ++block_callers[return_address];
    const auto now = Clock::now();
    if (now < next_report) return;
    next_report = now + std::chrono::seconds(1);
    std::fprintf(stderr, "recomp vblank: present_waits=%u", present_waits);
    for (const auto &[caller, count] : block_callers) std::fprintf(stderr, " block@%08x=%u", caller, count);
    std::fputc('\n', stderr);
    block_callers.clear();
    present_waits = 0u;
}

void recomp_d3d_sleep_until(long long steady_ns)
{
    sleepUntil(Clock::time_point(std::chrono::nanoseconds(steady_ns)), Clock::now);
}
