// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_framegen_schedule.h"

#include <stdbool.h>

static int64_t slotNs(unsigned k)
{
    return 1000000000 / (60 * (int64_t)k);
}

RecompFramegenDue recomp_framegen_due(unsigned k, int64_t present_ns, int64_t now_ns, unsigned next_slot)
{
    RecompFramegenDue due = {-1, 0u};
    const int64_t elapsed = now_ns - present_ns;
    if (k < 2u || elapsed < 0 || next_slot >= k) return due;
    const int64_t spacing = slotNs(k);
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
    const int64_t until = present_ns + (int64_t)slot * slotNs(k) - now_ns;
    return until > 0 ? until : 0;
}
