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

/* The life bar colours PC Settings offers, in its row's order. Custom takes
   launcher.json's lifeBarCustom, an RRGGBB hex colour. */
enum RecompLifeBarColour {
    RECOMP_LIFE_BAR_DEFAULT,
    RECOMP_LIFE_BAR_RED,
    RECOMP_LIFE_BAR_ORANGE,
    RECOMP_LIFE_BAR_YELLOW,
    RECOMP_LIFE_BAR_BLUE,
    RECOMP_LIFE_BAR_PURPLE,
    RECOMP_LIFE_BAR_PINK,
    RECOMP_LIFE_BAR_CUSTOM,
    RECOMP_LIFE_BAR_COLOURS
};
/* launcher.json's names for them. */
static const char *const RECOMP_LIFE_BAR_NAMES[RECOMP_LIFE_BAR_COLOURS] = {
    "default", "red", "orange", "yellow", "blue", "purple", "pink", "custom",
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
