/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_RUNNER_ASSETS_H
#define DOA2U_RECOMP_RUNNER_ASSETS_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runner text is UTF-16, written as u"" literals. C11 types those as
   uint_least16_t; macOS has no <uchar.h> to name that char16_t. */
typedef uint_least16_t recomp_utf16;

/* Sprites and message text the runner adds to the game. The game draws a
   sprite by id (group << 16 | index): 0xaa970 finds its 12-byte rectangle
   {left, top, right, bottom, u16 index, u16 group} and 0xaa9b0 its texture
   entry {texture, palette}. The game has 0x1a5 groups, which it loads and
   unloads by number, so the runner's are a group past them. */
enum { RECOMP_RUNNER_SPRITE_GROUP = 0x1f0, RECOMP_RUNNER_SPRITE_SLOTS = 8 };
enum {
    RECOMP_RUNNER_SPRITE_PC_WHITE = 0,  /* PC Settings' label and its glow */
    RECOMP_RUNNER_SPRITE_PC_GLOW = 1,
    RECOMP_RUNNER_SPRITE_QUIT_LABEL = 2, /* the main menu's Quit Game label and pictures */
    RECOMP_RUNNER_SPRITE_QUIT_PICTURE = 3,
    RECOMP_RUNNER_SPRITE_QUIT_WIDE = 4,
};

/* Forgets every sprite and text; each boot calls it before the installs. */
void recomp_runner_assets_reset(void);
/* A slot's rectangle and texture entry in guest memory; the rectangle's id
   words must hold the slot and RECOMP_RUNNER_SPRITE_GROUP. */
void recomp_runner_sprite_set(unsigned slot, uint32_t rectangle, uint32_t entry);
static inline uint32_t recomp_runner_sprite_id(unsigned slot)
{
    return (uint32_t)RECOMP_RUNNER_SPRITE_GROUP << 16 | slot;
}

/* Message numbers for 0x6a3e0 from RECOMP_RUNNER_TEXT_FIRST (past PC Settings'
   own 0x7000 range and every number the game uses), with a UTF-16 text in guest memory per game
   language (Japanese, English, French, German, Italian, Spanish). */
enum { RECOMP_RUNNER_TEXT_FIRST = 0x7100 };
void recomp_runner_text_set(uint32_t number, const uint32_t text[6]);
bool recomp_runner_text_lookup(uint32_t number, unsigned language, uint32_t *text);

/* 0xaa970 and 0xaa9b0. */
RecompFunction recomp_runner_assets_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
