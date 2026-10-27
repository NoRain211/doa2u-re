#include "xapi_time_adapter.h"

#include <cstdio>

namespace {
constexpr uint64_t frame_ns = UINT64_C(1000000000) / 60u;

int failures;

void check(const char *name, bool passed)
{
    if (!passed) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}
}

int main()
{
    recomp_xapi_frame_clock_set_enabled(true);
    check("enabled", recomp_xapi_frame_clock_enabled());
    check("starts at frame end", recomp_xapi_performance_counter() == frame_ns);

    // Time is constant inside a frame.
    check("constant in frame", recomp_xapi_performance_counter() == frame_ns);
    recomp_xapi_frame_clock_advance();
    check("one frame", recomp_xapi_performance_counter() == frame_ns * 2u);
    recomp_xapi_frame_clock_advance();
    check("two frames", recomp_xapi_performance_counter() == frame_ns * 3u);

    // A wait reaches its deadline at once and moves the next frame.
    const uint64_t wait_deadline = frame_ns * 3u + 25000000u;
    check("wait skipped", recomp_xapi_skip_wait(wait_deadline));
    // Reads on the guest thread add a 1 us tick.
    const uint64_t reached = recomp_xapi_performance_counter();
    check("wait reaches deadline", reached >= wait_deadline && reached - wait_deadline <= 1000u);
    check("earlier wait keeps time", recomp_xapi_skip_wait(frame_ns * 3u + 1000u) &&
        recomp_xapi_performance_counter() > reached);
    recomp_xapi_frame_clock_advance();
    check("wait moves next frame", recomp_xapi_performance_counter() == frame_ns * 4u + 25000000u);
    recomp_xapi_frame_clock_advance();
    check("wait does not repeat", recomp_xapi_performance_counter() == frame_ns * 5u + 25000000u);

    // A wait ending by the next frame's end does not stretch this frame.
    const uint64_t frame_end = recomp_xapi_performance_counter();
    check("one-frame sleep skipped", recomp_xapi_skip_wait(frame_end + frame_ns));
    check("one-frame sleep reached", recomp_xapi_performance_counter() >= frame_end + frame_ns);
    recomp_xapi_frame_clock_advance();
    check("one-frame sleep keeps the frame", recomp_xapi_performance_counter() == frame_end + frame_ns);

    // The game reads this counter through RDTSC at 733,333,333 Hz.
    const uint64_t now = recomp_xapi_performance_counter();
    const uint64_t tsc = xbox_ReadTimeStampCounter();
    check("tsc tracks counter", tsc == now / 1000000000u * 733333333u +
        now % 1000000000u * 733333333u / 1000000000u);

    RecompFrameClockState saved;
    recomp_xapi_frame_clock_save(&saved);
    const uint64_t before = recomp_xapi_performance_counter();
    recomp_xapi_frame_clock_advance();
    check("moved before restore", recomp_xapi_performance_counter() != before);
    recomp_xapi_frame_clock_restore(&saved);
    check("restored", recomp_xapi_performance_counter() == before);
    check("restored frame", recomp_xapi_frame_clock_enabled());

    recomp_xapi_frame_clock_set_enabled(false);
    check("disabled", !recomp_xapi_frame_clock_enabled());
    check("host clock resumes", recomp_xapi_performance_counter() > 0u);

    if (failures != 0) return 1;
    std::puts("PASS frame clock locks guest time to frame boundaries");
    return 0;
}
