/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_D3D8_ADAPTER_H
#define DOA2U_RECOMP_D3D8_ADAPTER_H

#include "runtime.h"
#include "d3d_presenter.h"

/* Verified 5849 library-static block holding the active fixed-function
   vertex declaration image. */
#define RECOMP_D3D_DEFAULT_DECLARATION 0x00357b80u

#ifdef __cplusplus
extern "C" {
#endif

RecompFunction recomp_d3d8_lookup_manual(uint32_t address);
uint32_t recomp_d3d8_require_device(void);
void recomp_d3d8_target(RecompD3dPresenterTarget *target);
void recomp_d3d8_submit(const RecompD3dPresenterCommand *command);
bool recomp_d3d8_draw_has_coverage(const RecompD3dPresenterDrawCommand *draw);
void recomp_d3d8_release_memory(uint32_t base, uint32_t size);
void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size);
void recomp_d3d8_shutdown(void);
/* Clears the main target, draws a BGRA card at level (0 black..255 full)
   and presents it. Call from recomp_d3d_present_hook, between guest frames. */
void recomp_d3d8_present_card(const uint32_t *pixels, uint32_t width, uint32_t height,
    float display_aspect, uint8_t level);
/* Optional host hook run once per Swap, after the frame counter updates. */
extern void (*recomp_d3d_present_hook)(uint32_t swap_counter);

#ifdef __cplusplus
}
#endif
#endif
