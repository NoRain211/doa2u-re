// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_TEXTURE_UPSCALE_H
#define DOA2U_RECOMP_TEXTURE_UPSCALE_H

#include <cstdint>
#include <vector>

/* The game's 2D art (menus, fonts, the HUD) is drawn at 480-line size and
   looks jagged when stretched to a modern display. The presenter redraws each
   such texture once, when first uploaded, with xBRZ (Zenju's edge-preserving
   "scale by rules" filter, GPL-3.0; fetched by the build, see NOTICE).

   The factor for a width x height texture: 4 up to 256 texels on a side, 2 up
   to 512, else 1 (no scaling). RECOMP_UI_UPSCALE=0 turns it off. */
uint32_t recomp_texture_upscale_factor(uint32_t width, uint32_t height);

/* Scales BGRA pixels (rows top down, alpha kept) by factor (2-6) into out,
   on worker threads. False if the factor or size is invalid. */
bool recomp_texture_upscale(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t factor,
    std::vector<uint8_t> &out);

#endif
