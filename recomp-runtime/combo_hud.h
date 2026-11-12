/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_COMBO_HUD_H
#define DOA2U_RECOMP_COMBO_HUD_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The fight HUD's additions outside Sparring: a combo counter with the
   combo's damage under it, and Sparring's combo bar with its blinking
   critical-stun segment, on the attacking side; and PC Settings' life bar
   colour. */

/* The life bar colours PC Settings offers, in its row's order. */
enum RecompLifeBarColour {
    RECOMP_LIFE_BAR_DEFAULT,
    RECOMP_LIFE_BAR_RED,
    RECOMP_LIFE_BAR_GREEN,
    RECOMP_LIFE_BAR_BLUE,
    RECOMP_LIFE_BAR_PURPLE,
    RECOMP_LIFE_BAR_PINK,
    RECOMP_LIFE_BAR_COLOURS
};
/* launcher.json's names for them. */
static const char *const RECOMP_LIFE_BAR_NAMES[RECOMP_LIFE_BAR_COLOURS] = {
    "default", "red", "green", "blue", "purple", "pink",
};

/* Call at each boot. The counter and bar stay off online, where both
   players' guest RAM must match. */
void recomp_combo_hud_install(bool online);
void recomp_combo_hud_set_life_bar_colour(unsigned colour);
RecompFunction recomp_combo_hud_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
