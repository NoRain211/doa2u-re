/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_MENU_QUIT_H
#define DOA2U_RECOMP_MENU_QUIT_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Quit Game, the main menu's last entry: its label, made from the menu's
   own lettering, and an icon in the style of the mode icons. Choosing it
   opens the runner's quit prompt (quit_prompt.c). */

/* Call at each boot, after recomp_runner_assets_reset and before the game
   runs: takes its guest memory and draws the icon. */
void recomp_menu_quit_install(void);
RecompFunction recomp_menu_quit_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
