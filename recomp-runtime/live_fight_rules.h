/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_FIGHT_RULES_H
#define DOA2U_RECOMP_LIVE_FIGHT_RULES_H

/* The decisions of a Live fight's rollback session that read no game state:
   the stop bit both sides share, the rollback player index, when the session
   may predict, which guest RAM its desync check keeps, and what a failed
   session or a departing player does. live_fight.c and netplay_session.cpp
   (RollbackMode::Live) use them; the fight test pins them without the game. */
#include "guest_snapshot.h"
#include "rollback.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A pad button bit no Xbox pad has. A side that asks its lockstep to stop
   rides the request on its next session pad; both sides see it and reset
   their lockstep on that frame. */
enum { RECOMP_LIVE_STOP_BUTTON = 0x8000u };

/* The rollback player index of a fight's two fighters: 0 for the one with the
   lower virtual address, 1 for the other. Both sides must agree, so the
   addresses (network order) are compared as numbers. */
unsigned recomp_live_player_index(uint32_t local_ina, uint32_t opponent_ina);

uint16_t recomp_live_pad_add_stop(uint16_t buttons);
uint16_t recomp_live_pad_mask_stop(uint16_t buttons);
/* True when either side's pad carries the stop request. */
bool recomp_live_pads_carry_stop(uint16_t local, uint16_t remote);

/* Frames the session may run past the other side's pads: lockstep (0) until
   the fight's lockstep batch has started (session frame 10) and while the
   round is not live. */
uint32_t recomp_live_window(bool round_live, uint32_t frame);

/* The ranges of guest RAM a Live fight's desync check ignores: everything
   outside the fight. Fills ignored with at most capacity ranges and returns
   how many, or 0 when they do not fit. */
size_t recomp_live_hash_ignored(RecompRamRange *ignored, size_t capacity);

/* Why a fight's rollback session ends before its stop frame: a desync, the
   other player's pads going quiet past the grace, a rewind that could not be
   loaded, or the other runner saying goodbye. */
typedef enum RecompLiveEnd {
    RECOMP_LIVE_END_DESYNC,
    RECOMP_LIVE_END_TIMEOUT,
    RECOMP_LIVE_END_REWIND,
    RECOMP_LIVE_END_PEER_LEFT,
} RecompLiveEnd;

/* The stop id netplay_session.cpp's endSession reports, so a Live end names
   the same failure as netplay's does. */
const char *recomp_live_end_stop_id(RecompLiveEnd end);

/* What an end does, supplied by netplay_session.cpp: tell the other player
   over the session's route, stop this runner as every other session does, or
   hand the fight back to the game's own Live code. */
typedef struct RecompLiveEndOps {
    void *context;
    void (*tell_opponent)(void *context);
    void (*stop_runner)(void *context, const char *stop_id);
    void (*hand_back)(void *context, const char *stop_id);
} RecompLiveEndOps;

/* Ends a fight's rollback session where it failed or the other player left
   (docs/xbox-live.md). A Live fight tells the other player when this side is the one
   leaving and hands the fight back to the game's own Live code and lockstep;
   every other session calls stop_runner, unchanged. True when the fight went
   back to the game, so the caller must return at once. */
bool recomp_live_end_session(bool live, RecompLiveEnd end, const RecompLiveEndOps *ops);

#ifdef __cplusplus
}
#endif

#endif
