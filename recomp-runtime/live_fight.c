/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_fight.h"

#include "game_state.h"
#include "kernel_abi.h"
#include "live_session.h"
#include "runtime.h"

#include <stdio.h>
#include <string.h>

/* A Live fight runs on rollback instead of the game's delay-based lockstep
   (docs/rollback-netcode.md, docs/xbox-live.md). Three guest functions carry it:

   - online_lockstep_frame 0x298930 (cdecl, 1 argument) hands the fight its
     pads from the lockstep ring. In a session the session's two pads are
     copied into the virtual pad slots instead, so both sides simulate the
     same inputs, and the batch counter advances one per frame, which is what
     the fight reseeds its Mersenne Twister from.
   - battle_online_round_start 0xd1000 (cdecl, returns 1 when aborted) runs
     once per match before its first round and waits for the peer's ready
     handshake. Letting it run keeps the game's session messages; the session
     starts on the first frame boundary after it returns, so both sides begin
     from the released state.
     The title and menu demo scenes (their manager at 0x87f534) run real
     fights with up to four fighters and leave their poses in
     g_fighter_bone_matrices; a fight writes only the bones its own models
     have, so each side would start with what its last demo scene left. The
     four banks are zeroed here, before the session's first save. (The match
     load, battle_start_fight 0xce050, would be the natural place, but the
     program reaches it by a direct tail call that no lookup sees.)
   - online_lockstep_request_stop 0x293980 (cdecl) asks the game's lockstep to
     stop at the match end. In a session the request rides this side's next
     session pad and both sides reset the lockstep on that frame, so the stop
     lands on the same frame on both sides.

   The pads' guest state: g_pad_state 0xa94a70 holds
   0x30-byte pads, slot 5 the local and slot 6 the remote fighter. */

enum {
    PAD_STATE = 0x00a94a70u,        /* g_pad_state */
    PAD_BYTES = 0x30u,
    LOCAL_PAD_SLOT = 5u,
    REMOTE_PAD_SLOT = 6u,
    LOCKSTEP_STALLED = 0x00f0a540u, /* g_lockstep_stalled */
    LOCKSTEP_BATCH = 0x00f0a560u,   /* g_lockstep_batch */
    LOCKSTEP_RESET = 0x002936a0u,   /* online_lockstep_reset */
    FIGHTER_BONE_MATRICES = 0x00875c20u, /* g_fighter_bone_matrices: 4 fighters, 32 bones */
    FIGHTER_BONE_BYTES = 4u * 0x800u,
};

/* The program's own bodies, which every hook falls back to. */
extern void sub_00298930(void); /* online_lockstep_frame */
extern void sub_000D1000(void); /* battle_online_round_start */
extern void sub_00293980(void); /* online_lockstep_request_stop */

/* Both sides, at the start of the session frame whose pads carry the stop. */
void recomp_live_stop_lockstep(void)
{
    kernel_call_guest(LOCKSTEP_RESET, NULL, 0u);
}

static void live_lockstep_frame(void)
{
    if (!recomp_live_rollback_active()) {
        sub_00298930();
        return;
    }
    const unsigned local = recomp_live_rollback_player() & 1u;
    memcpy(recomp_memory(PAD_STATE + LOCAL_PAD_SLOT * PAD_BYTES, PAD_BYTES),
        recomp_memory(PAD_STATE + local * PAD_BYTES, PAD_BYTES), PAD_BYTES);
    memcpy(recomp_memory(PAD_STATE + REMOTE_PAD_SLOT * PAD_BYTES, PAD_BYTES),
        recomp_memory(PAD_STATE + (1u - local) * PAD_BYTES, PAD_BYTES), PAD_BYTES);
    *recomp_memory_u32(LOCKSTEP_STALLED) = 0u;
    ++*recomp_memory_u32(LOCKSTEP_BATCH);
    kernel_return_caller_cleanup(0u);
}

static void live_round_start(void)
{
    uint32_t local = 0u, opponent = 0u;
    sub_000D1000();
    /* Aborted (the peer left or the handshake failed): there is no fight to
       roll back, so leave the game to its own session messages. */
    if (recomp_runtime.registers.eax != 0u) return;
    memset(recomp_memory(FIGHTER_BONE_MATRICES, FIGHTER_BONE_BYTES), 0, FIGHTER_BONE_BYTES);
    if (recomp_game_live_fight_pair(&local, &opponent)) recomp_live_round_started(opponent);
    else
        fprintf(stderr, "recomp live: the session names no lockstep fighter pair; "
            "the game keeps its own lockstep\n");
}

static void live_request_stop(void)
{
    if (!recomp_live_rollback_active()) {
        sub_00293980();
        return;
    }
    recomp_live_request_stop();
    kernel_return_caller_cleanup(0u);
}

RecompFunction recomp_live_fight_lookup(uint32_t guest_address)
{
    if (!recomp_netplay_session_live()) return NULL;
    switch (guest_address) {
    case 0x00298930u: return live_lockstep_frame;
    case 0x000d1000u: return live_round_start;
    case 0x00293980u: return live_request_stop;
    default: return NULL;
    }
}
