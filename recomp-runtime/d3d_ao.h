// SPDX-License-Identifier: GPL-3.0-or-later
/* Screen-space ambient occlusion, opt-in as RECOMP_D3D_AO. Both presenters
   run it once per frame after the 3D scene and before the first 2D layer
   draw, so the HUD, menus, fonts, movies and full-screen effects drawn after
   it keep the pixels the game wrote. The occlusion pass reconstructs view
   space from the main depth buffer and the projection the scene drew with,
   sums a normal-oriented hemisphere over a rotated 12-tap disk, and the blur
   pass keeps it to surfaces at the same depth. The result multiplies the main
   target's RGB. */
#ifndef DOA2U_RECOMP_D3D_AO_H
#define DOA2U_RECOMP_D3D_AO_H

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/* HLSL for the three passes. The presenter declares ao_depth (the main depth
   buffer's sampleable view) and ao_value (the half-resolution occlusion
   target), and recompAoDepthAt / recompAoAt load a texel of each. Depth is
   loaded with sample 0 under RECOMP_AO_MSAA. */
#define RECOMP_AO_FIELDS "float4 ao_proj; float4 ao_params; float4 ao_screen;"

#define RECOMP_AO_DEPTH_HLSL \
    "float recompAoDepthAt(int2 p) {\n" \
    "#ifdef RECOMP_AO_MSAA\n" \
    " return ao_depth.Load(p, 0).x;\n" \
    "#else\n" \
    " return ao_depth.Load(int3(p, 0)).x;\n" \
    "#endif\n" \
    "}\n"

/* The sample disk: direction and distance, rotated per pixel so neighbouring
   pixels see different taps without a temporal noise pattern. */
#define RECOMP_AO_KERNEL_HLSL \
    "static const float2 kAoSamples[12] = {\n" \
    " {0.2041, 0.0000}, {-0.2607, 0.2388}, {0.0399, -0.4547}, {0.3286, 0.4286},\n" \
    " {-0.6030, -0.1067}, {0.5712, -0.3634}, {-0.1911, 0.7107}, {-0.3644, -0.7016},\n" \
    " {0.7906, 0.2887}, {-0.8224, 0.3395}, {0.3965, -0.8472}, {0.2930, 0.9341}};\n"

/* ao_proj holds 1/P00, 1/P11, P22 and 1/P32 of the row-vector projection, so
   1/z = (d - P22) / P32. The radius follows each pixel's distance, which
   keeps the picture the same at any output resolution. */
#define RECOMP_AO_COMMON_HLSL RECOMP_AO_DEPTH_HLSL RECOMP_AO_KERNEL_HLSL \
    "float3 recompAoPosition(int2 p) {\n" \
    " float invz = (recompAoDepthAt(p) - ao_proj.z) * ao_proj.w;\n" \
    " float z = 1.0 / max(invz, 1.0e-6);\n" \
    " float2 uv = (float2(p) + 0.5) / ao_screen.xy;\n" \
    " return float3((uv.x * 2.0 - 1.0) * z * ao_proj.x, (1.0 - uv.y * 2.0) * z * ao_proj.y, z);\n" \
    "}\n"

#define RECOMP_AO_OCCLUSION_HLSL \
    "float4 recompAoOcclusion(float4 position) {\n" \
    " int2 c = int2(position.xy);\n" \
    " int2 full = int2(ao_screen.xy);\n" \
    " int2 p = clamp(c * 2, int2(0, 0), full - 1);\n" \
    " float3 P = recompAoPosition(p);\n" \
    " float3 dx = recompAoPosition(clamp(p + int2(2, 0), int2(0, 0), full - 1)) - P;\n" \
    " float3 dy = recompAoPosition(clamp(p + int2(0, 2), int2(0, 0), full - 1)) - P;\n" \
    " float3 n = cross(dx, dy);\n" \
    " float len = length(n);\n" \
    " float ao = 1.0;\n" \
    " if (len > 1.0e-7) {\n" \
    "  n /= len;\n" \
    "  if (dot(n, P) > 0.0) n = -n;\n" \
    "  float world = clamp(P.z * ao_params.x, 0.03, 2.0);\n" \
    "  float pixels = clamp(world * (ao_screen.y * 0.5) / (ao_proj.y * max(P.z, 1.0e-3)), 2.0, 64.0);\n" \
    "  float angle = frac(sin(dot(float2(c), float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;\n" \
    "  float co = cos(angle), si = sin(angle);\n" \
    "  float occ = 0.0;\n" \
    "  [unroll] for (int i = 0; i < 12; ++i) {\n" \
    "   float2 o = float2(kAoSamples[i].x * co - kAoSamples[i].y * si,\n" \
    "    kAoSamples[i].x * si + kAoSamples[i].y * co) * pixels;\n" \
    "   float3 v = recompAoPosition(clamp(p + int2(floor(o)), int2(0, 0), full - 1)) - P;\n" \
    "   float d2 = dot(v, v);\n" \
    "   if (d2 > 1.0e-8 && d2 < world * world)\n" \
    "    occ += saturate(dot(n, v) * rsqrt(d2) - ao_params.z) * (1.0 - d2 / (world * world));\n" \
    "  }\n" \
    "  ao = saturate(1.0 - ao_params.y * occ * (1.0 / 12.0));\n" \
    " }\n" \
    " return float4(ao, ao, ao, 1.0); }\n"

#define RECOMP_AO_BLUR_HLSL \
    "float4 recompAoBlur(float4 position) {\n" \
    " int2 c = int2(position.xy);\n" \
    " int2 half_size = int2(ao_screen.zw);\n" \
    " int2 full = int2(ao_screen.xy);\n" \
    " float zc = recompAoPosition(clamp(c * 2, int2(0, 0), full - 1)).z;\n" \
    " float sum = 0.0, weight = 0.0;\n" \
    " [unroll] for (int y = -2; y <= 2; ++y) {\n" \
    "  [unroll] for (int x = -2; x <= 2; ++x) {\n" \
    "   int2 q = clamp(c + int2(x, y), int2(0, 0), half_size - 1);\n" \
    "   float z = recompAoPosition(clamp(q * 2, int2(0, 0), full - 1)).z;\n" \
    "   float w = exp(-float(x * x + y * y) * 0.25) / (1.0 + abs(z - zc) / max(zc * 0.02, 1.0e-4));\n" \
    "   sum += recompAoAt(q) * w;\n" \
    "   weight += w;\n" \
    "  }\n" \
    " }\n" \
    " float ao = weight > 0.0 ? sum / weight : 1.0;\n" \
    " return float4(ao, ao, ao, 1.0); }\n"

#define RECOMP_AO_APPLY_HLSL \
    "float4 recompAoApply(float2 uv) {\n" \
    " float ao = ao_value.Sample(ao_sampler, uv).r;\n" \
    " return float4(ao, ao, ao, 1.0); }\n"

/* The passes' constants. SDL_GPU's Vulkan backend binds only 4 KiB of a
   uniform buffer, and this stays far below that. */
struct RecompAoConstants {
    /* 1/P00, 1/P11, P22 and 1/P32 of the projection's last two rows. */
    float depth[4];
    /* Radius per view-space unit, strength, normal bias. */
    float params[4];
    /* Full-resolution width and height, then the occlusion target's. */
    float screen[4];
};

/* False when the projection cannot reconstruct view space: a matrix that
   mixes eye x/y into w, or zeros from a draw whose state was never captured. */
inline bool recomp_ao_projection(const float projection[16])
{
    const float p00 = projection[0], p11 = projection[5];
    const float p22 = projection[10], p32 = projection[14];
    if (!(p00 > 0.0f) || !(p11 > 0.0f) || !(p22 >= 1.0f) || !(p32 < 0.0f)) return false;
    if (!std::isfinite(p00) || !std::isfinite(p11) || !std::isfinite(p22) || !std::isfinite(p32)) return false;
    // clip.w must be the eye depth: the fourth column is (0, 0, 1, 0).
    return std::fabs(projection[3]) <= 1.0e-4f && std::fabs(projection[7]) <= 1.0e-4f &&
        std::fabs(projection[11] - 1.0f) <= 1.0e-4f && std::fabs(projection[15]) <= 1.0e-4f;
}

inline RecompAoConstants recomp_ao_constants(const float projection[16],
    uint32_t width, uint32_t height, uint32_t occlusion_width, uint32_t occlusion_height)
{
    RecompAoConstants constants{};
    constants.depth[0] = 1.0f / projection[0];
    constants.depth[1] = 1.0f / projection[5];
    constants.depth[2] = projection[10];
    constants.depth[3] = 1.0f / projection[14];
    // A soft, subtle darkening in creases; it never reaches black.
    constants.params[0] = 0.04f;  // world radius per unit of view depth
    constants.params[1] = 1.1f;   // how much occlusion darkens
    constants.params[2] = 0.15f;  // normal-facing bias against self-occlusion
    constants.screen[0] = static_cast<float>(width);
    constants.screen[1] = static_cast<float>(height);
    constants.screen[2] = static_cast<float>(occlusion_width);
    constants.screen[3] = static_cast<float>(occlusion_height);
    return constants;
}

/* RECOMP_D3D_AO: 1 turns it on; unset or 0 leaves it off. */
inline bool recomp_ao_env_enabled()
{
    const char *value = std::getenv("RECOMP_D3D_AO");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

#endif
