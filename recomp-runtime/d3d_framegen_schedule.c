// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_framegen_schedule.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

int64_t recomp_framegen_slot_ns(unsigned k)
{
    return 1000000000 / (60 * (int64_t)k);
}

RecompFramegenDue recomp_framegen_due(unsigned k, int64_t present_ns, int64_t now_ns, unsigned next_slot)
{
    RecompFramegenDue due = {-1, 0u};
    const int64_t elapsed = now_ns - present_ns;
    if (k < 2u || elapsed < 0 || next_slot >= k) return due;
    const int64_t spacing = recomp_framegen_slot_ns(k);
    int64_t slot = elapsed / spacing;
    bool late = false;
    if (slot >= (int64_t)k - 1) slot = (int64_t)k - 1;
    else late = elapsed - slot * spacing > spacing / 2;
    if (slot < (int64_t)next_slot) return due;
    due.missed = (unsigned)(slot - next_slot) + (late ? 1u : 0u);
    if (!late) due.slot = (int)slot;
    return due;
}

int64_t recomp_framegen_until(unsigned k, int64_t present_ns, int64_t now_ns, unsigned slot)
{
    if (k < 2u) return 0;
    const int64_t until = present_ns + (int64_t)slot * recomp_framegen_slot_ns(k) - now_ns;
    return until > 0 ? until : 0;
}

RecompFramegenPlan recomp_framegen_plan(float refresh_hz, int vrr)
{
    RecompFramegenPlan plan = {0u, RECOMP_FRAMEGEN_SLOT_VSYNC, NULL};
    if (!(refresh_hz > 0.0f)) {
        plan.reason = "unknown refresh";
        return plan;
    }
    /* The nearest whole multiple of 60 Hz, and whether the display is one. */
    const unsigned multiple = (unsigned)(refresh_hz / 60.0f + 0.5f);
    const bool whole = multiple >= 1u &&
        fabsf(refresh_hz - 60.0f * (float)multiple) <= 0.005f * refresh_hz;
    if (whole) {
        if (multiple < 2u) {
            plan.reason = "the refresh is below 120 Hz";
            return plan;
        }
        plan.k = multiple;
        return plan;
    }
    if (!vrr) {
        plan.reason = "the refresh is not a whole multiple of 60 Hz";
        return plan;
    }
    /* Timed slots sit just below the panel's ceiling, 3 Hz at most, so VRR
       never runs at its maximum and neither tears nor falls back. */
    const unsigned k = (unsigned)((refresh_hz - 3.0f) / 60.0f);
    if (k < 2u) {
        plan.reason = "the refresh has no two slots below its maximum";
        return plan;
    }
    plan.k = k;
    plan.mode = RECOMP_FRAMEGEN_SLOT_TIMED;
    return plan;
}
