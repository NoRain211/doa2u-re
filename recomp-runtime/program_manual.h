/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_PROGRAM_MANUAL_H
#define DOA2U_RECOMP_PROGRAM_MANUAL_H

#include "runtime.h"

#include <stdbool.h>

/* --skip-intro: omit the logo movie's playback wait and end the boot opening movie. */
extern bool recomp_skip_intro;
void recomp_program_manual_reset(void);
/* Forgets resolved guest calls, for a reboot (program_adapters.c). */
void recomp_program_lookup_reset(void);
/* Netplay: the names shown above each life bar, local_side's in yellow; null or empty draws none. */
void recomp_program_set_fight_names(const char *side0, const char *side1, uint32_t local_side);
/* Netplay: each side's profile name (UTF-8), whose controller 1 settings
   that side's port takes when the game sets up its default profile; null or
   empty keeps the defaults. */
void recomp_program_set_side_profiles(const char *side0, const char *side1);

RecompFunction recomp_lookup_manual(uint32_t guest_address);

#endif
