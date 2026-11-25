#ifndef DOAXBV_RECOMP_D3D_PRESENTER_BACKEND_H
#define DOAXBV_RECOMP_D3D_PRESENTER_BACKEND_H

#include "d3d_presenter.h"

#include <string>
#include <vector>

/* The Windows presenter worker (d3d_presenter_thread.cpp) or, on macOS and
   Linux, d3d_presenter_inline.cpp calls these. The backend is
   d3d_presenter_d3d11.cpp or, for Vulkan and on other hosts,
   d3d_presenter_sdl_gpu.cpp. */

RecompD3dPresenterError d3d_backend_create(
    const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter);
RecompD3dPresenterError d3d_backend_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command);
/* Counts native raster coverage without shading or target writes. Errors leave
   covered true; an open guest visibility query cannot include this probe. */
RecompD3dPresenterError d3d_backend_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered);
/* targets=false keeps render and depth targets and drops only cached textures. */
RecompD3dPresenterError d3d_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size, bool targets);
RecompD3dPresenterError d3d_backend_destroy(RecompD3dPresenter **presenter);
/* SDL_GPU only, for tests: waits for the boot precompile and returns the draw
   shaders it holds. */
unsigned d3d_backend_draw_shader_count(RecompD3dPresenter *presenter);
/* For tests: waits for the boot precompile to finish, where destroy would stop it. */
void d3d_backend_join_precompile(RecompD3dPresenter *presenter);
/* Compiles every boot shader this run may use into RECOMP_SHADER_CACHE, with no
   window and no GPU device: tools/build_game.py runs it after a build so the
   first launch finds them cached. Returns how many compiled. */
unsigned d3d_backend_precompile_boot_shaders(const RecompD3dPresenterConfig *config);
void d3d_backend_set_immediate_present(bool enabled);
/* True when the presenter must not pace PRESENT: the backend paces it itself
   (on a VRR display, or Direct3D 11 holding each frame 2 or 4 refreshes) or
   presents each frame at once, as immediate present asks. */
bool d3d_backend_paces_present(RecompD3dPresenter *presenter);
/* True when frame generation runs on this presenter. */
bool d3d_backend_framegen_on(RecompD3dPresenter *presenter);
/* Sends the frame's recorded GPU work before the presenter holds a PRESENT. */
void d3d_backend_flush(RecompD3dPresenter *presenter);
/* recomp_d3d_presenter_set_quality, from any thread. */
void d3d_backend_set_quality(float scale, uint32_t msaa, bool smaa, float sharpen, bool ao);
bool d3d_backend_quality_live();
/* recomp_d3d_presenter_set_display_mode, from any thread. */
void d3d_backend_set_display_mode(unsigned mode);
void d3d_backend_report_draw_textures();

enum D3dBackendVisibilityPoll {
    D3D_BACKEND_VISIBILITY_IDLE,
    D3D_BACKEND_VISIBILITY_PENDING,
    D3D_BACKEND_VISIBILITY_READY,
    D3D_BACKEND_VISIBILITY_FAILED,
};
/* Resolves the oldest ended visibility query without waiting; READY fills
   its index and count. Queries resolve in submission order. */
D3dBackendVisibilityPoll d3d_backend_poll_visibility(
    RecompD3dPresenter *presenter, uint32_t *index, uint32_t *pixels);
/* Inline backends only: true when the next swapchain acquire would not wait
   for the GPU to finish an earlier frame. Not a pure query: its only caller
   is Swap's wait, just before that frame's PRESENT, and with VRR the SDL_GPU
   backend queues the frame's draws here so they render during the wait. */
bool d3d_backend_present_ready(RecompD3dPresenter *presenter);
/* Windows worker only: presents frame generation's due slot, if any. The
   worker calls it between records and while it waits. Returns the 100 ns
   ticks until the next slot is due, or UINT64_MAX. */
uint64_t d3d_backend_idle(RecompD3dPresenter *presenter);
/* For tests: renders generated slot's picture and reads the center pixel of
   real frames N-1 and N and of it, as 0xAARRGGBB. Returns frame generation's
   k, or 0 without two real frames. */
unsigned d3d_backend_framegen_probe(RecompD3dPresenter *presenter, unsigned slot, uint32_t pixels[3]);
/* For tests: runs frame generation's flow passes repeat more times on the
   history, waits for them, and reads the forward then the backward field
   (4 floats per block, see framegenFlow) into fields when capacity holds both.
   size gets the blocks per row and column. Returns the wait for the GPU in
   milliseconds, or a negative value without a field. */
double d3d_backend_framegen_flow(RecompD3dPresenter *presenter, unsigned repeat, unsigned size[2], float *fields,
    size_t capacity);
/* For tests: generates the picture t of the way from real frame N-1 to N
   repeat times, waits for it, and reads it as 0xAARRGGBB into pixels when
   capacity holds the main target. size gets its width and height. Returns the
   wait for the GPU in milliseconds, or a negative value without two real
   frames. */
double d3d_backend_framegen_generate(RecompD3dPresenter *presenter, float t, unsigned repeat, unsigned size[2],
    uint32_t *pixels, size_t capacity);
/* For tests: draws frame generation's HUD mask from the 2D layer draws queued
   since the last PRESENT repeat times, after submitting the draws before
   them, and waits for it; the draws stay queued. draws gets their count.
   Returns the wait for the GPU in milliseconds, or a negative value without a
   mask. */
double d3d_backend_framegen_cover(RecompD3dPresenter *presenter, unsigned repeat, unsigned *draws);
/* For d3d_framegen_replay.cpp and tests: makes picture (the main target's
   size, 0xAARRGGBB before gamma) real frame N and hud (one byte per pixel) its
   HUD mask, as a PRESENT under frame generation does; restart starts the
   history again from N. False without the history. */
bool d3d_backend_framegen_push(RecompD3dPresenter *presenter, const uint32_t *picture, const uint8_t *hud, bool restart);
/* For d3d_framegen_replay.cpp: frame generation crossfades from now on, as
   without its flow. */
void d3d_backend_framegen_crossfade(RecompD3dPresenter *presenter);
/* For tests: a real frame recorded more than late_ns after the one before
   starts frame generation's history again. 25 ms by default on SDL_GPU,
   40 ms on Direct3D 11; tests that push frames unpaced widen it. */
void d3d_backend_framegen_set_late_ns(int64_t late_ns);

/* SDL_GPU backend only: SMAA's three passes' HLSL and its defines for a frame
   of this size, so the presenter and the DXIL test compile the same source. */
void d3d_backend_smaa_source(uint32_t width, uint32_t height, std::string &source,
    std::vector<std::string> &defines);

#endif
