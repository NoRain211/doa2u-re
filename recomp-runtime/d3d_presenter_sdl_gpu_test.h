// SPDX-License-Identifier: GPL-3.0-or-later
/* The SDL_GPU presenter's test hooks. d3d_presenter_sdl_gpu.cpp includes this
   after its internals, whose shader table it uses; it sits in its own file to
   keep that one under public-export.json's file size limit. */

unsigned d3d_backend_draw_shader_count(RecompD3dPresenter *presenter)
{
    // Joining here leaves destroy nothing to join and no compile in flight.
    if (presenter->precompile.joinable()) presenter->precompile.join();
    std::lock_guard<std::mutex> lock(presenter->shader_lock);
    return static_cast<unsigned>(presenter->shaders.size());
}

void d3d_backend_join_precompile(RecompD3dPresenter *presenter)
{
    if (presenter->precompile.joinable()) presenter->precompile.join();
}

void d3d_backend_wait_upscales(RecompD3dPresenter *presenter)
{
    presenter->upscaler.wait();
}
