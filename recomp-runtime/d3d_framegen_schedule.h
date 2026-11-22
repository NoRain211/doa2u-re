// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_D3D_FRAMEGEN_SCHEDULE_H
#define DOA2U_RECOMP_D3D_FRAMEGEN_SCHEDULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RECOMP_D3D_FRAMEGEN's window presents for one guest frame on a display of
   k * 60 Hz. Once real frame N arrives at present_ns, slot j (0 .. k-1) is
   due at present_ns + j / (60 k) s and shows the picture (j + 1) / k of the
   way from N-1 to N, so slot k-1 is N itself. */
typedef struct RecompFramegenDue {
    int slot;        /* the slot to show now, or -1 */
    unsigned missed; /* slots from next_slot on that were skipped as late */
} RecompFramegenDue;

/* next_slot is the first slot neither shown nor skipped. An in-between slot
   is late once half a slot has passed; the real slot stays due until the
   next real frame arrives. */
RecompFramegenDue recomp_framegen_due(unsigned k, int64_t present_ns, int64_t now_ns, unsigned next_slot);

/* Nanoseconds from now_ns until slot is due, 0 when it already is. */
int64_t recomp_framegen_until(unsigned k, int64_t present_ns, int64_t now_ns, unsigned slot);

/* Nanoseconds between two slots of a display of k * 60 Hz. */
int64_t recomp_framegen_slot_ns(unsigned k);

/* How the slots reach the display. */
typedef enum RecompFramegenSlotMode {
    RECOMP_FRAMEGEN_SLOT_VSYNC = 0, /* one refresh each: Present(1, ...) */
    RECOMP_FRAMEGEN_SLOT_TIMED = 1, /* 1 / (60 k) s each: Present(0, tearing), for VRR */
} RecompFramegenSlotMode;

/* The presents per guest frame frame generation runs with, 0 when it stays off. */
typedef struct RecompFramegenPlan {
    unsigned k;                /* present pictures per guest frame */
    RecompFramegenSlotMode mode;
    const char *reason;        /* why k is 0, for the log; NULL when it is not */
} RecompFramegenPlan;

/* RECOMP_D3D_FRAMEGEN on a display of refresh_hz. A whole multiple of 60 Hz
   implies VRR is too, so its slots stay on VSYNC. A VRR display of any other
   rate gets k = floor((refresh_hz - 3) / 60) slots, timed just below the
   panel's ceiling so VRR never reaches it. */
RecompFramegenPlan recomp_framegen_plan(float refresh_hz, int vrr);

#ifdef __cplusplus
}
#endif

#endif
