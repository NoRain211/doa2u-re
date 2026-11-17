/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_fight_rules.h"

#include "runtime.h" /* RECOMP_XBOX_RAM_SIZE */

/* A virtual address is 10.x.y.z in network order. Reading it as a host number
   would order the octets backwards on a little-endian host, so compare the
   stored bytes as the number they spell. */
static uint32_t address_number(uint32_t ina)
{
    return (ina >> 24) | ((ina >> 8) & 0x0000ff00u) | ((ina << 8) & 0x00ff0000u) | (ina << 24);
}

unsigned recomp_live_player_index(uint32_t local_ina, uint32_t opponent_ina)
{
    return address_number(local_ina) < address_number(opponent_ina) ? 0u : 1u;
}

uint16_t recomp_live_pad_add_stop(uint16_t buttons)
{
    return (uint16_t)(buttons | RECOMP_LIVE_STOP_BUTTON);
}

uint16_t recomp_live_pad_mask_stop(uint16_t buttons)
{
    return (uint16_t)(buttons & (uint16_t)~RECOMP_LIVE_STOP_BUTTON);
}

bool recomp_live_pads_carry_stop(uint16_t local, uint16_t remote)
{
    return ((local | remote) & RECOMP_LIVE_STOP_BUTTON) != 0u;
}

uint32_t recomp_live_window(bool round_live, uint32_t frame)
{
    /* The fight holds still until lockstep batch 10 (game_mode_match_update),
       and a rematch saves meanwhile. */
    return round_live && frame >= 10u ? RECOMP_ROLLBACK_WINDOW : 0u;
}

size_t recomp_live_hash_ignored(RecompRamRange *ignored, size_t capacity)
{
    /* Each side's Live layer, profile, HUD and draw state differ by design, as
       do the cloth and stage effects keyed to frames since boot, so hash only
       the fight: the match and fighter block, the battle state and the game's
       Mersenne Twister (0x8a5768, 625 words). */
    static const RecompRamRange kept[] = {
        {0x00863a8cu, 0x0087d56cu}, /* up to g_game_mode_frame_counter, frames since boot */
        {0x0087d570u, 0x0087f53cu},
        {0x008a3968u, 0x008a396cu}, /* g_battle_state */
        {0x008a5768u, 0x008a612cu},
    };
    const size_t kept_count = sizeof kept / sizeof *kept;
    if (ignored == NULL || capacity < kept_count + 1u) return 0u;
    size_t count = 0u, from = 0u;
    for (size_t i = 0u; i < kept_count; ++i) {
        if (kept[i].start > from) ignored[count++] = (RecompRamRange){from, kept[i].start};
        from = kept[i].stop;
    }
    if (from < RECOMP_XBOX_RAM_SIZE) ignored[count++] = (RecompRamRange){from, RECOMP_XBOX_RAM_SIZE};
    return count;
}

const char *recomp_live_end_stop_id(RecompLiveEnd end)
{
    switch (end) {
    case RECOMP_LIVE_END_DESYNC: return "rollback:desync";
    case RECOMP_LIVE_END_TIMEOUT: return "rollback:peer-timeout";
    case RECOMP_LIVE_END_REWIND: return "rollback:unrestorable";
    case RECOMP_LIVE_END_PEER_LEFT: return "rollback:peer-left";
    }
    return "rollback:end";
}

bool recomp_live_end_session(bool live, RecompLiveEnd end, const RecompLiveEndOps *ops)
{
    if (!live) {
        /* Every other session keeps its own ending: the runner stops, or a room
           returns to its menu, exactly as before. */
        if (ops != NULL && ops->stop_runner != NULL) ops->stop_runner(ops->context, recomp_live_end_stop_id(end));
        return false;
    }
    /* Live: the game's own Live messages and lockstep carry the fight from
       here, so nothing stops the runner. Tell the other player first, while the
       session's route is still bound, unless they are the one who left. */
    if (end != RECOMP_LIVE_END_PEER_LEFT && ops != NULL && ops->tell_opponent != NULL)
        ops->tell_opponent(ops->context);
    if (ops != NULL && ops->hand_back != NULL) ops->hand_back(ops->context, recomp_live_end_stop_id(end));
    return true;
}
