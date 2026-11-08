/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_UNLOCK_PROFILE_H
#define DOA2U_RECOMP_UNLOCK_PROFILE_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Unlock Everything offers every costume and the three hidden characters
   without saving them: the profile keeps what the player earned, so turning
   the setting off gives the profile back as it was, plus anything earned
   since. */

/* Forgets every earned record; call at each boot, before the first frame. */
void recomp_unlock_profile_reset(void);
/* Once a frame. On, the active profile is unlocked in memory, its earned
   values recorded first; off, each recorded profile gets its earned values
   back. */
void recomp_unlock_profile_frame(bool enabled);
/* Saves write, and awards change, the earned values; loads drop the records
   of the profiles they replace. */
RecompFunction recomp_unlock_profile_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
