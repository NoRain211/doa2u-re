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

enum {
    GAME_MODE = 0x00878392u,
    FRONTEND_SCENE = 0x00df5dd8u,
    FRONTEND_SCENE_TABLE = 0x00df5e08u,
    /* Two page stacks share one layout: the pages at +0, the top's index at
       +0x96 (a signed byte; the 32 dwords end at +0x80) and a flag at +0x9c
       that live_page_apply_navigation sets while a page is up. The main one
       (frontend_online_select_enabled reads its flag), and the one
       live_pages_update draws over it (friends, voice and settings pages). */
    LIVE_PAGES = 0x00f25208u,
    LIVE_OVERLAY_PAGES = 0x00f25bc0u,
    LIVE_PAGE_INDEX = 0x96u,
    LIVE_PAGE_SHOWN = 0x9cu,
    LIVE_PAGE_STACK = 0x20u,
    SESSION_PLAYER_COUNT = 0x00f099ecu,
    MATCH_ROLE = 0x00f09a44u,
    LOCKSTEP_FLAGS = 0x00f0a548u,
};

static const char *const LIVE_PAGE_NAMES[] = {
    "account_select", "logon_status", "main_menu", "join_pending", "search_setup", "search_results",
    "join_setup", "friend_list", "friend_detail", "friend_voice_mail", "friend_feedback",
    "recent_player_list", "recent_player_detail", "recent_player_confirm", "recent_player_voice_mail",
    "recent_player_feedback", "voice_settings", "volume_settings", "ranking_select", "ranking_list",
    "player_slots", "session_lobby", "session_menu", "session_settings", "match_setup", "match_wait",
    "ranking_resume",
};

static int32_t live_top_page(uint32_t stack)
{
    const int8_t index = (int8_t)*recomp_memory(stack + LIVE_PAGE_INDEX, 1u);
    if (*recomp_memory(stack + LIVE_PAGE_SHOWN, 1u) == 0u || index < 0 || (uint32_t)index >= LIVE_PAGE_STACK)
        return -1;
    const int32_t page = (int32_t)*recomp_memory_u32(stack + 4u * (uint32_t)index);
    return page >= 0 && page < (int32_t)(sizeof LIVE_PAGE_NAMES / sizeof *LIVE_PAGE_NAMES) ? page : -1;
}

void recomp_game_live_screen(RecompGameLiveScreen *screen)
{
    screen->game_mode = *recomp_memory(GAME_MODE, 1u);
    screen->scene = *recomp_memory(FRONTEND_SCENE, 1u);
    screen->scene_table = *recomp_memory_u32(FRONTEND_SCENE_TABLE);
    screen->live_page = live_top_page(LIVE_PAGES);
    screen->live_overlay = live_top_page(LIVE_OVERLAY_PAGES);
    screen->session_players = *recomp_memory_u32(SESSION_PLAYER_COUNT);
    screen->match_role = *recomp_memory_u32(MATCH_ROLE);
    screen->lockstep_flags = *recomp_memory(LOCKSTEP_FLAGS, 1u);
}

const char *recomp_game_live_page_name(int32_t page)
{
    if (page < 0 || page >= (int32_t)(sizeof LIVE_PAGE_NAMES / sizeof *LIVE_PAGE_NAMES)) return "none";
    return LIVE_PAGE_NAMES[page];
}
