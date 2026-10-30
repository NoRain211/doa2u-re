#ifndef DOA2U_RECOMP_D3D_DRAW_SHADER_H
#define DOA2U_RECOMP_D3D_DRAW_SHADER_H

#include "d3d_draw_model.h"
#include "d3d_presenter.h"

#include <cstdint>
#include <string>

/* The draw shader and its constant buffer, shared by the Direct3D 11 and
   SDL_GPU presenters. Both compile the same HLSL: D3D11 with D3DCompile, the
   SDL_GPU presenter through SPIR-V (d3d_shader_compiler.h). */

/* WVP/blend transforms and draw flags (140 floats), vc[192], then lighting:
   normal transforms, material/base, directions/colors/flags, world transforms,
   point positions, attenuation/range and material-scaled ambient, then specular, view and stage (340 floats),
   then post-filter flags, texel size and four constants (24 floats), the
   stage-1 texture matrix (16), bump matrices, bump fog and fog color (16),
   the extra reflection world-view/normal transforms (96), and the stage-2
   texture matrix and flags (20), then scaled receiver depth bias (4), then the
   projected stage 1-3 matrices, count and slots (56). */
constexpr uint32_t kRecompD3dDrawConstantFloats = 140u + 192u * 4u + 512u + 4u + 56u;

/* Every FVF seen across all logged runs (14). Compiling them at boot keeps
   first use of each scene from stalling ~85 ms per shader. */
constexpr uint32_t kRecompD3dBootDrawFvfs[] = {
    0x042u, 0x104u, 0x112u, 0x116u, 0x118u, 0x11Au,
    0x142u, 0x144u, 0x212u, 0x216u, 0x21Au, 0x242u, 0x244u, 0x404u};

/* What selects one draw shader: its FVF, vertex program and variant. */
struct RecompD3dDrawShaderKey {
    uint32_t fvf;
    uint32_t program_count;
    const uint32_t (*program)[4];
    bool coverage_only;
    bool scaled_depth_bias;
    bool sample_depth_bias;
};

/* Builds the HLSL for one FVF, plus its vertex program when it has one.
   Entry points are vs_main and ps_main; an XYZ|DIFFUSE line list without a
   program also has gs_main, which widens and antialiases the lines. */
bool recomp_d3d_draw_shader_source(
    const RecompD3dDrawShaderKey &key, RecompD3dVertexLayout &layout, std::string &source);

/* The bound target when a draw is recorded. */
struct RecompD3dDrawTargetState {
    /* Viewport in host target pixels: x, y, width, height, min and max depth. */
    float viewport[6];
    /* Host pixels per guest pixel of the bound target. */
    float scale_x, scale_y;
};

/* Fills the draw shader's constant buffer (kRecompD3dDrawConstantFloats).
   Returns false when the draw's state cannot be expressed. */
bool recomp_d3d_draw_constants(
    const RecompD3dPresenterDrawCommand &draw, const RecompD3dVertexLayout &layout,
    const RecompD3dDrawTargetState &target, bool texture_bound, bool scaled_depth_bias,
    float *constants);

/* Whether a draw widens or antialiases an XYZ|DIFFUSE line list (gs_main). */
inline bool recomp_d3d_draw_geometry_line(const RecompD3dPresenterDrawCommand &draw)
{
    return draw.line_antialias || draw.line_width > 1.0f;
}

#endif
