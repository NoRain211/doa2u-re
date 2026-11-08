/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_MENU_QUIT_H
#define DOA2U_RECOMP_MENU_QUIT_H

#include "input_model.h"
#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Quit Game, the main menu's last entry: its label, made from the menu's
   own lettering, an icon in the style of the mode icons, and a Yes/No
   popup before the runner quits. */

/* Call at each boot, after recomp_runner_assets_reset and before the game
   runs: reserves its guest memory and draws the icon. */
void recomp_menu_quit_install(void);
/* Host pads, before the game reads them: while the popup is open it takes
   them, and the game sees an idle pad. */
void recomp_menu_quit_filter(uint32_t port, RecompInputGamepad *pad);
bool recomp_menu_quit_open(void);
RecompFunction recomp_menu_quit_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
