/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_KO_REPLAY_H
#define DOA2U_RECOMP_KO_REPLAY_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Dynamic KO replay (PC Settings: KO Replay, Original / Dynamic): in the
   manner of Tekken Tag Tournament, a few frames after the KO hit lands the
   game stands still and DOA2U's own multi-angle replay (which it otherwise
   keeps for a human winner's few special moves) shows the hit three times,
   each from a camera of its own, the last at 85% speed; then the fall plays
   out live and the round moves on without the game's usual replay. Offline
   only. */
void recomp_ko_replay_install(bool online);
void recomp_ko_replay_set_enabled(bool enabled);
/* From hit_voice.c's 0x119420 hook: side's life just fell to 0. */
void recomp_ko_replay_note_ko(unsigned side);
/* Once a presented frame (the runner's swap), before the next guest frame. */
void recomp_ko_replay_frame(void);
/* The round's end (0xcf900), its frame waits (0xfa3d0), the replay pass
   step (0xc4950) and the replay task's end (0x1106c0, 0xcb2a0). */
RecompFunction recomp_ko_replay_lookup_manual(uint32_t guest_address);

/* The replay's timing, in frames of the fight (60 a second). */
typedef struct RecompKoReplayTiming {
    unsigned cut;           /* frames recorded past the KO when the replay starts (16) */
    unsigned before;        /* frames each earlier shot shows before the hit (8) */
    unsigned after;         /* and after it (13; at most cut) */
    unsigned last_before;   /* frames the last shot shows before the hit (10) */
    unsigned shots;         /* 1 to 3, the last running on to the newest frame (3) */
    float speed;            /* the earlier shots' speed, (1.0) */
    float last_speed;       /* the last shot's (85) */
} RecompKoReplayTiming;

void recomp_ko_replay_default_timing(RecompKoReplayTiming *timing);

/* The first move of a character's multi-angle list (0x692ef8), or -1. */
int recomp_ko_replay_listed_move(uint8_t character);

#ifdef __cplusplus
}
#endif

#endif
