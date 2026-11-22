// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_framegen_schedule.h"

#include <stdio.h>
#include <string.h>

static int failures;

/* One display: the presents per guest frame, how they reach it, and why frame
   generation stays off when k is 0 (a NULL reason means it runs). */
static void checkPlan(const char *name, float refresh_hz, int vrr, unsigned k,
    RecompFramegenSlotMode mode, const char *reason)
{
    const RecompFramegenPlan plan = recomp_framegen_plan(refresh_hz, vrr);
    if (plan.k != k || plan.mode != mode ||
        (plan.reason != NULL) != (reason != NULL) ||
        (reason != NULL && strcmp(plan.reason, reason) != 0)) {
        fprintf(stderr, "framegen plan %s: %.2f Hz vrr=%d got k=%u mode=%d reason=%s, "
            "want k=%u mode=%d reason=%s\n", name, refresh_hz, vrr, plan.k,
            (int)plan.mode, plan.reason != NULL ? plan.reason : "(runs)", k, (int)mode,
            reason != NULL ? reason : "(runs)");
        ++failures;
    }
}

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
    if (recomp_framegen_slot_ns(2u) != 8333333 || recomp_framegen_slot_ns(4u) != 4166666) {
        fprintf(stderr, "framegen schedule: slot_ns is wrong\n");
        ++failures;
    }
    /* A whole multiple of 60 Hz keeps VSYNC slots with or without VRR. */
    checkPlan("120 vrr", 120.0f, 1, 2u, RECOMP_FRAMEGEN_SLOT_VSYNC, NULL);
    checkPlan("120", 120.0f, 0, 2u, RECOMP_FRAMEGEN_SLOT_VSYNC, NULL);
    checkPlan("240 vrr", 240.0f, 1, 4u, RECOMP_FRAMEGEN_SLOT_VSYNC, NULL);
    checkPlan("240", 240.0f, 0, 4u, RECOMP_FRAMEGEN_SLOT_VSYNC, NULL);
    /* VRR at another rate times its slots just below the panel's ceiling. */
    checkPlan("144 vrr", 144.0f, 1, 2u, RECOMP_FRAMEGEN_SLOT_TIMED, NULL);
    checkPlan("165 vrr", 165.0f, 1, 2u, RECOMP_FRAMEGEN_SLOT_TIMED, NULL);
    checkPlan("200 vrr", 200.0f, 1, 3u, RECOMP_FRAMEGEN_SLOT_TIMED, NULL);
    checkPlan("280 vrr", 280.0f, 1, 4u, RECOMP_FRAMEGEN_SLOT_TIMED, NULL);
    /* Without VRR those rates are refused, as they were. */
    checkPlan("144", 144.0f, 0, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is not a whole multiple of 60 Hz");
    checkPlan("165", 165.0f, 0, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is not a whole multiple of 60 Hz");
    checkPlan("200", 200.0f, 0, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is not a whole multiple of 60 Hz");
    checkPlan("280", 280.0f, 0, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is not a whole multiple of 60 Hz");
    /* 122 Hz has one slot below its ceiling, and everything below 120 Hz none. */
    checkPlan("122 vrr", 122.0f, 1, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh has no two slots below its maximum");
    checkPlan("122", 122.0f, 0, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is not a whole multiple of 60 Hz");
    checkPlan("100 vrr", 100.0f, 1, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh has no two slots below its maximum");
    checkPlan("60 vrr", 60.0f, 1, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is below 120 Hz");
    checkPlan("60", 60.0f, 0, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC,
        "the refresh is below 120 Hz");
    checkPlan("unknown", 0.0f, 1, 0u, RECOMP_FRAMEGEN_SLOT_VSYNC, "unknown refresh");
    return failures == 0 ? 0 : 1;
}
