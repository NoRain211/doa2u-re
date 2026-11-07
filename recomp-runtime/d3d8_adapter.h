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
/* Width over height the game draws its frames for: 4:3, or the widescreen shape. */
float recomp_d3d8_content_aspect(void);
/* Fits an on-screen pretransformed draw of the game's 16:9 2D layer to
   another widescreen shape (widescreen.h), scissor rectangle included. */
void recomp_d3d8_fit_2d_layer(RecompD3dPresenterDrawCommand *draw);
void recomp_d3d8_fit_3d_viewport(RecompD3dPresenterDrawCommand *draw);
void recomp_d3d8_release_memory(uint32_t base, uint32_t size);
void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size);
void recomp_d3d8_shutdown(void);
/* Shutdown for a reboot: the window stays, and the next CreateDevice reuses it. */
void recomp_d3d8_reset_keeping_window(void);
/* Hidden run-ahead frames keep their device model but skip the flip. */
void recomp_d3d8_set_present_suppressed(bool suppressed);
/* Repeated run-ahead frames also skip draws, clears, copies and visibility tests. */
void recomp_d3d8_set_gpu_work_suppressed(bool suppressed);
/* Clears the main target, draws a BGRA card at level (0 black..255 full)
   and presents it. Call from recomp_d3d_present_hook, between guest frames. */
void recomp_d3d8_present_card(const uint32_t *pixels, uint32_t width, uint32_t height,
    float display_aspect, uint8_t level);
/* Presents the last shown frame again with draw's shapes over it, for a
   wait between guest frames; shapes drawn opaque look the same each time. */
void recomp_d3d8_present_overlay(void (*draw)(const RecompD3dPresenterTarget *target));
/* Swaps presented so far (the guest's frame counter). */
uint32_t recomp_d3d8_swap_counter(void);
/* Optional host hook run once per Swap, after the frame counter updates. */
extern void (*recomp_d3d_present_hook)(uint32_t swap_counter);
/* Optional host hook that draws over a shown frame, into target, just
   before its Present. */
extern void (*recomp_d3d_overlay_hook)(const RecompD3dPresenterTarget *target);
/* Optional host hook at the frame boundary: guest state is final and the
   frame-locked clock has already advanced. SyncTest snapshots here. */
extern void (*recomp_d3d_frame_boundary_hook)(uint32_t swap_counter);
/* Host device and frame models that the guest's D3D8 object mirrors. */
size_t recomp_d3d8_state_size(void);
/* Guest return address of the last Present. */
uint32_t recomp_d3d8_swap_caller(void);
void recomp_d3d8_state_save(void *state);
void recomp_d3d8_state_restore(const void *state);

#ifdef __cplusplus
}
#endif
#endif
