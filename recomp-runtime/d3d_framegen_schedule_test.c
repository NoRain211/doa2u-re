// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_framegen_schedule.h"

#include <stdio.h>

static int failures;

static void check(const char *name, unsigned k, int64_t now_ns, unsigned next_slot, int slot, unsigned missed)
{
    const RecompFramegenDue due = recomp_framegen_due(k, 1000000, 1000000 + now_ns, next_slot);
    if (due.slot != slot || due.missed != missed) {
        fprintf(stderr, "framegen schedule %s: k=%u got slot=%d missed=%u, want slot=%d missed=%u\n",
            name, k, due.slot, due.missed, slot, missed);
        ++failures;
    }
}

int main(void)
{
    /* k = 4 at 240 Hz: slots every 4.1667 ms, slot 3 is the real frame. */
    check("arrival", 4u, 0, 0u, 0, 0u);
    check("before arrival", 4u, -1, 0u, -1, 0u);
    check("slot 1 on time", 4u, 4166667, 1u, 1, 0u);
    check("slot 1 a little late", 4u, 5000000, 1u, 1, 0u);
    check("slot 1 not yet due", 4u, 4000000, 1u, -1, 0u);
    check("slot 1 late", 4u, 6500000, 1u, -1, 1u);
    check("slot 1 missed, slot 2 due", 4u, 8400000, 1u, 2, 1u);
    check("in-betweens missed, real due", 4u, 13000000, 1u, 3, 2u);
    check("real never late", 4u, 16000000, 3u, 3, 0u);
    check("real after its slot", 4u, 30000000, 1u, 3, 2u);
    check("all shown", 4u, 13000000, 4u, -1, 0u);
    check("slot already shown", 4u, 4500000, 2u, -1, 0u);
    /* k = 2 at 120 Hz: slot 0 is the in-between, slot 1 the real frame at 8.333 ms. */
    check("k2 arrival", 2u, 100000, 0u, 0, 0u);
    check("k2 arrival late", 2u, 4500000, 0u, -1, 1u);
    check("k2 real on time", 2u, 8333334, 1u, 1, 0u);
    check("k2 real not yet due", 2u, 8000000, 1u, -1, 0u);
    check("k2 in-between missed", 2u, 9000000, 0u, 1, 1u);
    check("k1 unsupported", 1u, 0, 0u, -1, 0u);
    if (recomp_framegen_until(4u, 0, 1000000, 1u) != 3166666 ||
        recomp_framegen_until(4u, 0, 5000000, 1u) != 0 ||
        recomp_framegen_until(2u, 0, 0, 1u) != 8333333) {
        fprintf(stderr, "framegen schedule: until is wrong\n");
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
