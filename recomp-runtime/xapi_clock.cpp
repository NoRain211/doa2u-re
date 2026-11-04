#include "xapi_time_adapter.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<uint64_t> skipped_ns{0};
}

bool (*recomp_xapi_fast_forward)(void) = nullptr;

uint64_t recomp_xapi_performance_counter(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) +
        skipped_ns.load(std::memory_order_relaxed);
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
