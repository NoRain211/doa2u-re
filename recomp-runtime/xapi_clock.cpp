#include "xapi_time_adapter.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace {
std::atomic<uint64_t> skipped_ns{0};

/* A frame of guest time. The game stores its measured frame time in
   1/6000 s units, so a whole frame is exactly 100 of them. */
constexpr uint64_t frame_ns = UINT64_C(1000000000) / 60u;
std::atomic<bool> frame_clock_enabled{false};
std::atomic<uint64_t> frame_clock_base{0};
std::atomic<uint64_t> frame_clock_offset{0};
std::atomic<uint64_t> frame_clock_wait{0};
std::atomic<uint64_t> frame_clock_frame{0};
/* Timing loops in the guest poll the counter. A small quantum lets them make
   progress; it is discarded at Swap so frame time stays reproducible. A
   coarser quantum keeps those loops from spending a whole frame's worth of
   host time per vblank: 1 us reaches a 60 Hz deadline in a few thousand reads. */
std::atomic<uint64_t> frame_clock_tick_ns{1000};
std::atomic<unsigned long long> frame_clock_host_thread{0};
/* The guest has read the real clock: enabling the frame clock later must
   start it there, not at zero. A boot that enables it before the guest runs
   starts at zero, as the console does. */
std::atomic<bool> real_clock_used{false};
/* Added to the counter-derived UTC while the frame clock runs, so the switch
   from the host's wall clock keeps the guest's date continuous. 100 ns units. */
std::atomic<uint64_t> frame_clock_utc_offset{0};

unsigned long long host_thread_id(void)
{
#ifdef _WIN32
    return static_cast<unsigned long long>(GetCurrentThreadId());
#else
    return (unsigned long long)(uintptr_t)pthread_self(); // pthread_t is a pointer on macOS
#endif
}

struct FrameClockEnv {
    FrameClockEnv()
    {
        const char *value = std::getenv("RECOMP_FRAME_CLOCK");
        if (value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0)
            frame_clock_enabled.store(true, std::memory_order_relaxed);
        const char *tick = std::getenv("RECOMP_FRAME_CLOCK_TICK_NS");
        if (tick != nullptr && *tick != '\0') {
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(tick, &end, 10);
            if (*end == '\0') frame_clock_tick_ns.store(parsed, std::memory_order_relaxed);
        }
    }
};
const FrameClockEnv frame_clock_env;

/* Guest time inside a frame is the frame's end. A wait that runs past it
   moves the next frame, so time never jumps between two reads in one frame. */
uint64_t frame_clock_displayed(void)
{
    const uint64_t base = frame_clock_base.load(std::memory_order_relaxed);
    const uint64_t offset = frame_clock_offset.load(std::memory_order_relaxed);
    uint64_t ahead = frame_ns;
    if (offset > ahead) ahead = offset;
    return base + ahead;
}

/* The free-running host clock the guest reads while the frame clock is off. */
uint64_t real_counter(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) +
        skipped_ns.load(std::memory_order_relaxed);
}

uint64_t host_utc_100ns(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) / 100u;
}
}

bool (*recomp_xapi_fast_forward)(void) = nullptr;

uint64_t recomp_xapi_performance_counter(void)
{
    if (frame_clock_enabled.load(std::memory_order_relaxed)) {
        const uint64_t tick = frame_clock_tick_ns.load(std::memory_order_relaxed);
        if (tick != 0u && frame_clock_host_thread.load(std::memory_order_relaxed) == host_thread_id())
            frame_clock_offset.fetch_add(tick, std::memory_order_relaxed);
        return frame_clock_displayed();
    }
    if (!real_clock_used.load(std::memory_order_relaxed))
        real_clock_used.store(true, std::memory_order_relaxed);
    return real_counter();
}

bool recomp_xapi_frame_clock_enabled(void)
{
    return frame_clock_enabled.load(std::memory_order_relaxed);
}

/* Enabling after the guest has read the real clock starts the frame clock
   where that clock now is: resetting to zero would take guest time backwards.
   A boot that enables it before the guest runs starts at zero, as it did.
   The state is written before the flag, so a reader never sees a half switch. */
void recomp_xapi_frame_clock_set_enabled(bool enabled)
{
    if (enabled) {
        if (frame_clock_enabled.load(std::memory_order_relaxed)) return;
        const bool resume = real_clock_used.load(std::memory_order_relaxed);
        const uint64_t now = resume ? real_counter() : 0u;
        recomp_xapi_frame_clock_reset();
        if (resume) {
            frame_clock_base.store(now > frame_ns ? now - frame_ns : 0u, std::memory_order_relaxed);
            frame_clock_utc_offset.store(host_utc_100ns() - now / 100u, std::memory_order_relaxed);
            real_clock_used.store(true, std::memory_order_relaxed);
        }
        frame_clock_enabled.store(true, std::memory_order_relaxed);
        return;
    }
    if (!frame_clock_enabled.load(std::memory_order_relaxed)) return;
    /* The guest goes back to the real clock; it must continue from at least
       the last frame time, which a wait may have moved past the host's. */
    const uint64_t displayed = frame_clock_displayed();
    const uint64_t real = real_counter();
    // Wraps when guest time ran behind, as recomp_xapi_hold_time relies on.
    skipped_ns.fetch_add(displayed - real, std::memory_order_relaxed);
    frame_clock_enabled.store(false, std::memory_order_relaxed);
    frame_clock_utc_offset.store(0u, std::memory_order_relaxed);
}

/* While the frame clock runs, the guest's UTC rides the counter; this keeps
   it where the host's wall clock was when the clock started. */
uint64_t recomp_xapi_frame_clock_utc_offset(void)
{
    return frame_clock_utc_offset.load(std::memory_order_relaxed);
}

void recomp_xapi_frame_clock_reset(void)
{
    /* Guest time starts over at zero, as on a boot: a later enable resets too. */
    real_clock_used.store(false, std::memory_order_relaxed);
    frame_clock_base.store(0, std::memory_order_relaxed);
    frame_clock_offset.store(0, std::memory_order_relaxed);
    frame_clock_wait.store(0, std::memory_order_relaxed);
    frame_clock_frame.store(0, std::memory_order_relaxed);
    frame_clock_host_thread.store(0, std::memory_order_relaxed);
    frame_clock_utc_offset.store(0, std::memory_order_relaxed);
}

void recomp_xapi_frame_clock_advance(void)
{
    if (!frame_clock_enabled.load(std::memory_order_relaxed)) return;
    const unsigned long long thread = host_thread_id();
    if (frame_clock_host_thread.load(std::memory_order_relaxed) == 0u)
        frame_clock_host_thread.store(thread, std::memory_order_relaxed);
    const uint64_t base = frame_clock_base.load(std::memory_order_relaxed);
    const uint64_t wait = frame_clock_wait.load(std::memory_order_relaxed);
    // Read ticks are discarded at the boundary; only real waits may move the
    // next frame past its nominal start. This keeps frame time reproducible.
    const uint64_t next = base + (wait > frame_ns ? wait : frame_ns);
    frame_clock_base.store(next, std::memory_order_relaxed);
    frame_clock_offset.store(0, std::memory_order_relaxed);
    frame_clock_wait.store(0, std::memory_order_relaxed);
    frame_clock_frame.fetch_add(1, std::memory_order_relaxed);
}

void recomp_xapi_frame_clock_save(RecompFrameClockState *state)
{
    state->base_ns = frame_clock_base.load(std::memory_order_relaxed);
    state->offset_ns = frame_clock_offset.load(std::memory_order_relaxed);
    state->wait_ns = frame_clock_wait.load(std::memory_order_relaxed);
    state->frame = frame_clock_frame.load(std::memory_order_relaxed);
    state->enabled = frame_clock_enabled.load(std::memory_order_relaxed);
}

void recomp_xapi_frame_clock_restore(const RecompFrameClockState *state)
{
    frame_clock_base.store(state->base_ns, std::memory_order_relaxed);
    frame_clock_offset.store(state->offset_ns, std::memory_order_relaxed);
    frame_clock_wait.store(state->wait_ns, std::memory_order_relaxed);
    frame_clock_frame.store(state->frame, std::memory_order_relaxed);
    frame_clock_enabled.store(state->enabled, std::memory_order_relaxed);
}

uint64_t recomp_xapi_performance_frequency(void)
{
    return UINT64_C(1000000000);
}

uint64_t xbox_ReadTimeStampCounter(void)
{
    const uint64_t now = recomp_xapi_performance_counter();
    const uint64_t frequency = recomp_xapi_performance_frequency();
    // 733,333,333 Hz, split to avoid overflowing the product.
    return now / frequency * 733333333u + now % frequency * 733333333u / frequency;
}

bool recomp_xapi_skip_wait(uint64_t deadline_ns)
{
    if (frame_clock_enabled.load(std::memory_order_relaxed)) {
        const uint64_t now = frame_clock_displayed();
        if (deadline_ns > now) {
            /* Jump to the deadline. Reaching it by read ticks cost thousands of
               idle scheduler passes per frame; a shorter later wait keeps it.
               Reads return the frame's end, so a thread sleeping one frame per
               loop is due at the next frame's end. Such a wait must not stretch
               the frame: two frames per Swap starved the movie audio stream. */
            const uint64_t ahead = deadline_ns - frame_clock_base.load(std::memory_order_relaxed);
            frame_clock_offset.store(ahead, std::memory_order_relaxed);
            if (ahead > 2u * frame_ns && ahead > frame_clock_wait.load(std::memory_order_relaxed))
                frame_clock_wait.store(ahead, std::memory_order_relaxed);
        }
        return true;
    }
    static const bool unpaced = [] {
        const char *value = std::getenv("RECOMP_UNPACED");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!unpaced && !(recomp_xapi_fast_forward != nullptr && recomp_xapi_fast_forward()))
        return false;
    // Only the guest execution thread advances time, after runnable work yields.
    const uint64_t now = recomp_xapi_performance_counter();
    if (deadline_ns > now)
        skipped_ns.fetch_add(deadline_ns - now, std::memory_order_relaxed);
    return true;
}

void recomp_xapi_hold_time(uint64_t held_ns)
{
    // Unsigned wraparound subtracts; the counter only stops while held.
    skipped_ns.fetch_sub(held_ns, std::memory_order_relaxed);
}
