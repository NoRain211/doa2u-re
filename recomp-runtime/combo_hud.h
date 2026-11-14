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

/* Call at each boot. Online every addition stays off and the life bar keeps
   its own colour, so both players' guest RAM matches. */
void recomp_combo_hud_install(bool online);
/* custom_rgb is 0xRRGGBB, used when colour is RECOMP_LIFE_BAR_CUSTOM. */
void recomp_combo_hud_set_life_bar_colour(unsigned colour, uint32_t custom_rgb);
RecompFunction recomp_combo_hud_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
