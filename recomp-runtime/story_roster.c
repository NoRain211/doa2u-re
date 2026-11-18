/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "story_roster.h"

#include <string.h>

/* Story mode gives each fighter seven bouts (0x692658: a 0x54-byte record
   per story slot, 0x6959c0 mapping character to slot; each bout 12 bytes,
   +5 the opponent, +6 the stage). Bouts whose opponent is 0xff are drawn by
   0xc4610 from the pool at 0x692b84, a 0xff-ended list of character ids that
   leaves out Tengu (0x11), Hitomi (0x15) and Bayman (0x18). Only the story
   setup, 0xc6d40, reads it (as 0x8a3994; the player's own fighter is taken
   out first by 0xc4510), and its used-flags (0x8a3948) hold 76 entries.

   Hitomi and Bayman join the pool once unlocked: the game's other random
   pool (0xc46f7) leaves each out until its flag in the game data (0x87cfa8)
   is 1, Hitomi's at +0x2ed4 (0x21430) and Bayman's at +0x2ed3 (0x21420).
   Their ids go where the end marker was and the marker moves into the
   alignment padding before the next list (0x692b94). The pool is guest data,
   so it is written again before each story setup. */
enum {
    STORY_SETUP = 0x000c6d40u,
    POOL = 0x00692b84u,
    POOL_END = 0x00692b90u, /* the 0xff after the twelfth fighter */
    GAME_DATA = 0x0087cfa8u,
    HITOMI_UNLOCKED = 0x2ed4u,
    BAYMAN_UNLOCKED = 0x2ed3u,
    HITOMI = 0x15u,
    BAYMAN = 0x18u,
};

extern void sub_000C6D40(void);

/* The disc's end or one written here: Hitomi, then Bayman, each optional,
   the marker, then zero padding. */
static bool known_end(const uint8_t end[4])
{
    unsigned i = 0u;
    if (end[i] == HITOMI) ++i;
    if (end[i] == BAYMAN) ++i;
    if (end[i++] != 0xffu) return false;
    while (i < 4u)
        if (end[i++] != 0u) return false;
    return true;
}

static bool unlocked(uint32_t data, uint32_t flag)
{
    return data != 0u && *recomp_memory(data + flag, 1u) == 1u;
}

static void story_setup(void)
{
    uint8_t *end = recomp_memory(POOL_END, 4u);
    if (known_end(end) && *recomp_memory(POOL, 1u) == 0x00u) {
        const uint32_t data = *recomp_memory_u32(GAME_DATA);
        uint8_t next[4] = {0u};
        unsigned count = 0u;
        if (unlocked(data, HITOMI_UNLOCKED)) next[count++] = HITOMI;
        if (unlocked(data, BAYMAN_UNLOCKED)) next[count++] = BAYMAN;
        next[count] = 0xffu;
        memcpy(end, next, sizeof next);
    }
    sub_000C6D40();
}

RecompFunction recomp_story_roster_lookup_manual(uint32_t guest_address)
{
    return guest_address == STORY_SETUP ? story_setup : NULL;
}
