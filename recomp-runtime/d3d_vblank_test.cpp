#include "d3d_vblank.h"
#include "xapi_time_adapter.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

static unsigned waits;
static uint64_t last_deadline;
static uint32_t current_thread = 1u;
static bool fast_forward;
static bool fast_forward_hook() { return fast_forward; }
extern "C" void recomp_kernel_wait_for_vblank(uint64_t deadline)
{
    ++waits;
    last_deadline = deadline;
    recomp_xapi_skip_wait(deadline);
}
extern "C" uint32_t recomp_kernel_current_thread_id(void) { return current_thread; }

int main()
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    // A VRR present waits for its 1/60 s slot; a late one presents now and moves the grid.
    if (recomp_d3d_vrr_slot(1000000000, 1000000001) != 1016666666 ||
        recomp_d3d_vrr_slot(1016666666, 1040000000) != 1040000000 ||
        recomp_d3d_vrr_slot(1040000000, 1040000000) != 1056666666) {
        std::fprintf(stderr, "vrr: present slots left the 1/60 s grid\n");
        return 1;
    }
    // Under the frame clock, Swap is the vblank: a wait each frame must not
    // round up to the refresh after the frame's end and run time double.
    recomp_xapi_frame_clock_set_enabled(true);
    recomp_d3d_vblank_reset();
    const uint64_t frame_start = recomp_xapi_performance_counter();
    for (unsigned i = 0; i < 10; ++i) {
        recomp_d3d_wait_vblank();
        recomp_d3d_wait_present();
        recomp_xapi_frame_clock_advance();
    }
    const uint64_t frame_span = recomp_xapi_performance_counter() - frame_start;
    if (frame_span < 166666660u || frame_span > 166700000u) {
        std::fprintf(stderr, "vblank: 10 frame-clock frames took %llu ns\n",
            static_cast<unsigned long long>(frame_span));
        return 1;
    }
    recomp_xapi_frame_clock_set_enabled(false);
    waits = 0;
    recomp_d3d_vblank_reset();
    const char *unpaced = std::getenv("RECOMP_UNPACED");
    if (unpaced != nullptr && std::strcmp(unpaced, "1") == 0) {
        const auto host_start = Clock::now();
        const uint64_t guest_start = recomp_xapi_performance_counter();
        const uint64_t ticks_start = xbox_ReadTimeStampCounter();
        for (unsigned i = 0; i < 60; ++i) {
            recomp_d3d_wait_vblank();
            recomp_d3d_wait_present();
        }
        const uint64_t elapsed = recomp_xapi_performance_counter() - guest_start;
        if (waits != 60 || elapsed < 999000000u ||
            xbox_ReadTimeStampCounter() - ticks_start < 732000000u ||
            Clock::now() - host_start >= 500ms) {
            std::fprintf(stderr, "unpaced: 60 refreshes must advance guest time without host pacing\n");
            return 1;
        }
        const uint64_t before = recomp_xapi_performance_counter();
        recomp_xapi_skip_wait(guest_start); // An older peer deadline cannot rewind time.
        if (recomp_xapi_performance_counter() < before) return 1;
        recomp_d3d_wait_present();
        if (waits != 61) return 1;
        return 0;
    }
    auto fastest = 1s + Clock::duration::zero();
    for (unsigned i = 0; i < 5; ++i) {
        recomp_d3d_wait_vblank();
        const auto start = Clock::now();
        recomp_d3d_wait_present();
        const auto elapsed = Clock::now() - start;
        if (elapsed < fastest) fastest = elapsed;
    }
    // Use the fastest sample so a host scheduling interruption cannot make
    // an otherwise immediate submit fail. A second refresh takes ~16.7 ms.
    if (fastest >= 8ms) {
        std::fprintf(stderr, "vblank: Present waits again after an explicit wait\n");
        return 1;
    }
    // A wake that lands late leaves the next refresh under one interval away,
    // so keep the slowest of three; a reused refresh makes all of them immediate.
    auto slowest = Clock::duration::zero();
    for (unsigned i = 0; i < 3; ++i) {
        const auto present_start = Clock::now();
        recomp_d3d_wait_present();
        const auto elapsed = Clock::now() - present_start;
        if (elapsed > slowest) slowest = elapsed;
    }
    if (slowest < 8ms) {
        std::fprintf(stderr, "vblank: consecutive presents reused a refresh\n");
        return 1;
    }
    // A fast-forwarded stretch moves guest time ahead of host time; paced
    // refreshes after it must still take one interval, not the skipped span.
    recomp_xapi_fast_forward = fast_forward_hook;
    fast_forward = true;
    for (unsigned i = 0; i < 120; ++i) recomp_d3d_wait_present();
    fast_forward = false;
    const auto start = Clock::now();
    recomp_d3d_wait_present();
    recomp_d3d_wait_present();
    if (Clock::now() - start >= 500ms) {
        std::fprintf(stderr, "vblank: paced wait after fast-forward used host time\n");
        return 1;
    }
    // A waiter kept off the CPU past refreshes is handed each one it missed
    // (CRI refills its streams once per wake, issue #145), a few at most.
    constexpr uint64_t interval_ns = 1000000000u / 60u;
    recomp_d3d_vblank_reset();
    recomp_d3d_wait_vblank();
    const uint64_t seen = last_deadline;
    std::this_thread::sleep_for(60ms);  // three or four refreshes
    for (uint64_t i = 1; i <= 3; ++i) {
        recomp_d3d_wait_vblank();
        if (last_deadline != seen + i * interval_ns) {
            std::fprintf(stderr, "vblank: a late waiter lost missed refresh %llu\n",
                static_cast<unsigned long long>(i));
            return 1;
        }
    }
    current_thread = 2u;  // another thread has missed nothing of its own
    const uint64_t other_start = recomp_xapi_performance_counter();
    recomp_d3d_wait_vblank();
    if (last_deadline < other_start) {
        std::fprintf(stderr, "vblank: a first wait was handed a past refresh\n");
        return 1;
    }
    current_thread = 1u;
    std::this_thread::sleep_for(150ms);  // past the catch-up limit: resume at the next refresh
    const uint64_t resume_start = recomp_xapi_performance_counter();
    recomp_d3d_wait_vblank();
    if (last_deadline < resume_start) {
        std::fprintf(stderr, "vblank: a long stall replayed its missed refreshes\n");
        return 1;
    }
    // Present's wait for a pending flip never takes a missed refresh.
    std::this_thread::sleep_for(35ms);  // two or three refreshes
    recomp_d3d_wait_present();
    uint64_t pending[2];
    recomp_d3d_vblank_state_save(pending);
    waits = 0;
    recomp_d3d_wait_present();
    if (waits != 1u || last_deadline < pending[1]) {
        std::fprintf(stderr, "vblank: Present queued a flip on a pending refresh\n");
        return 1;
    }
    return 0;
}
