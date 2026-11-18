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

   Hitomi joins the pool: her id goes where the end marker was and the marker
   moves into the alignment padding before the next list (0x692b94). The pool
   is guest data, so it is written again before each story setup. */
enum {
    STORY_SETUP = 0x000c6d40u,
    POOL = 0x00692b84u,
    POOL_END = 0x00692b90u,  /* the 0xff after the twelfth fighter */
    HITOMI = 0x15u,
};

extern void sub_000C6D40(void);

static void story_setup(void)
{
    uint8_t *end = recomp_memory(POOL_END, 4u);
    static const uint8_t ORIGINAL[4] = {0xffu, 0x00u, 0x00u, 0x00u};
    if (memcmp(end, ORIGINAL, sizeof ORIGINAL) == 0 && *recomp_memory(POOL, 1u) == 0x00u) {
        end[0] = HITOMI;
        end[1] = 0xffu;
    }
    sub_000C6D40();
}

RecompFunction recomp_story_roster_lookup_manual(uint32_t guest_address)
{
    return guest_address == STORY_SETUP ? story_setup : NULL;
}
