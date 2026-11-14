/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_RUNNER_ASSETS_H
#define DOA2U_RECOMP_RUNNER_ASSETS_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the runner adds to the game's menus: guest memory from its asset area
   (xbox_memory_layout.h), sprites and message text. Each boot calls
   recomp_runner_assets_reset, then the features install theirs. */
void recomp_runner_assets_reset(void);
/* Zeroed guest memory for this boot. The features' needs are fixed, so a
   full area stops the runner. */
uint32_t recomp_runner_alloc(uint32_t size, uint32_t alignment);

/* ---- Sprites ---------------------------------------------------------- */

/* The game draws a sprite by id (group << 16 | index): 0xaa970 finds its
   12-byte rectangle {left, top, right, bottom, u16 index, u16 group} and
   0xaa9b0 its texture entry {texture, palette}. The game has 0x1a5 groups,
   which it loads and unloads by number, so the runner's are a group past them. */
enum { RECOMP_RUNNER_SPRITE_GROUP = 0x1f0, RECOMP_RUNNER_SPRITE_SLOTS = 8 };
enum {
    RECOMP_RUNNER_SPRITE_PC_WHITE = 0,  /* PC Settings' label and its glow */
    RECOMP_RUNNER_SPRITE_PC_GLOW = 1,
    RECOMP_RUNNER_SPRITE_QUIT_LABEL = 2, /* the main menu's Quit Game label and pictures */
    RECOMP_RUNNER_SPRITE_QUIT_PICTURE = 3,
    RECOMP_RUNNER_SPRITE_QUIT_WIDE = 4,
};
static inline uint32_t recomp_runner_sprite_id(unsigned slot)
{
    return (uint32_t)RECOMP_RUNNER_SPRITE_GROUP << 16 | slot;
}
/* A palette header and its 256 ARGB colours, whose address goes to *colours. */
uint32_t recomp_runner_palette(uint32_t *colours);
/* A swizzled P8 sprite of 2^log_width x 2^log_height texels in a slot, drawn
   with a palette header. Its rectangle starts empty. */
void recomp_runner_sprite(unsigned slot, unsigned log_width, unsigned log_height, uint32_t palette);
void recomp_runner_sprite_size(unsigned slot, int width, int height);
void recomp_runner_sprite_palette(unsigned slot, uint32_t palette);
/* Writes row-major palette indices over a sprite's texels and tells the presenter. */
void recomp_runner_sprite_pixels(unsigned slot, const uint8_t *pixels);

/* One of the game's own P8 sprites, which the runner cuts letters from. */
typedef struct RecompGameSprite {
    uint32_t pixels, palette, colours; /* texels, palette header, its ARGB colours */
    uint32_t mask_x, mask_y;           /* the texture's swizzle */
    int left, top, right, bottom;
} RecompGameSprite;
/* False until the sprite's group is loaded, or unless it is P8 and height rows tall. */
bool recomp_game_sprite(uint32_t id, int height, RecompGameSprite *sprite);
/* The palette index of a texel, x and y within the sprite's rectangle. */
uint8_t recomp_game_sprite_index(const RecompGameSprite *sprite, int x, int y);
static inline uint32_t recomp_game_sprite_colour(const RecompGameSprite *sprite, uint8_t index)
{
    return *recomp_memory_u32(sprite->colours + 4u * index);
}

/* ---- Text ------------------------------------------------------------- */

/* Runner text is UTF-16, written as u"" literals. C11 types those as
   uint_least16_t; macOS has no <uchar.h> to name that char16_t. */
typedef uint_least16_t recomp_utf16;

/* Message numbers for 0x6a3e0, past every number the game uses. */
enum { RECOMP_RUNNER_TEXT_PC = 0x7000, RECOMP_RUNNER_TEXT_QUIT = 0x7200 };
/* A message's text per game language (Japanese, English, French, German,
   Italian, Spanish), copied into guest memory. */
void recomp_runner_text_set(uint32_t number, const recomp_utf16 *const text[6]);
bool recomp_runner_text_lookup(uint32_t number, unsigned language, uint32_t *text);
/* While to is not 0, the game's message from reads as message to. */
void recomp_runner_text_redirect(uint32_t from, uint32_t to);

/* 0x6a3e0 (message text), 0xaa970 and 0xaa9b0 (sprites). */
RecompFunction recomp_runner_assets_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
