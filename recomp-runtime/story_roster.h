/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_STORY_ROSTER_H
#define DOA2U_RECOMP_STORY_ROSTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Story mode's random opponents include Hitomi and Bayman once unlocked
   (story_roster.c). */
RecompFunction recomp_story_roster_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
