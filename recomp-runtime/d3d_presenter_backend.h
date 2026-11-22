#ifndef DOAXBV_RECOMP_D3D_PRESENTER_BACKEND_H
#define DOAXBV_RECOMP_D3D_PRESENTER_BACKEND_H

#include "d3d_presenter.h"

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
void d3d_backend_set_immediate_present(bool enabled);
/* recomp_d3d_presenter_set_quality, from any thread. */
void d3d_backend_set_quality(float scale, uint32_t msaa, bool smaa);
bool d3d_backend_quality_live();
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
/* SDL_GPU only, for tests: renders generated slot's picture and reads the
   center pixel of real frames N-1 and N and of it, as 0xAARRGGBB. Returns
   frame generation's k, or 0 without two real frames. */
unsigned d3d_backend_framegen_probe(RecompD3dPresenter *presenter, unsigned slot, uint32_t pixels[3]);

#endif
