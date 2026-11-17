/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_GAME_STATE_H
#define DOA2U_RECOMP_GAME_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DOA2U guest state a netplay session reads. */

/* A Live fight predicts only while a round is live: the game-mode task in
   its match state (g_game_mode_state, 0x8a56d0, is 5), the fight not frozen
   by the pause menu (g_game_tasks_suspended, 0x8783c9), time left
   (g_round_timer_seconds, 0x87ae08) and both fighters' life (0x87ae54,
   stride 0x94) above 0. A KO or time-over ends it seconds before the save
   at the end of a match, so menus and saves run in lockstep, which never
   rewinds. */
bool recomp_game_round_live(void);
/* A Live match's two fighters, from the session's slot table (g_session_players
   0xf192a8): slots 0 and 1 hold the fighters, each member's sockaddr at +0x14
   with sin_addr at +0x18, and the local member is the one whose XUID is the
   logged-on user's (g_online_users 0xf1cba8, +0) or that is marked at +0x2c.
   Writes their virtual addresses (network order) to local and opponent when
   not NULL. False when the table does not name two fighters or cannot tell
   the local one; the caller then keeps the game's own lockstep. */
bool recomp_game_live_fight_pair(uint32_t *local, uint32_t *opponent);

#ifdef __cplusplus
}
#endif

#endif
