/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_state.h"

#include "runtime.h"

#include <string.h>

bool recomp_game_round_live(void)
{
    if (*recomp_memory(0x008a56d0u, 1u) != 5u || *recomp_memory(0x008783c9u, 1u) != 0u ||
        *recomp_memory(0x0087ae08u, 1u) == 0u)
        return false;
    for (uint32_t side = 0u; side < 2u; ++side) {
        const uint8_t *life = recomp_memory(0x0087ae54u + side * 0x94u, 2u);
        if ((int16_t)(life[0] | life[1] << 8) <= 0) return false;
    }
    return true;
}

/* One session player's record (0xf0a5a0, 8 x 0x30): XUID at +0, the peer's
   sockaddr at +0x14 with sin_addr at +0x18, and +0x2c marks the local
   player. g_session_players (0xf192a8, 8 slots) points at one record per
   session slot, and g_session_player_count (0xf099ec) says how many. */
enum {
    SESSION_PLAYERS = 0x00f192a8u,
    SESSION_PLAYER_COUNT = 0x00f099ecu,
    SESSION_PLAYER_XUID = 0x00u,
    SESSION_PLAYER_SOCKADDR = 0x14u,
    SESSION_PLAYER_SOCKADDR_INA = 0x18u,
    SESSION_PLAYER_LOCAL = 0x2cu,
    SESSION_SLOTS = 8u,
    ONLINE_USERS = 0x00f1cba8u, /* g_online_users: XONLINE_USER records, XUID at +0 */
    LOCAL_XUID = 0x00f191f4u,   /* g_local_xuid, 8 bytes */
    /* The game's own lockstep participants, XUIDs refreshed from the session
       table whenever g_match_role (0xf09a44) changes, and bit 0 of the flags
       marking them usable (online_lockstep_update_flags, 0x29df60). */
    LOCKSTEP_FLAGS = 0x00f0a548u,   /* g_online_lockstep_flags */
    LOCKSTEP_PLAYER0 = 0x00f1a190u, /* g_lockstep_player0_id, 8 bytes */
    LOCKSTEP_PLAYER1 = 0x00f1a198u, /* g_lockstep_player1_id, 8 bytes */
    FIGHTER_SLOTS = 2u,
};

static uint64_t read_xuid(uint32_t address)
{
    uint64_t xuid;
    memcpy(&xuid, recomp_memory(address, sizeof xuid), sizeof xuid);
    return xuid;
}

/* The virtual address of the session member with this XUID, or 0. */
static uint32_t session_player_address(uint64_t xuid)
{
    uint32_t count = *recomp_memory_u32(SESSION_PLAYER_COUNT);
    if (count > SESSION_SLOTS) count = SESSION_SLOTS;
    for (uint32_t slot = 0u; slot < count; ++slot) {
        const uint32_t member = *recomp_memory_u32(SESSION_PLAYERS + 4u * slot);
        if (member == 0u || read_xuid(member + SESSION_PLAYER_XUID) != xuid) continue;
        const uint32_t ina = *recomp_memory_u32(member + SESSION_PLAYER_SOCKADDR_INA);
        if (ina != 0u) return ina;
    }
    return 0u;
}

/* The pair the game's own lockstep picked, which in a 3+ player session need
   not be slots 0 and 1. False when it cannot name both, or marks a spectator
   (neither participant is the local player) so the caller does not fall back
   to the slots. */
static bool lockstep_fight_pair(uint32_t *local, uint32_t *opponent, bool *spectator)
{
    if ((*recomp_memory(LOCKSTEP_FLAGS, 1u) & 1u) == 0u) return false;
    const uint64_t first = read_xuid(LOCKSTEP_PLAYER0);
    const uint64_t second = read_xuid(LOCKSTEP_PLAYER1);
    if (first == 0u || second == 0u) return false;
    const uint32_t first_ina = session_player_address(first);
    const uint32_t second_ina = session_player_address(second);
    /* Two fighters at one address cannot be ordered into two players: both
       sides would pick the same index and swap the pads. */
    if (first_ina == 0u || second_ina == 0u || first_ina == second_ina) return false;
    const uint64_t user = read_xuid(LOCAL_XUID);
    if (user == first) {
        if (local != NULL) *local = first_ina;
        if (opponent != NULL) *opponent = second_ina;
        return true;
    }
    if (user == second) {
        if (local != NULL) *local = second_ina;
        if (opponent != NULL) *opponent = first_ina;
        return true;
    }
    *spectator = true;
    return false;
}

/* The two-player layout: slots 0 and 1 hold the fighters. */
static bool slot_fight_pair(uint32_t *local, uint32_t *opponent)
{
    uint64_t user;
    memcpy(&user, recomp_memory(ONLINE_USERS, sizeof user), sizeof user);
    uint32_t address[FIGHTER_SLOTS] = {0u, 0u};
    bool mine[FIGHTER_SLOTS] = {false, false};
    unsigned fighters = 0u;
    for (uint32_t slot = 0u; slot < FIGHTER_SLOTS; ++slot) {
        const uint32_t member = *recomp_memory_u32(SESSION_PLAYERS + 4u * slot);
        if (member == 0u) continue;
        const uint32_t ina = *recomp_memory_u32(member + SESSION_PLAYER_SOCKADDR_INA);
        if (ina == 0u) continue;
        uint64_t xuid;
        memcpy(&xuid, recomp_memory(member + SESSION_PLAYER_XUID, sizeof xuid), sizeof xuid);
        address[fighters] = ina;
        /* Two marks of the local member: the logged-on XUID and the game's own
           byte at +0x2c. Exactly one fighter carries them while both are in
           the match; the other reads zero either way. */
        mine[fighters] = (user != 0u && xuid == user) || *recomp_memory(member + SESSION_PLAYER_LOCAL, 1u) != 0u;
        ++fighters;
    }
    if (fighters != FIGHTER_SLOTS || mine[0] == mine[1] || address[0] == address[1]) return false;
    const unsigned own = mine[0] ? 0u : 1u;
    if (local != NULL) *local = address[own];
    if (opponent != NULL) *opponent = address[1u - own];
    return true;
}

bool recomp_game_live_fight_pair(uint32_t *local, uint32_t *opponent)
{
    bool spectator = false;
    if (lockstep_fight_pair(local, opponent, &spectator)) return true;
    /* A spectator is not a fighter: pairing it with two others would roll back
       someone else's fight. */
    if (spectator) return false;
    /* Slots 0 and 1 are only the fighters while the session has two players;
       with more, the lockstep pair is the only safe answer. */
    if (*recomp_memory_u32(SESSION_PLAYER_COUNT) > FIGHTER_SLOTS) return false;
    return slot_fight_pair(local, opponent);
}
