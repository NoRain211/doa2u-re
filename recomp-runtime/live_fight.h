/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_FIGHT_H
#define DOA2U_RECOMP_LIVE_FIGHT_H

/* The game's own Xbox Live lockstep, replaced while a Live fight runs on
   rollback (netplay_session.cpp, RollbackMode::Live). A manual lookup
   (live_xapi.c, program_manual.c) asks for the guest addresses here before
   the program's own body; outside Live every address keeps its body. */
#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NULL for every address unless Live is on this boot
   (recomp_netplay_session_set_live). The hooks forward to the game's own
   bodies whenever no session runs. */
RecompFunction recomp_live_fight_lookup(uint32_t guest_address);

/* netplay_session.cpp: Live is on for this boot. Declared here because
   live_fight.c is C and netplay_session.h is C++. */
bool recomp_netplay_session_live(void);

#ifdef __cplusplus
}
#endif

#endif
