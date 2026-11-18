/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_SESSION_H
#define DOA2U_RECOMP_LIVE_SESSION_H

/* The rollback session of a Live fight (netplay_session.cpp, RollbackMode::Live)
   and the fight hooks that drive it (live_fight.c). */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read by live_transport.c. */
bool recomp_live_rollback_active(void);
bool recomp_live_rollback_hidden(void); /* a frame run again after a rewind */
uint32_t recomp_live_rollback_frame(void);
unsigned recomp_live_rollback_player(void);

/* live_fight.c -> netplay_session.cpp */
/* battle_online_round_start returned: start the session at the next frame boundary. */
void recomp_live_round_started(uint32_t opponent_ina);
/* The game asked to stop its lockstep (online_lockstep_request_stop): the
   request rides this side's next session pad. */
void recomp_live_request_stop(void);

/* netplay_session.cpp -> live_fight.c, at the start of the frame whose pads carry the stop. */
void recomp_live_stop_lockstep(void);

#ifdef __cplusplus
}
#endif

#endif
