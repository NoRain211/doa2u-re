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

#ifdef __cplusplus
}
#endif

#endif
