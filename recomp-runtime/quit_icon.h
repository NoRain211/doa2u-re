/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_QUIT_ICON_H
#define DOA2U_RECOMP_QUIT_ICON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { QUIT_ICON_SIZE = 128 };

/* Draws the main menu's Quit Game picture: grey (0-255) and alpha per pixel,
   rows top down. squeeze narrows it about its centre (0.75 for the game's
   widescreen pictures, 1 for the others). */
void recomp_quit_icon_draw(float squeeze, uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE],
    uint8_t alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE]);
/* The same for the Jukebox's picture: a record and a quaver. */
void recomp_jukebox_icon_draw(float squeeze, uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE],
    uint8_t alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
