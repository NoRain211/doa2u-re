#include "d3d_vblank.h"
#include "xapi_time_adapter.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

static unsigned waits;
static bool fast_forward;
static bool fast_forward_hook() { return fast_forward; }
extern "C" void recomp_kernel_wait_for_vblank(uint64_t deadline)
{
    ++waits;
    recomp_xapi_skip_wait(deadline);
}

int main()
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
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
    return 0;
}
