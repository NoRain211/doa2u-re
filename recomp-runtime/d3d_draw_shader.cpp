// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_draw_shader.h"
#include "d3d_vertex_program.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

struct Viewport {
    float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
};

Viewport hostViewport(const RecompD3dDrawTargetState &target)
{
    return {target.viewport[0], target.viewport[1], target.viewport[2],
        target.viewport[3], target.viewport[4], target.viewport[5]};
}

/* Untransformed positions with a host-side world-view-projection composite.

   Vertex components other than position vary by FVF: this title draws both
   XYZ|NORMAL|TEX1 and XYZ|DIFFUSE|TEX1, whose texcoords sit at different
   offsets and whose colors come from different sources. One shader cannot
   serve both, so the shader is assembled per FVF from the decoded layout and
   cached with its matching input layout.

   Vertex color comes from the stream or the admitted fixed-function lighting
   path. Unsupported lighting configurations retain the earlier texture/normal
   fallback. Texture alpha remains independent of lit diffuse RGB. */
constexpr char kDrawShaderPrologue[] =
    "cbuffer Transform : register(b0) {\n"
    "    row_major float4x4 wvp[4];\n"
    "    float4 draw_flags;\n"
    "    float4 blend_flags;\n"
    "    float4 texture_factor;\n"
    "    float4 lighting_flags;\n"
    "    float4 texture_flags;\n"
    "    row_major float4x4 reflection_world_view;\n"
    "    row_major float4x4 reflection_normal;\n"
    "    row_major float4x4 reflection_transform;\n"
    "    float4 reflection_diffuse;\n"
    "    float4 reflection_flags;\n"
    "    float4 vc[192];\n"
    "    row_major float4x4 directional_normals[4];\n"
    "    float4 directional_base;\n"
    "    float4 directional_material;\n"
    "    float4 directional_directions[8];\n"
    "    float4 directional_colors[8];\n"
    "    float4 directional_flags;\n"
    "    row_major float4x4 light_world[4];\n"
    "    float4 light_positions[8];\n"
    "    float4 light_attenuation[8];\n"
    "    float4 light_ambient[8];\n"
    "    float4 light_specular[8];\n"
    "    float4 light_view;\n"
    "    float4 light_stage;\n"
    "    float4 filter_flags;\n"
    "    float4 filter_texel;\n"
    "    float4 filter_constants[4];\n"
    "    row_major float4x4 reflection_transform1;\n"
    "    float4 bump_matrix[2];\n"
    "    float4 bump_fog;\n"
    "    float4 fog_color;\n"
    "    row_major float4x4 reflection_blend_world_view[3];\n"
    "    row_major float4x4 reflection_blend_normal[3];\n"
    "    row_major float4x4 reflection_transform2;\n"
    "    float4 reflection_stage2_flags;\n"
    "    float4 raster_bias;\n"
    "    row_major float4x4 projected_transform[3];\n"
    "    float4 projected_stages;\n"
    "    float4 projected_slots;\n"
    "}\n"
    "Texture2D guest_texture : register(t0);\n"
    "Texture3D guest_volume : register(t3);\n"
    "SamplerState guest_sampler : register(s0);\n"
    "Texture2D alpha_mask : register(t1);\n"
    "SamplerState mask_sampler : register(s1);\n"
    "Texture2D bump_map : register(t2);\n"
    "SamplerState bump_sampler : register(s2);\n";

/* Builds the draw shader for one decoded vertex layout. Only the components
   the layout actually carries appear in VSIn, so the input layout and the
   shader signature always agree. */
int buildDrawShaderSource(
    const RecompD3dVertexLayout &layout,
    char *out,
    size_t out_size, bool coverage_only)
{
    const bool has_normal =
        layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_diffuse =
        layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT;
    const bool has_texcoord =
        layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT;
    const bool four_coords = layout.texcoord_count == 4u;
    const char *extra_coords = four_coords
        ? "    float2 texcoord1 : TEXCOORD1;\n"
          "    float2 texcoord2 : TEXCOORD2;\n"
          "    float2 texcoord3 : TEXCOORD3;\n"
        : layout.texcoord_count == 2u ? "    float2 texcoord1 : TEXCOORD1;\n" : "";

    char position[768];
    if (layout.pretransformed) {
        std::snprintf(position, sizeof position,
            "    output.position = mul(float4(input.position.xyz, 1.0f), wvp[0]) / input.position.w;\n");
    } else if (layout.blend_weight_count != 0u) {
        std::snprintf(position, sizeof position,
            "    if (blend_flags.x > 0.5f) {\n"
            "    output.position = float4(0, 0, 0, 0);\n"
            "    float remainder = 1.0f;\n"
            "    [unroll] for (uint i = 0; i < %u; ++i) {\n"
            "        output.position += input.weights[i] * mul(float4(input.position, 1), wvp[i]);\n"
            "        remainder -= input.weights[i];\n"
            "    }\n"
            "    output.position += remainder * mul(float4(input.position, 1), wvp[%u]);\n"
            "    } else { output.position = mul(float4(input.position, 1), wvp[0]); }\n",
            layout.blend_weight_count, layout.blend_weight_count);
    } else {
        std::snprintf(position, sizeof position,
            "    output.position = mul(float4(input.position, 1.0f), wvp[0]);\n");
    }
    if (coverage_only) {
        return std::snprintf(out, out_size,
            "%sstruct VSIn { float3 position : POSITION; %s };\n"
            "struct VSOut { float4 position : SV_POSITION; };\n"
            "VSOut vs_main(VSIn input) { VSOut output;\n%s return output; }\n"
            "float4 ps_main(VSOut input) : SV_TARGET { return 0; }\n",
            kDrawShaderPrologue,
            layout.blend_weight_count ? "float3 weights : BLENDWEIGHT;" : "", position);
    }
    return std::snprintf(
        out,
        out_size,
        "%s"
        "struct VSIn {\n"
        "    %s position : POSITION;\n"
        "%s%s%s%s%s"
        "};\n"
        "struct VSOut {\n"
        "    float4 position : SV_POSITION;\n"
        "    float4 color : COLOR0;\n"
        "    float2 texcoord : TEXCOORD0;\n"
        "    float volume_coord : TEXCOORD11;\n"
        "    float2 reflection_coord : TEXCOORD4;\n"
        "    float3 lit_specular : TEXCOORD6;\n"
        "    float projected_q : TEXCOORD7;\n"
        "    float2 reflection_coord2 : TEXCOORD10;\n"
        "    float3 projected_eye : TEXCOORD12;\n"
        "%s"
        "};\n"
        "VSOut vs_main(VSIn input) {\n"
        "    VSOut output = (VSOut)0;\n"
        "%s"
        "%s"
        "%s"
        "%s"
        "    output.reflection_coord = float2(0, 0);\n"
        "%s"
        "%s"
        "    return output;\n"
        "}\n"
        "float4 ps_main(VSOut input) : SV_TARGET {\n"
        /* Projective texgen divides after interpolation, as NV2A does. */
        "    if (filter_texel.z > 2.5f) input.texcoord /= input.projected_q;\n"
        "    float4 shaded = input.color;\n"
        "    if (reflection_flags.x > 0.5f) {\n"
        "        float4 diffuse = %s;\n"
        "        if (directional_flags.x > 0.5f) diffuse.rgb = input.color.rgb;\n"
        "        float4 base = guest_texture.Sample(guest_sampler, input.texcoord);\n"
        "        if (filter_flags.w > 0.5f && base.a == 0.0f) discard;\n"
        "        base.a *= diffuse.a;\n"
        "        float4 env = alpha_mask.Sample(mask_sampler, input.reflection_coord);\n"
        "        if (reflection_stage2_flags.y > 0.5f && env.a == 0.0f) discard;\n"
        /* w: bit 0 premultiplied blend, bit 1 stage-2 pass-through (no diffuse),
           bit 2 color-only blend (stage-1 alpha keeps CURRENT). */
        "        shaded = (fmod(reflection_flags.w, 2.0f) > 0.5f\n"
        "            ? saturate(env + base * (1.0f - env.a)) : lerp(base, env, env.a));\n"
        "        if (reflection_flags.w > 3.5f) shaded.a = base.a;\n"
        "        if (reflection_stage2_flags.x > 0.5f) {\n"
        "            float4 third = bump_map.Sample(bump_sampler, input.reflection_coord2);\n"
        "            shaded = lerp(shaded, third, third.a);\n"
        "        }\n"
        "        shaded *= fmod(reflection_flags.w, 4.0f) > 1.5f ? 1.0f : diffuse;\n"
        "    } else\n"
        "%s"
        "    if (blend_flags.y == 1.0f) {\n"
        "        shaded = texture_factor;\n"
        "    } else if (draw_flags.x > 0.5f) {\n"
        "        float4 sampled = reflection_stage2_flags.z > 0.5f\n"
        "            ? guest_volume.Sample(guest_sampler, float3(input.texcoord, input.volume_coord))\n"
        "            : guest_texture.Sample(guest_sampler, input.texcoord * texture_flags.xy);\n"
        "        if (filter_flags.w > 0.5f && sampled.a == 0.0f) discard;\n"
        "        if (lighting_flags.y > 0.5f) sampled.rgb = 1.0f;\n"
        "        shaded %s sampled;\n"
        "        if (directional_flags.w > 0.5f) {\n"
        "            shaded.rgb = (light_stage.x > 0.5f ? sampled.rgb : 1.0f) *\n"
        "                (light_stage.y > 1.5f ? texture_factor.rgb : light_stage.y > 0.5f ? input.color.rgb : 1.0f);\n"
        "            shaded.a = (light_stage.z > 0.5f ? sampled.a : 1.0f) *\n"
        "                (light_stage.w > 1.5f ? texture_factor.a : light_stage.w > 0.5f ? input.color.a : 1.0f);\n"
        "        } else if (directional_flags.x > 0.5f) shaded.rgb *= input.color.rgb;\n"
        "        if (lighting_flags.x > 0.5f) shaded.rgb = 0.0f;\n"
        "        shaded.a = blend_flags.w > 2.5f ? input.color.a\n"
            "            : blend_flags.w > 1.5f ? sampled.a\n"
            "            : blend_flags.w > 0.5f ? blend_flags.z : shaded.a * blend_flags.z;\n"
        "        if (blend_flags.y > 5.5f) shaded.rgb = saturate(4.0f * shaded.rgb * texture_factor.rgb);\n"
        "        else if (blend_flags.y > 4.5f) shaded.rgb *= texture_factor.rgb;\n"
        "        else if (blend_flags.y > 3.5f) shaded.a *= texture_factor.a;\n"
        "        else if (blend_flags.y > 2.5f) shaded.rgb = saturate(2.0f * shaded.rgb * texture_factor.rgb);\n"
        "        else if (blend_flags.y > 1.5f) shaded *= texture_factor;\n"
        "    }\n"
        /* Projected ADD stages (0x257ee0) precede the final combiner's specular. */
        "    if (projected_stages.x > 0.5f) {\n"
        "        [unroll] for (uint i = 0; i < 3; ++i) {\n"
        "            float slot = projected_slots[i];\n"
        "            if (i >= (uint)projected_stages.x || slot < -0.5f) continue;\n"
        "            float3 g = mul(float4(input.projected_eye, 1), projected_transform[i]).xyz;\n"
        "            float2 uv = g.xy / g.z;\n"
        "            float4 t;\n"
        "            if (slot < 0.5f) t = guest_texture.Sample(guest_sampler, uv);\n"
        "            else if (slot < 1.5f) t = alpha_mask.Sample(mask_sampler, uv);\n"
        "            else t = bump_map.Sample(bump_sampler, uv);\n"
        "            shaded.rgb = saturate(shaded.rgb + t.rgb);\n"
        "        }\n"
        "    }\n"
        /* Fixed function adds specular after the texture stages, textured or not. */
        "    if (directional_flags.w > 0.5f) shaded.rgb += input.lit_specular;\n"
        /* A stage-1 mask; z > 1.5 adds its ALPHAKILL, and z > 2.5 only kills. */
        "    if (reflection_flags.z > 0.5f) {\n"
        "        float mask = alpha_mask.SampleBias(mask_sampler, input.reflection_coord, reflection_flags.w).a;\n"
        "        if (reflection_flags.z > 1.5f && mask == 0.0f) discard;\n"
        "        if (reflection_flags.z < 2.5f) shaded.a *= mask;\n"
        "    }\n"
        /* NV2A compares rounded 8-bit alpha. Function values follow
           RecompD3dCompareFunc, from NEVER (0) to ALWAYS (7). */
        "    if (draw_flags.y > 0.5f) {\n"
        "        float alpha = round(saturate(shaded.a) * 255.0f);\n"
        "        float ref = draw_flags.w;\n"
        "        int func = (int)draw_flags.z;\n"
        "        bool alpha_pass = func == 7 ||\n"
        "            (func == 1 && alpha < ref) ||\n"
        "            (func == 2 && alpha == ref) ||\n"
        "            (func == 3 && alpha <= ref) ||\n"
        "            (func == 4 && alpha > ref) ||\n"
        "            (func == 5 && alpha != ref) ||\n"
        "            (func == 6 && alpha >= ref);\n"
        "        if (!alpha_pass) discard;\n"
        "    }\n"
        "    return shaded;\n"
        "}\n",
        kDrawShaderPrologue,
        layout.pretransformed ? "float4" : "float3",
        layout.blend_weight_count ? "    float3 weights : BLENDWEIGHT;\n" : "",
        has_normal ? "    float3 normal : NORMAL;\n" : "",
        has_diffuse ? "    float4 diffuse : COLOR0;\n" : "",
        has_texcoord ? "    float2 texcoord : TEXCOORD0;\n" : "",
        extra_coords,
        extra_coords,
        position,
        /* Guest vertex color when the stream has one; otherwise the normal
           stands in so surfaces remain distinguishable. */
        has_diffuse
            ? "    output.color = input.diffuse;\n"
            : (has_normal
                   ? "    output.color = "
                     "float4(abs(normalize(input.normal)), 1.0f);\n"
                   : "    output.color = float4(0.75f, 0.75f, 0.78f, 1.0f);\n"),
        has_texcoord
            ? "    output.texcoord = input.texcoord;\n"
            : "    output.texcoord = float2(0.0f, 0.0f);\n",
        four_coords
            ? "    output.texcoord1 = input.texcoord1;\n"
              "    output.texcoord2 = input.texcoord2;\n"
              "    output.texcoord3 = input.texcoord3;\n"
            : layout.texcoord_count == 2u ? "    output.texcoord1 = input.texcoord1;\n" : "",
        /* z > 2.5: a kill-only stage-1 mask reads UV0. */
        layout.texcoord_count == 2u
            ? "    if (reflection_flags.z > 0.5f) output.reflection_coord = reflection_flags.z > 2.5f ? input.texcoord : input.texcoord1;\n"
            : has_texcoord ? "    if (reflection_flags.z > 2.5f) output.reflection_coord = input.texcoord;\n" : "",
        has_texcoord && !layout.pretransformed
            ? (has_normal
                /* Mesh-UV reflections (x == 2) may pair with stage-0 texgen. */
                ? "    if (reflection_flags.x > 1.5f) output.reflection_coord = input.texcoord;\n"
                  "    if (reflection_stage2_flags.w > 0.5f) output.reflection_coord2 = input.texcoord;\n"
                  "    if (abs(filter_texel.z - 2.0f) < 0.5f) {\n"
                  "        float3 uv = mul(float4(input.texcoord, 1, 0), reflection_transform).xyz;\n"
                  "        output.texcoord = uv.xy; output.volume_coord = uv.z;\n"
                  "    }\n"
                  "    if (filter_texel.z > 2.5f) {\n"
                  "        float3 g = mul(mul(float4(input.position, 1), reflection_world_view), reflection_transform).xyz;\n"
                  "        output.texcoord = g.xy;\n"
                  "        output.volume_coord = g.z;\n"
                  "        output.projected_q = reflection_stage2_flags.z > 0.5f ? 1.0f : g.z;\n"
                  "        output.projected_eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "    }\n"
                  "    if (abs(reflection_flags.x - 1.0f) < 0.5f || abs(filter_texel.z - 1.0f) < 0.5f ||\n"
                  "        (reflection_stage2_flags.x > 0.5f && reflection_stage2_flags.w < 0.5f)) {\n"
                  "        float3 eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "        float3 n = mul(float4(input.normal, 0), reflection_normal).xyz;\n"
                  "        if (reflection_flags.y > 0.5f) n = normalize(n);\n"
                  "        float3 r = reflect(normalize(eye), n);\n"
                  "        float2 generated = mul(float4(r, 1), reflection_transform).xy;\n"
                  "        if (reflection_stage2_flags.x > 0.5f && reflection_stage2_flags.w < 0.5f)\n"
                  "            output.reflection_coord2 = mul(float4(r, 1), reflection_transform2).xy;\n"
                  "        if (filter_texel.z > 0.5f) output.texcoord = generated;\n"
                  "        if (abs(reflection_flags.x - 1.0f) < 0.5f) output.reflection_coord = filter_texel.z > 0.5f\n"
                  "            ? mul(float4(r, 1), reflection_transform1).xy : generated;\n"
                  "    }\n"
                /* shortcut: without a normal stream NV2A reads a zero inline normal, so
                   r is the eye direction; check the inline register if env maps look wrong. */
                : "    if (reflection_flags.x > 1.5f) output.reflection_coord = input.texcoord;\n"
                  "    if (reflection_stage2_flags.w > 0.5f) output.reflection_coord2 = input.texcoord;\n"
                  "    if (abs(filter_texel.z - 2.0f) < 0.5f) {\n"
                  "        float3 uv = mul(float4(input.texcoord, 1, 0), reflection_transform).xyz;\n"
                  "        output.texcoord = uv.xy; output.volume_coord = uv.z;\n"
                  "    }\n"
                  "    if (filter_texel.z > 2.5f) {\n"
                  "        float3 g = mul(mul(float4(input.position, 1), reflection_world_view), reflection_transform).xyz;\n"
                  "        output.texcoord = g.xy;\n"
                  "        output.volume_coord = g.z;\n"
                  "        output.projected_q = reflection_stage2_flags.z > 0.5f ? 1.0f : g.z;\n"
                  "        output.projected_eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "    }\n"
                  "    if (abs(reflection_flags.x - 1.0f) < 0.5f || abs(filter_texel.z - 1.0f) < 0.5f ||\n"
                  "        (reflection_stage2_flags.x > 0.5f && reflection_stage2_flags.w < 0.5f)) {\n"
                  "        float3 eye = mul(float4(input.position, 1), reflection_world_view).xyz;\n"
                  "        float3 r = normalize(eye);\n"
                  "        float2 generated = mul(float4(r, 1), reflection_transform).xy;\n"
                  "        if (reflection_stage2_flags.x > 0.5f && reflection_stage2_flags.w < 0.5f)\n"
                  "            output.reflection_coord2 = mul(float4(r, 1), reflection_transform2).xy;\n"
                  "        if (filter_texel.z > 0.5f) output.texcoord = generated;\n"
                  "        if (abs(reflection_flags.x - 1.0f) < 0.5f) output.reflection_coord = filter_texel.z > 0.5f\n"
                  "            ? mul(float4(r, 1), reflection_transform1).xy : generated;\n"
                  "    }\n")
            : "",
        has_diffuse ? "input.color" : "reflection_diffuse",
        four_coords
            /* MINUS1_TO_1_D3D maps a texel byte b to (b - 128) / 127. */
            ? "    if (filter_texel.w > 0.5f) {\n"
              "        float3 n = (round(alpha_mask.Sample(mask_sampler, input.texcoord).rgb * 255.0f) - 128.0f) / 127.0f;\n"
              "        float2 st = float2(dot(float3(input.texcoord1, input.texcoord2.x), n),\n"
              "            dot(float3(input.texcoord2.y, input.texcoord3), n));\n"
              "        shaded = guest_texture.Sample(guest_sampler, st * texture_flags.xy);\n"
              "    } else if (texture_flags.z > 0.5f) {\n"
              "        float4 t0 = guest_texture.Sample(guest_sampler, input.texcoord * texture_flags.xy);\n"
              "        float4 t1 = guest_texture.Sample(guest_sampler, input.texcoord1 * texture_flags.xy);\n"
              "        float4 t2 = guest_texture.Sample(guest_sampler, input.texcoord2 * texture_flags.xy);\n"
              "        float4 t3 = guest_texture.Sample(guest_sampler, input.texcoord3 * texture_flags.xy);\n"
              "        shaded = saturate(filter_flags.z *\n"
              "            (saturate(filter_flags.y * (filter_constants[0] * t0 + filter_constants[1] * t1))\n"
              "           + saturate(filter_flags.y * (filter_constants[2] * t2 + filter_constants[3] * t3))));\n"
              "    } else\n"
            : layout.texcoord_count == 2u
            ? "    if (texture_flags.w > 0.5f) {\n"
              "        float3 rgb = guest_texture.Sample(guest_sampler, input.texcoord1 * texture_flags.xy).rgb;\n"
              "        float alpha = alpha_mask.Sample(mask_sampler, input.texcoord * lighting_flags.zw).a;\n"
              "        shaded = input.color * float4(rgb, alpha);\n"
              "    } else\n"
            : has_texcoord
            /* NV2A Gaussian-cubic: nine level-0 taps at one guest texel, 1-2-1 weights.
               A scaled back buffer has n host texels per guest texel; each tap box-averages
               them so thin highlights match the guest image instead of aliasing into halos. */
            ? "    if (filter_flags.x > 1.5f) {\n"
              "        float l = dot(guest_texture.Sample(guest_sampler, input.texcoord * texture_flags.xy).rgb,\n"
              "            filter_constants[0].rgb);\n"
              "        shaded = saturate(filter_constants[1] * clamp(l, -1.0f, 1.0f));\n"
              "    } else if (filter_flags.x > 0.5f) {\n"
              "        float2 uv = input.texcoord * texture_flags.xy;\n"
              "        float tw, th;\n"
              "        guest_texture.GetDimensions(tw, th);\n"
              "        int2 n = clamp(int2(ceil(float2(tw, th) * filter_texel.xy - 0.01f)), 1, 8);\n"
              "        float4 t0 = 0.0f;\n"
              "        [unroll] for (int y = -1; y <= 1; ++y)\n"
              "            [unroll] for (int x = -1; x <= 1; ++x)\n"
              "                [loop] for (int j = 0; j < n.y; ++j)\n"
              "                    [loop] for (int i = 0; i < n.x; ++i)\n"
              "                        t0 += (2 - abs(x)) * (2 - abs(y)) / (16.0f * n.x * n.y) *\n"
              "                            guest_texture.SampleLevel(guest_sampler, uv + (float2(x, y) +\n"
              "                                (float2(i, j) + 0.5f) / float2(n) - 0.5f) * filter_texel.xy, 0);\n"
              "        shaded = saturate(2.0f * (t0 - filter_constants[0]) - 1.0f);\n"
              "    } else\n" : "",
        /* The fixed-function diffuse default is white. Preserve texture alpha
           before the shared alpha test and SRC_ALPHA blending. */
        has_diffuse ? "*=" : "=");
}

} // namespace

/* Builds the HLSL for one FVF, plus its vertex program when it has one. */
bool recomp_d3d_draw_shader_source(
    const RecompD3dDrawShaderKey &pipeline, RecompD3dVertexLayout &layout,
    std::string &compiled_source)
{
    const uint32_t fvf = pipeline.fvf;
    if (!recomp_d3d_fvf_layout(fvf, &layout)) {
        return false;
    }

    const int source_size = buildDrawShaderSource(layout, nullptr, 0u, pipeline.coverage_only);
    if (source_size < 0) return false;
    compiled_source.resize(static_cast<size_t>(source_size) + 1u);
    if (buildDrawShaderSource(layout, compiled_source.data(), compiled_source.size(),
            pipeline.coverage_only) != source_size) return false;
    compiled_source.resize(static_cast<size_t>(source_size));
    if (pipeline.coverage_only) return true;
    if (layout.blend_weight_count != 0u && !layout.pretransformed &&
        layout.texcoord_offset != RECOMP_D3D_FVF_ABSENT) {
        std::string reflection = "        if (blend_flags.x > 0.5f) {\n"
            "            eye *= input.weights[0];\n";
        const bool normal = layout.normal_offset != RECOMP_D3D_FVF_ABSENT;
        if (normal) reflection += "            n *= input.weights[0];\n";
        reflection += "            float remainder = 1.0f - input.weights[0];\n";
        for (uint32_t i = 1u; i <= layout.blend_weight_count; ++i) {
            const auto matrix = std::to_string(i - 1u);
            const auto weight = i == layout.blend_weight_count ? "remainder" :
                "input.weights[" + std::to_string(i) + "]";
            reflection += "            eye += " + weight + " * mul(float4(input.position, 1), reflection_blend_world_view[" + matrix + "]).xyz;\n";
            if (normal) reflection += "            n += " + weight + " * mul(float4(input.normal, 0), reflection_blend_normal[" + matrix + "]).xyz;\n";
            if (i != layout.blend_weight_count) reflection += "            remainder -= " + weight + ";\n";
        }
        reflection += "        }\n";
        const auto point = compiled_source.find(normal ? "        if (reflection_flags.y" : "        float3 r = normalize(eye);");
        if (point == std::string::npos) return false;
        compiled_source.insert(point, reflection);
    }
    if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT && !layout.pretransformed &&
        layout.diffuse_offset == RECOMP_D3D_FVF_ABSENT) {
        std::string lighting = "    if (directional_flags.x > 0.5f) {\n"
            "        float3 n = mul(float4(input.normal,0),directional_normals[0]).xyz;\n"
            "        float3 world_pos = mul(float4(input.position,1),light_world[0]).xyz;\n";
        if (layout.blend_weight_count) {
            lighting += "        if (blend_flags.x > 0.5f) { n=0; world_pos=0; float remainder=1;\n";
            for (uint32_t i=0; i<layout.blend_weight_count; ++i) {
                const auto index=std::to_string(i);
                lighting += "n += input.weights["+index+"] * mul(float4(input.normal,0),directional_normals["+index+"]).xyz;\n"
                    "world_pos += input.weights["+index+"] * mul(float4(input.position,1),light_world["+index+"]).xyz;\n"
                    "remainder -= input.weights["+index+"];\n";
            }
            lighting += "n += remainder * mul(float4(input.normal,0),directional_normals["+
                std::to_string(layout.blend_weight_count)+"]).xyz;\n"
                "world_pos += remainder * mul(float4(input.position,1),light_world["+
                std::to_string(layout.blend_weight_count)+"]).xyz; }\n";
        }
        lighting += "        if (directional_flags.y > 0.5f && dot(n,n)>0) n *= rsqrt(dot(n,n));\n"
            "        float3 rgb=directional_base.rgb, spec=0;\n"
            "        for (uint i=0; i<(uint)directional_flags.z; ++i) {\n"
            "            float3 l = directional_directions[i].xyz; float atten = 1;\n"
            "            if (light_positions[i].w > 0.5f) {\n"
            "                l = light_positions[i].xyz - world_pos;\n"
            "                float d = length(l);\n"
            "                float denominator = dot(light_attenuation[i].xyz,float3(1,d,d*d));\n"
            "                atten = d <= light_attenuation[i].w && denominator > 0 ? 1/denominator : 0;\n"
            "                l = d > 0 ? l/d : float3(0,0,0);\n"
            "            }\n"
            "            rgb += (directional_material.rgb * directional_colors[i].rgb * max(0,dot(n,l)) + light_ambient[i].rgb) * atten;\n"
            "            if (light_view.w > 0 && dot(n,l) > 0)\n"
            "                spec += atten*light_specular[i].rgb*pow(max(dot(n,normalize(l+light_view.xyz)),1e-6f),light_view.w);\n"
            "        }\n"
            "        output.color.rgb=saturate(rgb);\n"
            "        if (directional_flags.w > 0.5f) output.color.a=directional_material.a;\n"
            "        output.lit_specular=saturate(spec);\n"
            "    }\n";
        const auto uv = compiled_source.find("    output.texcoord =");
        if (uv == std::string::npos) return false;
        compiled_source.insert(uv, lighting);
    }
    if (pipeline.program_count) {
        if (fvf != 0x112u && fvf != 0x012u && fvf != 0x142u && fvf != 0x442u) return false;
        std::string body;
        if (!recomp_d3d_vertex_program_source(pipeline.program, pipeline.program_count, fvf, body)) {
            std::fprintf(stderr, "recomp d3d presenter: vertex program untranslated fvf=%03x count=%u\n",
                fvf, pipeline.program_count);
            return false;
        }
        const auto begin = compiled_source.find("VSOut vs_main(");
        const auto end = compiled_source.find("float4 ps_main(", begin);
        if (begin == std::string::npos || end == std::string::npos) return false;
        compiled_source.replace(begin, end-begin, "VSOut vs_main(VSIn input) { VSOut output = (VSOut)0;\n" + body + "return output; }\n");
        // Preserve q until the pixel shader so projection follows interpolation.
        const std::string declaration = "struct VSOut {";
        const auto fields = compiled_source.find(declaration);
        if (fields == std::string::npos) return false;
        compiled_source.insert(fields + declaration.size(), "\n    float2 program_q : TEXCOORD5;\n"
            "    float program_fog : TEXCOORD8;\n    float4 program_t23 : TEXCOORD9;\n"
            "    float4 program_d1 : COLOR1;\n");
        const std::string pixel = "float4 ps_main(VSOut input) : SV_TARGET {";
        const auto sample = compiled_source.find(pixel);
        if (sample == std::string::npos) return false;
        compiled_source.insert(sample + pixel.size(),
            "\n    input.texcoord /= input.program_q.x;\n"
            "    if (reflection_flags.z > 0.5f) input.reflection_coord /= input.program_q.y;\n");
        // The stage effect's bump combiner; see RecompD3dPresenterDrawCommand::bump_env.
        const std::string shaded = "    float4 shaded = input.color;\n";
        const auto branch = compiled_source.find(shaded);
        if (branch == std::string::npos) return false;
        compiled_source.insert(branch + shaded.size(),
            "    if (bump_fog.w > 1.5f) {\n"
            "        float2 n = bump_map.Sample(bump_sampler, input.texcoord).rg;\n"
            "        float4 t1 = guest_texture.Sample(guest_sampler, (input.reflection_coord +\n"
            "            float2(dot(bump_matrix[0].xz, n), dot(bump_matrix[0].yw, n))) * texture_flags.xy);\n"
            "        float4 t3 = alpha_mask.Sample(mask_sampler, input.program_t23.zw +\n"
            "            float2(dot(bump_matrix[1].xz, n), dot(bump_matrix[1].yw, n)));\n"
            "        float4 v0 = saturate(input.color), v1 = saturate(input.program_d1);\n"
            "        float s = saturate(dot(v0.rgb * 2.0f - 1.0f, t3.rgb * 2.0f - 1.0f));\n"
            "        s *= s; s *= s; s *= s;\n"
            "        float3 r0 = clamp(s + v1.rgb * t1.a - 1.0f, -1.0f, 1.0f);\n"
            "        r0 = clamp(r0 + t3.a * t1.rgb, -1.0f, 1.0f);\n"
            "        float f = bump_fog.z > 0.5f ? saturate(bump_fog.x + bump_fog.y * input.program_fog) : 1.0f;\n"
            "        shaded = float4(lerp(fog_color.rgb, saturate(r0), f), t3.a * v0.a);\n"
            "    } else if (bump_fog.w > 0.5f) {\n"
            "        float2 n = bump_map.Sample(bump_sampler, input.texcoord).rg;\n"
            "        float4 t2 = guest_texture.Sample(guest_sampler, (input.program_t23.xy +\n"
            "            float2(dot(bump_matrix[0].xz, n), dot(bump_matrix[0].yw, n))) * texture_flags.xy);\n"
            "        float4 t3 = alpha_mask.Sample(mask_sampler, input.program_t23.zw +\n"
            "            float2(dot(bump_matrix[1].xz, n), dot(bump_matrix[1].yw, n)));\n"
            "        float k = t3.a * t3.a; k *= k;\n"
            "        float4 v0 = saturate(input.color);\n"
            "        float f = bump_fog.z > 0.5f ? saturate(bump_fog.x + bump_fog.y * input.program_fog) : 1.0f;\n"
            "        shaded = float4(lerp(fog_color.rgb, t2.rgb * (1.0f - k) * v0.rgb, f), v0.a);\n"
            "    } else\n");
    }
    if (fvf == 0x42u && pipeline.program_count == 0u) {
        /* shortcut: trapezoidal alpha coverage and square caps approximate NV2A's
           line filter, calibrate against hardware when comparing the line effect. */
        compiled_source +=
            "\n[maxvertexcount(8)] void gs_main(line VSOut v[2], inout TriangleStream<VSOut> output) {\n"
            "    float4 style = filter_constants[0];\n"
            "    if (style.x <= 0) return;\n"
            "    VSOut a = v[0], b = v[1];\n"
            "    [unroll] for (uint plane=0; plane<3; ++plane) {\n"
            "        float da = plane==0 ? a.position.z : plane==1 ? a.position.w-a.position.z : a.position.w-1e-6f;\n"
            "        float db = plane==0 ? b.position.z : plane==1 ? b.position.w-b.position.z : b.position.w-1e-6f;\n"
            "        if (da<0 && db<0) return;\n"
            "        if (da<0 || db<0) {\n"
            "            float t=da/(da-db); VSOut c=(VSOut)0;\n"
            "            c.position=lerp(a.position,b.position,t); c.color=lerp(a.color,b.color,t);\n"
            "            if (da<0) a=c; else b=c;\n"
            "        }\n"
            "    }\n"
            "    float2 delta=(b.position.xy/b.position.w-a.position.xy/a.position.w)/style.zw;\n"
            "    float len=length(delta); if (len<1e-6f) return;\n"
            "    float2 side=style.y>0 ? float2(-delta.y,delta.x)/len :\n"
            "        (abs(delta.x)>=abs(delta.y) ? float2(0,1) : float2(1,0));\n"
            "    uint layers=style.y>0 ? 4 : 2;\n"
            "    for (uint i=0; i<layers; ++i) {\n"
            "        float offset=style.y>0 ? (i==0 ? -(style.x+1)*0.5f : i==1 ? -abs(style.x-1)*0.5f :\n"
            "            i==2 ? abs(style.x-1)*0.5f : (style.x+1)*0.5f) : (i==0 ? -style.x*0.5f : style.x*0.5f);\n"
            "        float coverage=style.y>0 ? (i==0 || i==3 ? 0 : min(style.x,1)) : 1;\n"
            "        VSOut p=a; p.position.xy+=side*offset*style.zw*p.position.w; p.color.a*=coverage; output.Append(p);\n"
            "        p=b; p.position.xy+=side*offset*style.zw*p.position.w; p.color.a*=coverage; output.Append(p);\n"
            "    }\n"
            "}\n";
    }
    if (pipeline.scaled_depth_bias) {
        const auto entry = compiled_source.find("float4 ps_main(");
        if (entry == std::string::npos) return false;
        compiled_source.replace(entry, 15, "float4 ps_color(");
        compiled_source +=
            "\nstruct DepthOutput { float4 color : SV_TARGET; float depth : SV_Depth; };\n"
            "DepthOutput ps_main(VSOut input";
        if (pipeline.sample_depth_bias) compiled_source += ", uint sample_index : SV_SampleIndex";
        compiled_source +=
            ") {\n"
            "    DepthOutput output;\n"
            "    float2 gradient = float2(ddx(input.position.z), ddy(input.position.z));\n"
            "    float depth = input.position.z;\n";
        // SV_Position already includes viewport mapping and native constant bias.
        // Only depth moves to the sample; color/UV/discard retain pixel-center inputs.
        if (pipeline.sample_depth_bias) compiled_source +=
            "    depth += dot(gradient, GetRenderTargetSamplePosition(sample_index));\n";
        compiled_source +=
            "    float slope = max(abs(gradient.x) * raster_bias.x, abs(gradient.y) * raster_bias.y);\n"
            "    output.depth = saturate(depth + raster_bias.z * slope);\n"
            "    output.color = ps_color(input); return output;\n}\n";
    }
    return true;
}

bool recomp_d3d_draw_constants(
    const RecompD3dPresenterDrawCommand &draw, const RecompD3dVertexLayout &layout,
    const RecompD3dDrawTargetState &target, bool texture_bound, bool scaled_depth_bias,
    float *constants_out)
{
    const bool screen_offset = draw.screen_space_offset[0] != 0.0f || draw.screen_space_offset[1] != 0.0f;
    const bool geometry_line = recomp_d3d_draw_geometry_line(draw);
    const bool sub_viewport = draw.has_transform && !draw.program_count && draw.viewport[2] > 0.0f;
    std::fill(constants_out, constants_out + kRecompD3dDrawConstantFloats, 0.0f);
    /* Restore depth slope per guest pixel after anisotropic host scaling.
       Constant bias stays in the native D24 rasterizer, after clipping. */
    if (scaled_depth_bias) {
        constants_out[1420] = target.scale_x;
        constants_out[1421] = target.scale_y;
        constants_out[1422] = draw.slope_scaled_depth_bias;
    }
    if (draw.texture.volume_depth != 0u) {
        if (draw.fvf != 0x142u || draw.program_count || draw.has_reflection || draw.has_alpha_mask ||
            draw.stage1_alpha_mask || draw.bump_env || draw.distortion || draw.four_tap_filter ||
            draw.bright_pass || draw.texgen_reflection)
            return false;
        constants_out[140 + 192 * 4 + 510] = 1.0f;
    }
    std::memcpy(constants_out, draw.transform, sizeof draw.transform);
    std::memcpy(constants_out + 16, draw.blend_transforms, sizeof draw.blend_transforms);
    if (layout.pretransformed || draw.program_count || screen_offset) {
        Viewport viewport = hostViewport(target);
        /* Guest pixel coordinates stay in guest units on a scaled target. */
        viewport.TopLeftX /= target.scale_x;
        viewport.TopLeftY /= target.scale_y;
        viewport.Width /= target.scale_x;
        viewport.Height /= target.scale_y;
        if (viewport.Width <= 0.0f || viewport.Height <= 0.0f ||
            viewport.MaxDepth <= viewport.MinDepth) {
            return false;
        }
        if (layout.pretransformed || draw.program_count) {
            /* Undo the bound viewport; legacy pixel centers are integers, while
               D3D11 centers are half integers. The shader restores clip W from RHW. */
            std::memset(constants_out, 0, sizeof draw.transform);
            constants_out[0] = 2.0f / viewport.Width;
            constants_out[5] = -2.0f / viewport.Height;
            constants_out[10] = 1.0f / (viewport.MaxDepth - viewport.MinDepth);
            constants_out[12] = -1.0f + (0.5f - viewport.TopLeftX) * constants_out[0];
            constants_out[13] = 1.0f + (0.5f - viewport.TopLeftY) * constants_out[5];
            constants_out[14] = -viewport.MinDepth * constants_out[10];
            constants_out[15] = 1.0f;
        }
        if (screen_offset) {
            const float width = sub_viewport ? draw.viewport[2] : viewport.Width;
            const float height = sub_viewport ? draw.viewport[3] : viewport.Height;
            const float dx = 2.0f * draw.screen_space_offset[0] / width;
            const float dy = -2.0f * draw.screen_space_offset[1] / height;
            if (width <= 0.0f || height <= 0.0f || !std::isfinite(dx) || !std::isfinite(dy))
                return false;
            // Post-compose every WVP so the displacement survives perspective division and blending.
            for (unsigned row = 0; row < 64; row += 4) {
                constants_out[row] += dx * constants_out[row + 3];
                constants_out[row + 1] += dy * constants_out[row + 3];
            }
        }
    }
    if (draw.program_count) {
        std::memcpy(constants_out + 140, draw.program_constants, sizeof draw.program_constants);
        /* Undo the guest sample grid, which may differ from the host target. */
        for (unsigned axis = 0; axis < 2; ++axis) {
            const float scale = draw.program_constants[58][axis];
            if (!std::isfinite(scale) || scale == 0) return false;
            constants_out[axis * 5] = 1.0f / scale;
            constants_out[12 + axis] = -draw.program_constants[59][axis] / scale;
        }
        const float depth_scale = draw.program_constants[58][2];
        if (!std::isfinite(depth_scale) || depth_scale <= 0) return false;
        constants_out[10] = 1.0f / depth_scale;
        constants_out[14] = -draw.program_constants[59][2] / depth_scale;
        constants_out[138] = draw.program_alpha_mask ? 1.0f : 0.0f;
        constants_out[139] = draw.program_mask_lod_bias;
    }
    if (draw.directional.enabled && !draw.program_count) {
        const auto &light = draw.directional;
        if (light.count > 8u) return false;
        float *constants = constants_out + 140 + 192 * 4;
        std::memcpy(constants, light.normal_transforms, sizeof light.normal_transforms);
        std::memcpy(constants+64, light.ambient_emissive, sizeof light.ambient_emissive);
        std::memcpy(constants+68, light.material_diffuse, sizeof light.material_diffuse);
        std::memcpy(constants+72, light.directions, sizeof light.directions);
        std::memcpy(constants+104, light.colors, sizeof light.colors);
        constants[136]=1;
        constants[137]=light.normalize ? 1.0f:0.0f;
        constants[138]=static_cast<float>(light.count);
        constants[139]=light.stage_mode ? 1.0f:0.0f;
        std::memcpy(constants+140, light.world_transforms, sizeof light.world_transforms);
        std::memcpy(constants+204, light.positions, sizeof light.positions);
        std::memcpy(constants+236, light.attenuation, sizeof light.attenuation);
        std::memcpy(constants+268, light.ambient, sizeof light.ambient);
        std::memcpy(constants+300, light.specular, sizeof light.specular);
        std::memcpy(constants+332, light.view, sizeof light.view);
        std::memcpy(constants+336, light.stage, sizeof light.stage);
    } else if (draw.directional.stage_mode) {
        // Unlit stage-0 selection: input.color is the vertex diffuse or a program's oD0.
        float *constants = constants_out + 140 + 192 * 4;
        constants[139] = 1.0f;
        std::memcpy(constants + 336, draw.directional.stage, sizeof draw.directional.stage);
    }
    constants_out[64] = texture_bound ? 1.0f : 0.0f;
    constants_out[65] = draw.depth.alpha_test_enable ? 1.0f : 0.0f;
    constants_out[66] = static_cast<float>(draw.depth.alpha_func);
    constants_out[67] = static_cast<float>(draw.depth.alpha_ref);
    constants_out[68] = draw.blend_weight_count != 0u ? 1.0f : 0.0f;
    constants_out[69] = draw.use_texture_factor ? 1.0f : draw.texture_factor_modulate ? 5.0f
        : draw.modulate_texture_factor_alpha ? 4.0f
        : draw.modulate_texture_factor ? 2.0f : draw.texture_factor_color_scale == 4u ? 6.0f
        : draw.texture_factor_color_scale == 2u ? 3.0f : 0.0f;
    constants_out[70] = draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE
        ? 1.0f : draw.material_alpha;
    constants_out[71] = draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE
        ? 3.0f : draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE
        ? 2.0f : draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE
        ? 1.0f : 0.0f;
    constants_out[72] = ((draw.texture_factor >> 16u) & 0xffu) / 255.0f;
    constants_out[73] = ((draw.texture_factor >> 8u) & 0xffu) / 255.0f;
    constants_out[74] = (draw.texture_factor & 0xffu) / 255.0f;
    constants_out[75] = ((draw.texture_factor >> 24u) & 0xffu) / 255.0f;
    constants_out[76] = draw.zero_diffuse_rgb ? 1.0f : 0.0f;
    /* NV2A A8 supplies white RGB; DXGI A8 supplies only alpha. */
    constants_out[77] = draw.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 ||
        draw.texture_alpha_only ? 1.0f : 0.0f;
    constants_out[80] = draw.texture.linear && draw.texture.width != 0u
        ? 1.0f / draw.texture.width : 1.0f;
    constants_out[81] = draw.texture.linear && draw.texture.height != 0u
        ? 1.0f / draw.texture.height : 1.0f;
    constants_out[82] = draw.four_tap_filter ? 1.0f : 0.0f;
    constants_out[78] = draw.alpha_mask.linear && draw.alpha_mask.width
        ? 1.0f / draw.alpha_mask.width : 1.0f;
    constants_out[79] = draw.alpha_mask.linear && draw.alpha_mask.height
        ? 1.0f / draw.alpha_mask.height : 1.0f;
    constants_out[83] = draw.has_alpha_mask ? 1.0f : 0.0f;
    {
        float *filter = constants_out + 140 + 192 * 4 + 340;
        const auto unpack = [](uint32_t argb, float *rgba) {
            rgba[0] = ((argb >> 16u) & 0xffu) / 255.0f;
            rgba[1] = ((argb >> 8u) & 0xffu) / 255.0f;
            rgba[2] = (argb & 0xffu) / 255.0f;
            rgba[3] = (argb >> 24u) / 255.0f;
        };
        if (draw.bright_pass) {
            filter[0] = 1.0f;
            filter[4] = 1.0f / draw.texture.width;
            filter[5] = 1.0f / draw.texture.height;
            unpack(draw.bright_pass_threshold, filter + 8);
        }
        if (draw.luminance_tint) {
            filter[0] = 2.0f;
            unpack(draw.luminance_tint_factors[0], filter + 8);
            unpack(draw.luminance_tint_factors[1], filter + 12);
        }
        filter[3] = draw.alpha_kill ? 1.0f : 0.0f;
        filter[6] = draw.texgen_reflection ? 1.0f : draw.texture_transform_uv ? 2.0f
            : draw.texgen_projected ? 3.0f : 0.0f;
        filter[7] = draw.distortion ? 1.0f : 0.0f;
        if (draw.four_tap_filter) {
            filter[1] = draw.four_tap_scale;
            filter[2] = draw.four_tap_sum_scale;
            for (unsigned i = 0u; i < 4u; ++i) unpack(draw.four_tap_factors[i], filter + 8 + i * 4);
        }
    }
    if (draw.has_reflection || draw.texgen_reflection || draw.texture_transform_uv || draw.texgen_projected) {
        std::memcpy(constants_out + 84, draw.reflection_world_view, 64u);
        std::memcpy(constants_out + 100, draw.reflection_normal, 64u);
        std::memcpy(constants_out + 116, draw.reflection_transform, 64u);
        constants_out[137] = draw.reflection_normalize ? 1.0f : 0.0f;
        std::memcpy(constants_out + 140 + 192 * 4 + 396, draw.reflection_blend_world_view,
            sizeof draw.reflection_blend_world_view);
        std::memcpy(constants_out + 140 + 192 * 4 + 444, draw.reflection_blend_normal,
            sizeof draw.reflection_blend_normal);
    }
    if (draw.has_reflection) {
        constants_out[140 + 192 * 4 + 509] = draw.reflection_alpha_kill ? 1.0f : 0.0f;
        std::memcpy(constants_out + 132, draw.reflection_diffuse, 16u);
        constants_out[136] = draw.reflection_mesh_uv ? 2.0f : 1.0f;
        constants_out[139] = (draw.reflection_premultiplied ? 1.0f : 0.0f) +
            (draw.reflection_skip_diffuse ? 2.0f : 0.0f) + (draw.reflection_color_only ? 4.0f : 0.0f);
        if (draw.texgen_reflection && !draw.reflection_mesh_uv)
            std::memcpy(constants_out + 140 + 192 * 4 + 364, draw.reflection_stage1_transform, 64u);
    }
    if (draw.reflection_stage2) {
        std::memcpy(constants_out + 140 + 192 * 4 + 492, draw.reflection_stage2_transform, 64u);
        constants_out[140 + 192 * 4 + 508] = 1.0f;
        constants_out[140 + 192 * 4 + 511] = draw.reflection_stage2_mesh_uv ? 1.0f : 0.0f;
    }
    if (draw.projected_stage_count != 0u) {
        float *projected = constants_out + 140 + 192 * 4 + 516;
        std::memcpy(projected, draw.projected_transforms, sizeof draw.projected_transforms);
        projected[48] = static_cast<float>(draw.projected_stage_count);
        for (unsigned i = 0u; i < 3u; ++i) projected[52 + i] = static_cast<float>(draw.projected_slots[i]);
    }
    if (draw.stage1_alpha_mask)
        constants_out[138] = draw.stage1_kill_only ? 3.0f : draw.stage1_alpha_kill ? 2.0f : 1.0f;
    if (draw.bump_env) {
        float *bump = constants_out + 140 + 192 * 4 + 380;
        std::memcpy(bump, draw.bump_matrices, sizeof draw.bump_matrices);
        std::memcpy(bump + 8, draw.bump_fog, sizeof draw.bump_fog);
        bump[11] = draw.bump_water ? 2.0f : 1.0f;
        bump[12] = ((draw.fog_color >> 16u) & 0xffu) / 255.0f;
        bump[13] = ((draw.fog_color >> 8u) & 0xffu) / 255.0f;
        bump[14] = (draw.fog_color & 0xffu) / 255.0f;
    }

    if (geometry_line) {
        const Viewport viewport = hostViewport(target);
        if (viewport.Width <= 0 || viewport.Height <= 0)
            return false;
        float *style = constants_out + 140 + 192 * 4 + 340 + 8;
        style[0] = draw.line_width;
        style[1] = draw.line_antialias ? 1.0f : 0.0f;
        style[2] = sub_viewport ? 2.0f / draw.viewport[2]
            : 2.0f * target.scale_x / viewport.Width;
        style[3] = sub_viewport ? -2.0f / draw.viewport[3]
            : -2.0f * target.scale_y / viewport.Height;
    }
    return true;
}
