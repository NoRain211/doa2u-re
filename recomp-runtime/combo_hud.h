/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_COMBO_HUD_H
#define DOA2U_RECOMP_COMBO_HUD_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fight HUD additions outside Sparring: the combo's damage under the game's
   own "N HIT COMBO !", Sparring's critical-stun segment over the life bars,
   and PC Settings' life bar colour. */

/* The life bar colours PC Settings offers, in its row's order: the game's own,
   16 to pick from, and Custom, launcher.json's lifeBarCustom (RRGGBB). */
enum {
    RECOMP_LIFE_BAR_DEFAULT = 0,
    RECOMP_LIFE_BAR_CUSTOM = 17,
    RECOMP_LIFE_BAR_COLOURS = 18,
};
/* launcher.json's names for them. */
static const char *const RECOMP_LIFE_BAR_NAMES[RECOMP_LIFE_BAR_COLOURS] = {
    "default", "red", "orange", "gold", "yellow", "lime", "green", "teal", "cyan",
    "sky", "blue", "indigo", "purple", "magenta", "pink", "white", "silver", "custom",
};
/* Their colours, 0xRRGGBB; 0 for Default and Custom. */
static const uint32_t RECOMP_LIFE_BAR_RGB[RECOMP_LIFE_BAR_COLOURS] = {
    0u, 0xff2a2au, 0xff8a1eu, 0xffc21eu, 0xfff23au, 0xa6ff2eu, 0x2ee05au, 0x1ec8aau, 0x2ee6ffu,
    0x6ab8ffu, 0x2e62ffu, 0x6a3cf0u, 0xa64cffu, 0xff3cf0u, 0xff7ac8u, 0xffffffu, 0xb4b8c8u, 0u,
};

/* Call at each boot. Each player's PC Settings decides the additions, online
   too: they change no fight state the Live check hashes and no guest timing. */
void recomp_combo_hud_install(void);
/* PC Settings' Combo HUD row: the damage line and the critical-stun segment
   (the Life Bar row keeps its colour either way). */
void recomp_combo_hud_set_enabled(bool on);
/* custom_rgb is 0xRRGGBB, used when colour is RECOMP_LIFE_BAR_CUSTOM. */
void recomp_combo_hud_set_life_bar_colour(unsigned colour, uint32_t custom_rgb);
/* PC Settings' Fight HUD: 0 Original (the base game's HUD), 1 the combo
   damage line only, 2 the critical-stun segment only, 3 both. */
enum { RECOMP_FIGHT_HUD_ORIGINAL = 0, RECOMP_FIGHT_HUD_DAMAGE = 1, RECOMP_FIGHT_HUD_STUN = 2, RECOMP_FIGHT_HUD_BOTH = 3 };
void recomp_combo_hud_set_choice(unsigned choice);
RecompFunction recomp_combo_hud_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
