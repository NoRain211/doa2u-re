// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_D3D_BOOT_SHADERS_H
#define DOA2U_RECOMP_D3D_BOOT_SHADERS_H

#include "d3d_draw_shader.h"
#include "d3d_presenter.h"

#include <cstdint>

/* Every shader a presenter builds before its first draw: the fixed FVF list
   with the variants the settings select, then the guest vertex programs the
   D3D8 adapter copied out of the executable. The boot precompile thread and
   the build's precompile-only pass both walk this, so their keys match and the
   cache one fills the other hits. build returns false to stop early. */
template <typename Build>
bool recomp_d3d_for_each_boot_shader(const RecompD3dPresenterConfig *config, bool scaled,
    uint32_t samples, bool framegen, Build &&build)
{
    for (const RecompD3dBootDrawShader &boot : kRecompD3dBootDrawShaders) {
        /* Only a scaled target draws the receiver depth bias, and with MSAA the
           shader is built for the sample count. Output 1 counts occlusion; 2 to
           4 shade frame generation's HUD mask and exist only with it. */
        if (boot.depth_bias && !scaled) continue;
        if (boot.output > 1u && !framegen) continue;
        const RecompD3dDrawShaderKey key{boot.fvf, 0u, nullptr, false, boot.depth_bias,
            boot.depth_bias && samples > 1u, boot.projected_stages};
        if (!build(key, boot.output)) return false;
    }
    for (uint32_t i = 0u; i < config->program_count; ++i) {
        const RecompD3dBootProgram &program = config->programs[i];
        const bool bias = program.depth_bias && scaled;
        const RecompD3dDrawShaderKey key{program.fvf, program.count, program.tokens, false, bias,
            bias && samples > 1u, false};
        if (!build(key, 0u)) return false;
    }
    return true;
}

#endif
