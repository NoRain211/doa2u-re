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
   projected stage 1-3 matrices, count and slots (56), then table fog (8),
   then the 2D texcoord clamp (4, last: recomp_d3d_draw_uv_clamp). */
constexpr uint32_t kRecompD3dDrawConstantFloats = 140u + 192u * 4u + 512u + 4u + 56u + 8u + 4u;
constexpr uint32_t kRecompD3dDrawUvClamp = kRecompD3dDrawConstantFloats - 4u;

/* One fixed-function shader compiled at boot. output is the SDL_GPU presenter's
   DrawOutput: 0 shaded, 1 an occlusion count, above that frame generation's
   HUD cover (2 alpha, 3 added, 4 added by alpha). depth_bias is the scaled
   receiver bias, which only a scaled target draws with; with MSAA it is built
   for the current sample count. */
struct RecompD3dBootDrawShader {
    uint32_t fvf;
    uint8_t output;
    bool depth_bias, projected_stages;
};

/* Shaders compiled at boot. The plain ones (18 FVFs) are those seen in the
   developers' logged runs plus 0x012, 0x102 and 0x204 from a player's log
   (#61). The variants are every other fixed-function key in 161 logged runs
   (demo, Story, menus, endings): Tina's ending draws 0x142 with projected
   stages, occlusion tests count 0x112 and 0x142, both also draw with the
   depth bias, and the HUD's 0x044, 0x104 and 0x144 cover the scene. A run that
   logs each key's variant named the cover outputs. Compiling them at boot
   keeps first use of each scene from stalling ~85 ms per shader. */
constexpr RecompD3dBootDrawShader kRecompD3dBootDrawShaders[] = {
    {0x012u}, {0x042u}, {0x044u}, {0x102u}, {0x104u}, {0x112u}, {0x116u}, {0x118u}, {0x11Au},
    {0x204u}, {0x142u}, {0x144u}, {0x212u}, {0x216u}, {0x21Au}, {0x242u}, {0x244u}, {0x404u},
    {0x142u, 0u, false, true}, {0x112u, 1u}, {0x142u, 1u}, {0x112u, 0u, true}, {0x142u, 0u, true},
    {0x044u, 2u}, {0x104u, 3u}, {0x144u, 2u}, {0x144u, 4u}};

/* What selects one draw shader: its FVF, vertex program and variant. */
struct RecompD3dDrawShaderKey {
    uint32_t fvf;
    uint32_t program_count;
    const uint32_t (*program)[4];
    bool coverage_only;
    bool scaled_depth_bias;
    bool sample_depth_bias;
    /* Tina's ending lights; only these shaders carry the projected-stage loop. */
    bool projected_stages;
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
/* The last four draw constants: the box a 2D draw's texcoords stay inside, or off. */
void recomp_d3d_draw_uv_clamp(const RecompD3dPresenterDrawCommand &draw, const RecompD3dVertexLayout &layout,
    bool texture_bound, float out[4]);

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
