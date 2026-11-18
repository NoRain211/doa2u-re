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
   player. 0xf192a8 points at one record per session slot. */
enum {
    SESSION_PLAYERS = 0x00f192a8u,
    SESSION_PLAYER_XUID = 0x00u,
    SESSION_PLAYER_SOCKADDR = 0x14u,
    SESSION_PLAYER_SOCKADDR_INA = 0x18u,
    SESSION_PLAYER_LOCAL = 0x2cu,
    ONLINE_USERS = 0x00f1cba8u, /* g_online_users: XONLINE_USER records, XUID at +0 */
    FIGHTER_SLOTS = 2u,
};

bool recomp_game_live_fight_pair(uint32_t *local, uint32_t *opponent)
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
