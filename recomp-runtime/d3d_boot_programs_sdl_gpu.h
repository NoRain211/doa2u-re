// SPDX-License-Identifier: GPL-3.0-or-later
/* The guest vertex programs' boot draw shaders for the SDL_GPU presenter.
   d3d_presenter_sdl_gpu.cpp includes this after its internals, whose
   lookupShaders and DrawOutput it uses; it sits in its own file to keep that
   one under public-export.json's file size limit. */

/* Builds each program's shader exactly as its first draw would, so the draw
   finds the pair cached. Depth bias is the same rule as the fixed list: only a
   scaled target draws with it, and with MSAA it is built for the sample count.
   A variant nobody logged (say a projected stage) still compiles on first
   use. scaled and samples are precompileDrawShaders' quality snapshot. */
unsigned precompileProgramShaders(RecompD3dPresenter *presenter, bool scaled, uint32_t samples)
{
    unsigned compiled = 0u;
    for (uint32_t i = 0u; i < presenter->config.program_count; ++i) {
        if (presenter->precompile_stop.load()) break;
        const RecompD3dBootProgram &program = presenter->config.programs[i];
        const bool bias = program.depth_bias && scaled;
        const RecompD3dDrawShaderKey key{program.fvf, program.count, program.tokens, false, bias,
            bias && samples > 1u, false};
        compiled += lookupShaders(presenter, key, kOutputShaded, samples).vertex != nullptr;
    }
    return compiled;
}

unsigned d3d_backend_draw_shader_count(RecompD3dPresenter *presenter)
{
    // Joining here leaves destroy nothing to join and no compile in flight.
    if (presenter->precompile.joinable()) presenter->precompile.join();
    std::lock_guard<std::mutex> lock(presenter->shader_lock);
    return static_cast<unsigned>(presenter->shaders.size());
}
