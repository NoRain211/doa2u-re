// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_PLAYSTATION_BUTTONS_H
#define DOA2U_RECOMP_PLAYSTATION_BUTTONS_H

#include "texture_pack.h"

#include <cstdint>
#include <string>

/* The game's menus draw their Xbox button prompts from 64x32 P8 textures: one
   for A and B, one for X and Y, and one for L and R. While a PlayStation pad
   drives the game, original Cross, Circle, Square, Triangle and L1/R1 art
   replaces them.

   Those textures' pack keys hash retail bytes, so they are not in this source.
   Run with RECOMP_TEXTURE_DUMP=png (or the launcher's Texture Dump row), open
   the dumped 64x32 button icons, and list their keys beside the launcher in
   textures/playstation-buttons.txt:

       # <16 hex digits> = <glyph>[,<glyph>...]
       0123456789abcdef = cross,circle

   A line's glyphs split its texture into equal cells from left to right. The
   names are cross, circle, square, triangle, l1, r1, l2 and r2. The file is
   read once at start; without it the game keeps its own icons. A texture pack's
   replacement for the same key wins. */
void recomp_playstation_buttons_configure(const std::string &textures_folder);

/* Set from the game thread as input is sampled; the presenters read it while
   they upload textures. */
void recomp_playstation_buttons_set_active(bool active);
bool recomp_playstation_buttons_active(void);
/* True when a PlayStation pad is active and the private mapping has at least
   one key: the presenters then hash textures to look for replacements. */
bool recomp_playstation_buttons_replacing(void);

/* The replacement art for a guest texture key, drawn at the guest size, when a
   PlayStation pad is active and the private mapping names the key; false
   otherwise, leaving out untouched. */
bool recomp_playstation_buttons_image(uint64_t key, uint32_t width, uint32_t height,
    RecompTexturePackImage &out);

#endif
