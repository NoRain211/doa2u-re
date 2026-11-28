/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vs_cpu.h"
#include "kernel_abi.h"

#include <stdbool.h>
#include <stdio.h>

/* Versus against the CPU (issue #116). Retail Versus needs two controllers:
   the main menu greys it out with fewer (0x134570 sets mode 5's slot at
   0xdf5d70 by the pads connected), and the Normal/Tag screen's confirm
   (0x1389f0) refuses one pad in Versus.

   Sparring (frontend mode 6, game mode 9) already lets one pad pick both
   fighters, and its partner side has no human flag (0x87ae30, a side block
   every 0x94 bytes), so the fight runs that side on the CPU. With one
   controller and Normal chosen, Versus confirms as Sparring, so the
   character select and stage select are Sparring's, under Versus's banner
   (0xa3ee0 draws the frontend mode's name). When the frontend hands the match
   to the fight (0xd87c0 copies game mode 0x878392 to the battle's 0x87cfa0),
   game mode 5 goes back in, so the fight keeps Versus's rules, rounds and win
   tally. Versus's player flag (0x8783a8) is 1 there, as with two players:
   the hand-off sets 0 for one human, which makes the round's end run Story's
   ladder. With 1 the game leaves the CPU level (0x87d0d4) alone, so it is set
   here from the Options difficulty. After a fight CHARACTER enters the
   character select again (0x138e10) with Sparring's modes, AGAIN repeats the
   fight (see one_human_setup), and the main menu ends it. Tag needs two
   controllers still: Sparring's tag select picks for three fighters. */
enum {
    MENU_SLOTS = 0x00134570u,     /* fills the main menu's mode slots each frame */
    NORMAL_TAG = 0x001389f0u,     /* the Normal/Tag screen's confirm; returns the next scene */
    MATCH_SELECT = 0x00138e10u,   /* the character select scene's enter */
    HANDOFF = 0x000d87c0u,        /* the frontend's game mode update */
    BANNER = 0x000a3ee0u,         /* draws the mode's name at the top left */
    ONE_HUMAN_SETUP = 0x000c6690u, /* the fight setup's call while the player flag is 0 */

    VERSUS_SLOT = 0x00df5d84u,    /* u32, 2 when the mode can be chosen */
    SELECTED_MODE = 0x00df57b4u,  /* u32 frontend mode */
    TAG_CHOICE = 0x00df57bfu,     /* the Normal/Tag screen's cursor, 1 for Tag */
    GAME_MODE = 0x00878392u,
    BATTLE_MODE = 0x0087cfa0u,
    HANDOFF_PENDING = 0x00877ea8u, /* 1 on the frame the match starts */
    PLAYERS = 0x008783a8u,
    CPU_LEVEL = 0x0087d0d4u,      /* 0-7 */
    HUMAN = 0x0087ae30u,          /* +1 the character, +2 the costume */
    SIDE_STRIDE = 0x94u,
    PROFILE = 0x0087cfa8u,        /* pointer to the options; +0 the difficulty */
    STORY_LEVELS = 0x00692438u,   /* Story's CPU level per difficulty and bout */
    STORY_BOUTS = 11u,
    DIFFICULTIES = 4u,
    LEVEL_BOUT = 4u,              /* Story's fifth fight: 1, 5, 6 and 7 by difficulty */

    FRONTEND_VERSUS = 5u,
    FRONTEND_SPARRING = 6u,
    GAME_VERSUS = 5u,
    GAME_SPARRING = 9u,
    CHARACTER_SELECT = 3u,        /* the scene NORMAL_TAG returns on a confirm */
    SLOT_OPEN = 2u,
};

extern void sub_00134570(void);
extern void sub_001389F0(void);
extern void sub_00138E10(void);
extern void sub_000D87C0(void);
extern void sub_00127CE0(void);   /* controllers connected */
extern void sub_000A3EE0(void);
extern void sub_000C6690(void);

static bool active;
static uint8_t picks[2][2];      /* each side's character and costume at the hand-off */

static uint8_t *byte_at(uint32_t address) { return recomp_memory(address, 1u); }

static uint8_t *pick_at(uint32_t side, uint32_t i) { return byte_at(HUMAN + side * SIDE_STRIDE + 1u + i); }

static void menu_slots(void)
{
    sub_00134570();
    *recomp_memory_u32(VERSUS_SLOT) = SLOT_OPEN;
    active = false;
}

static void normal_tag(void)
{
    uint32_t *mode = recomp_memory_u32(SELECTED_MODE);
    /* Back from the character select: the screen is Versus's again. */
    if (active && *mode == FRONTEND_SPARRING) *mode = FRONTEND_VERSUS;
    active = false;
    const bool alone = *mode == FRONTEND_VERSUS && *byte_at(TAG_CHOICE) == 0u &&
        kernel_call_lifted(sub_00127CE0, NULL, 0u) < 2u;
    if (alone) *mode = FRONTEND_SPARRING;
    sub_001389F0();
    if (!alone) return;
    active = recomp_runtime.registers.eax == CHARACTER_SELECT;
    if (!active) *mode = FRONTEND_VERSUS;
    else fprintf(stderr, "recomp vs cpu: one controller, the CPU takes the other side\n");
}

static void match_select(void)
{
    if (active && *byte_at(GAME_MODE) == GAME_VERSUS) {
        *byte_at(GAME_MODE) = GAME_SPARRING;
        *recomp_memory_u32(SELECTED_MODE) = FRONTEND_SPARRING;
    }
    sub_00138E10();
}

static void banner(void)
{
    uint32_t *mode = recomp_memory_u32(SELECTED_MODE);
    const bool shown = active && *mode == FRONTEND_SPARRING;
    if (shown) *mode = FRONTEND_VERSUS;
    sub_000A3EE0();
    if (shown) *mode = FRONTEND_SPARRING;
}

static void handoff(void)
{
    const bool fight = active && *byte_at(HANDOFF_PENDING) == 1u && *byte_at(GAME_MODE) == GAME_SPARRING;
    if (fight) {
        *byte_at(GAME_MODE) = GAME_VERSUS;
        *recomp_memory_u32(SELECTED_MODE) = FRONTEND_VERSUS;
    }
    sub_000D87C0();
    if (!fight) return;
    *byte_at(PLAYERS) = 1u;
    for (uint32_t side = 0u; side < 2u; ++side)
        for (uint32_t i = 0u; i < 2u; ++i) picks[side][i] = *pick_at(side, i);
    const uint32_t profile = *recomp_memory_u32(PROFILE);
    uint8_t difficulty = profile != 0u ? *byte_at(profile) : 1u;
    if (difficulty >= DIFFICULTIES) difficulty = DIFFICULTIES - 1u;
    *byte_at(CPU_LEVEL) = *byte_at(STORY_LEVELS + difficulty * STORY_BOUTS + LEVEL_BOUT);
}

/* AGAIN after a fight: the menu leaves the player flag at 0, the CPU side
   has been given another fighter, and the fight's setup would pick the stage
   from Story's ladder. With Versus's 1 and the hand-off's picks the same
   fight starts again. */
static void one_human_setup(void)
{
    if (active && *byte_at(BATTLE_MODE) == GAME_VERSUS) {
        *byte_at(PLAYERS) = 1u;
        for (uint32_t side = 0u; side < 2u; ++side)
            for (uint32_t i = 0u; i < 2u; ++i) *pick_at(side, i) = picks[side][i];
        return;
    }
    sub_000C6690();
}

RecompFunction recomp_vs_cpu_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case MENU_SLOTS: return menu_slots;
    case NORMAL_TAG: return normal_tag;
    case MATCH_SELECT: return match_select;
    case HANDOFF: return handoff;
    case BANNER: return banner;
    case ONE_HUMAN_SETUP: return one_human_setup;
    default: return NULL;
    }
}
