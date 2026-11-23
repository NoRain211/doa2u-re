// SPDX-License-Identifier: GPL-3.0-or-later
/* The output pass both presenters run on the finished picture, after SMAA:
   AMD FidelityFX CAS sharpening, then the game's gamma ramp. The filter
   follows CasFilter in ffx_cas.h (GPUOpen-Effects/FidelityFX-CAS 9fabcc9,
   MIT, licenses/FidelityFX-CAS.txt) without scaling and with
   CAS_BETTER_DIAGONALS. */
#ifndef DOA2U_RECOMP_D3D_CAS_H
#define DOA2U_RECOMP_D3D_CAS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

/* HLSL for float4 outputPixel(float4 position). It needs Texture2D pixels and
   the cbuffer RECOMP_OUTPUT_PASS_FIELDS lays out as RecompOutputPass. */
#define RECOMP_OUTPUT_PASS_FIELDS "uint4 ramp[64]; float4 output_pass;"
#define RECOMP_OUTPUT_PASS_HLSL \
    "float3 casLoad(int2 p, int2 offset, int2 last) {\n" \
    " return pixels.Load(int3(clamp(p + offset, int2(0, 0), last), 0)).rgb; }\n" \
    "float rampValue(uint index, uint shift) {\n" \
    " return float((ramp[index >> 2][index & 3u] >> shift) & 255u) / 255.0; }\n" \
    "float4 outputPixel(float4 position) {\n" \
    " int2 p = int2(position.xy);\n" \
    " float4 c = pixels.Load(int3(p, 0));\n" \
    " if (output_pass.x != 0.0) {\n" \
    "  int2 last = int2(output_pass.zw);\n" \
    "  float3 a = casLoad(p, int2(-1, -1), last), b = casLoad(p, int2(0, -1), last);\n" \
    "  float3 cc = casLoad(p, int2(1, -1), last), d = casLoad(p, int2(-1, 0), last);\n" \
    "  float3 f = casLoad(p, int2(1, 0), last), g = casLoad(p, int2(-1, 1), last);\n" \
    "  float3 hh = casLoad(p, int2(0, 1), last), i = casLoad(p, int2(1, 1), last);\n" \
    "  float3 mn = min(min(min(d, c.rgb), min(f, b)), hh);\n" \
    "  float3 mx = max(max(max(d, c.rgb), max(f, b)), hh);\n" \
    "  mn += min(min(min(mn, a), min(cc, g)), i);\n" \
    "  mx += max(max(max(mx, a), max(cc, g)), i);\n" \
    "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1.0e-5)));\n" \
    "  float3 weight = amp * output_pass.x;\n" \
    "  c.rgb = saturate(((b + d + f + hh) * weight + c.rgb) / (1.0 + 4.0 * weight));\n" \
    " }\n" \
    " if (output_pass.y != 0.0) {\n" \
    "  uint3 index = (uint3)(saturate(c.rgb) * 255 + 0.5);\n" \
    "  c.rgb = float3(rampValue(index.r, 0u), rampValue(index.g, 8u), rampValue(index.b, 16u));\n" \
    " }\n" \
    " return c; }\n"

/* The output pass's constants. The ramp packs one 0x00BBGGRR word per entry:
   SDL_GPU's Vulkan backend binds only 4 KiB of a uniform buffer. pass holds
   CAS's peak (0 for off), 1 to apply the ramp, and the picture's last column
   and row. */
struct RecompOutputPass {
    uint32_t ramp[256];
    float pass[4];
};

/* CAS's peak weight for a sharpness from 0 to 1 (CasSetup); 0 turns it off. */
inline float recomp_cas_peak(float sharpness)
{
    if (!(sharpness > 0.0f)) return 0.0f;
    return -1.0f / (8.0f - 3.0f * (std::min)(sharpness, 1.0f));
}

/* ramp holds each channel's 0-1 value per entry, as the presenters keep it. */
inline RecompOutputPass recomp_output_pass(const float (&ramp)[256][4], bool gamma, float sharpness,
    uint32_t width, uint32_t height)
{
    RecompOutputPass constants{};
    for (unsigned i = 0u; i < 256u; ++i) {
        for (unsigned channel = 0u; channel < 3u; ++channel) {
            const float value = std::clamp(ramp[i][channel], 0.0f, 1.0f) * 255.0f + 0.5f;
            constants.ramp[i] |= static_cast<uint32_t>(value) << (channel * 8u);
        }
    }
    constants.pass[0] = recomp_cas_peak(sharpness);
    constants.pass[1] = gamma ? 1.0f : 0.0f;
    constants.pass[2] = static_cast<float>(width) - 1.0f;
    constants.pass[3] = static_cast<float>(height) - 1.0f;
    return constants;
}

/* RECOMP_D3D_SHARPEN: CAS sharpness from 0 to 1; unset or 0 is off. */
inline float recomp_cas_env_sharpness()
{
    const char *value = std::getenv("RECOMP_D3D_SHARPEN");
    const float sharpness = value != nullptr ? std::strtof(value, nullptr) : 0.0f;
    return std::isfinite(sharpness) ? std::clamp(sharpness, 0.0f, 1.0f) : 0.0f;
}

#endif
