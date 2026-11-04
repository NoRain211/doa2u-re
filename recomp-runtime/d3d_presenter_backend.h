#ifndef DOAXBV_RECOMP_D3D_PRESENTER_BACKEND_H
#define DOAXBV_RECOMP_D3D_PRESENTER_BACKEND_H

#include "d3d_presenter.h"

/* The presenter worker (d3d_presenter_thread.cpp) or, on hosts whose window
   must stay on its own thread, d3d_presenter_inline.cpp calls these. The
   backend is d3d_presenter_d3d11.cpp on Windows and d3d_presenter_sdl_gpu.cpp
   elsewhere. */

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
   for the GPU to finish an earlier frame. */
bool d3d_backend_present_ready(RecompD3dPresenter *presenter);

#endif
