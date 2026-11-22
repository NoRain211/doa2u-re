#ifndef DOA2U_RECOMP_D3D_FRAMEGEN_SHADERS_H
#define DOA2U_RECOMP_D3D_FRAMEGEN_SHADERS_H

#include <cstdint>
#include <initializer_list>
#include <string>

/* Frame generation's HLSL. The SDL_GPU presenter compiles it with shaderc,
   to SPIR-V and Metal; the vk = false form is for a planned Direct3D 11 port
   (D3DCompile). Shaderc needs [[vk::binding]] attributes to place each
   texture and constant buffer where SDL_GPU binds it; FXC cannot parse them,
   and its registers (t0.., s0.., b0) already name the same slots, so that
   form leaves the attributes out. Every pass draws one fullscreen triangle
   (vs_main of blendShader) and reads its inputs with Load, the warp's
   pictures with SampleLevel; Flow.info and Flow.weights vary by pass.
   These are fragment shaders within Shader Model 4, so Direct3D 11 runs them
   at feature level 10. */

namespace recomp_framegen {

enum FlowPipeline {
    kFlowLuma, kFlowCoarse, kFlowSearch, kFlowConfidence, kFlowCutBlocks, kFlowCut, kFlowSelect, kFlowWarp, kFlowView
};

constexpr int kFlowCoarseRadius = 4, kFlowRadius = 2;  // search window, in pixels of the level
constexpr unsigned kFlowLevels = 4u;  // the luminance pyramid, finest first

struct FlowUniforms {
    int32_t info[4];
    float weights[4];
};

/* Frame generation's crossfade blends the stored UNORM values before gamma,
   the space the game's own pixels and alpha blends live in, so a generated
   frame matches what the game would draw for a fade between the two. */
inline std::string blendShader(bool vk)
{
    const auto binding = [vk](unsigned slot, const char *set) {
        return vk ? std::string("[[vk::binding(") + std::to_string(slot) + ", " + set + ")]] " : std::string();
    };
    return binding(0u, "2") + "Texture2D previous : register(t0);\n" +
        binding(0u, "2") + "SamplerState previous_sampler : register(s0);\n" +
        binding(1u, "2") + "Texture2D current : register(t1);\n" +
        binding(1u, "2") + "SamplerState current_sampler : register(s1);\n" +
        binding(0u, "RECOMP_UNIFORM_SET") + "cbuffer Blend : register(b0) { float4 weight; };\n" +
        "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
        " float2 p = float2((id << 1) & 2, id & 2);\n"
        " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n"
        "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
        " int3 at = int3(p.xy, 0);\n"
        " return lerp(previous.Load(at), current.Load(at), weight.x); }\n";
}

// Declares each texture with the sampler the presenter binds beside it, in slot order.
inline std::string flowShader(bool vk, std::initializer_list<const char *> textures, const char *body)
{
    const auto binding = [vk](unsigned slot, const char *set) {
        return vk ? std::string("[[vk::binding(") + std::to_string(slot) + ", " + set + ")]] " : std::string();
    };
    std::string hlsl = binding(0u, "RECOMP_UNIFORM_SET") +
        "cbuffer Flow : register(b0) { int4 info; float4 weights; };\n";
    unsigned slot = 0u;
    for (const char *name : textures) {
        const std::string at = std::to_string(slot);
        hlsl += binding(slot, "2") + "Texture2D " + name + " : register(t" + at + ");\n" +
            binding(slot, "2") + "SamplerState " + name + "_sampler : register(s" + at + ");\n";
        ++slot;
    }
    return hlsl + body;
}

/* Luminance of the picture (info.x = 1 to 4, a box of that size per texel,
   weights = Rec. 601) or of the level below (info.x = 2, weights = red). */
inline const char kFlowLumaShader[] =
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " int2 origin = int2(p.xy) * info.x;\n"
    " float sum = 0;\n"
    " for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)\n"
    "  if (x < info.x && y < info.x) sum += dot(picture.Load(int3(origin + int2(x, y), 0)).rgb, weights.rgb);\n"
    " return (sum / (info.x * info.x)).xxxx; }\n";

/* One texel per 8x8 block of source: the motion to its best match in target
   within RADIUS of the prediction (PREDICT: twice the vector of the coarser
   level's block, else zero), refined to a fraction by an equiangular fit of the
   SADs beside it. Out: xy = motion in pixels of this level, z = the match's
   mean absolute difference, w = how much the window's mean exceeds it (zero
   when nothing in the window tells candidates apart). info.xy = level size,
   info.zw = coarser field size. */
inline const char kFlowSearchShader[] =
    "float sad(float own[64], int2 at, int2 last) {\n"
    " float sum = 0;\n"
    " [unroll] for (int i = 0; i < 64; ++i)\n"
    "  sum += abs(own[i] - target.Load(int3(clamp(at + int2(i & 7, i >> 3), 0, last), 0)).r);\n"
    " return sum; }\n"
    "float fit(float a, float b, float c) {\n"
    " float slope = max(a, c) - b;\n"
    " return slope > 0 ? clamp(0.5 * (a - c) / slope, -0.5, 0.5) : 0; }\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " int2 block = int2(p.xy), origin = block * 8, last = info.xy - 1;\n"
    " float own[64];\n"
    " [unroll] for (int i = 0; i < 64; ++i) own[i] = source.Load(int3(min(origin + int2(i & 7, i >> 3), last), 0)).r;\n"
    " int2 center = int2(0, 0);\n"
    "#if PREDICT\n"
    " center = int2(round(coarse.Load(int3(min(block >> 1, info.zw - 1), 0)).xy * 2));\n"
    "#endif\n"
    " int2 at = center;\n"
    " float best = 1e30, best_cost = 1e30, total = 0;\n"
    " [loop] for (int y = -RADIUS; y <= RADIUS; ++y) [loop] for (int x = -RADIUS; x <= RADIUS; ++x) {\n"
    "  float sum = sad(own, origin + center + int2(x, y), last);\n"
    "  float cost = sum + 1e-5 * (x * x + y * y);\n"  // exact ties go to the prediction
    "  total += sum;\n"
    "  if (cost < best_cost) { best_cost = cost; best = sum; at = center + int2(x, y); } }\n"
    " float2 sub = float2(\n"
    "  fit(sad(own, origin + at - int2(1, 0), last), best, sad(own, origin + at + int2(1, 0), last)),\n"
    "  fit(sad(own, origin + at - int2(0, 1), last), best, sad(own, origin + at + int2(0, 1), last)));\n"
    " float count = (2 * RADIUS + 1) * (2 * RADIUS + 1);\n"
    " return float4(float2(at) + sub, best / 64, max(total / count - best, 0) / 64); }\n";

/* The finest search of one direction, checked against the other direction's
   vector where the block lands, and doubted as much as the HUD mask covers
   the block in its own frame: the still 2D layer must not pull the scene
   around it toward zero. Out: xy = motion in main target pixels (weights.x
   per finest-level pixel), z = confidence 0..1, w = the match's mean
   absolute difference. info.xy = field size, weights.y = main target pixels
   per block, weights.zw = 1 / main target size. */
inline const char kFlowConfidenceShader[] =
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " int2 block = int2(p.xy);\n"
    " float scene = 0;\n"
    " [unroll] for (int i = 0; i < 16; ++i)\n"
    "  scene += cover.SampleLevel(cover_sampler, (block + (float2(i & 3, i >> 2) + 0.5) / 4) * weights.y * weights.zw, 0).r;\n"
    " float4 own = field.Load(int3(block, 0));\n"
    " float2 landed = (float2(block) + 0.5) * 8 + own.xy;\n"
    " float2 back = other.Load(int3(clamp(int2(floor(landed / 8)), 0, info.xy - 1), 0)).xy;\n"
    " float consistent = saturate(1 - length(own.xy + back) / 2);\n"  // 0 at 2 pixels apart
    " float matched = saturate(1 - own.z / 0.08);\n"                  // 0 at 8% mean difference
    " float distinct = saturate(own.w / 0.02);\n"                     // 0 in a flat window
    " return float4(own.xy * weights.x, consistent * matched * distinct * scene / 16, own.z); }\n";

/* Cut detection, per 8x8 block of the coarsest luminance level: the best
   normalized correlation of N-1's block with N within 4 pixels. A fade or a
   flash scales and offsets the luminance, which correlation ignores, so they
   still match; the flow's absolute differences do not. A block votes as much
   as neither HUD mask covers it, and not at all where either frame is flat
   there (a solid fade or flash tells nothing). Out: x = vote, y = vote if it
   matched, z = the best correlation. info.xy = level size, weights.y = main
   target pixels per block, weights.zw = 1 / main target size. */
inline const char kFlowCutBlocksShader[] =
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " int2 block = int2(p.xy), origin = block * 8, last = info.xy - 1;\n"
    " float own[64], mean = 0, spread = 0, best = -1, here = 0, scene = 0;\n"
    " [unroll] for (int i = 0; i < 64; ++i) {\n"
    "  own[i] = previous.Load(int3(min(origin + int2(i & 7, i >> 3), last), 0)).r;\n"
    "  mean += own[i] / 64; }\n"
    " [unroll] for (int j = 0; j < 64; ++j) { own[j] -= mean; spread += own[j] * own[j]; }\n"
    " [loop] for (int y = -4; y <= 4; ++y) [loop] for (int x = -4; x <= 4; ++x) {\n"
    "  float sum = 0, square = 0, cross = 0;\n"
    "  [unroll] for (int i = 0; i < 64; ++i) {\n"
    "   float b = current.Load(int3(clamp(origin + int2(x, y) + int2(i & 7, i >> 3), 0, last), 0)).r - mean;\n"
    "   sum += b; square += b * b; cross += own[i] * b; }\n"
    "  float variance = max(square - sum * sum / 64, 0);\n"
    "  if (x == 0 && y == 0) here = variance;\n"
    "  best = max(best, cross * rsqrt(max(spread * variance, 1e-12))); }\n"
    " [unroll] for (int k = 0; k < 16; ++k) {\n"
    "  float2 at = (block + (float2(k & 3, k >> 2) + 0.5) / 4) * weights.y * weights.zw;\n"
    "  scene += min(previous_cover.SampleLevel(previous_cover_sampler, at, 0).r,\n"
    "   current_cover.SampleLevel(current_cover_sampler, at, 0).r) / 16; }\n"
    " float vote = min(spread, here) > 64 * 0.01 * 0.01 ? scene : 0;\n"  // flat: deviation under 1%
    " return float4(vote, best >= 0.7 ? vote : 0, best, 0); }\n";

/* Whether real frames N-1 and N are a cut: at least a quarter of the blocks
   voted (kFlowCutBlocksShader), under 30% of the votes matched, and the pair
   before (earlier, its w) matched. Fast camera moves and close-up fights
   beyond the search's reach also fail to match, but for several pairs in a
   row; a cut is one pair between steady ones. Out, one texel: x = 1 at a
   cut, y = the share that voted, z = the share of votes that matched,
   w = 1 when under half matched. info.xy = block count. */
inline const char kFlowCutShader[] =
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " float2 sum = 0;\n"
    " [loop] for (int y = 0; y < info.y; ++y) [loop] for (int x = 0; x < info.x; ++x)\n"
    "  sum += blocks.Load(int3(x, y, 0)).xy;\n"
    " float voted = sum.x / (info.x * info.y), matched = sum.y / max(sum.x, 1e-4);\n"
    " bool unsteady = earlier.Load(int3(0, 0, 0)).w > 0.5;\n"
    " return float4(voted >= 0.25 && matched < 0.3 && !unsteady ? 1 : 0, voted, matched,\n"
    "  voted >= 0.25 && matched < 0.5 ? 1 : 0); }\n";

/* The motion each finest-level pixel of the picture t of the way from N-1 to
   N follows: zero, or the forward or reversed backward vector of one of the
   2x2 blocks nearest it, whichever brings 3x3 luminance patches of N-1 (back
   by t along it) and N (on by 1 - t) closest. A doubtful vector pays for its
   doubt, and one far from the confident vectors around it for straying, so
   lone picks do not speckle; zero wins ties, so still overlays stay still.
   A pick that still strays is not trusted: a pocket of zero or of another
   vector inside a confidently moving surface would show as a hard-edged
   patch. Out: xy = motion in main target pixels times trust, z = trust: the
   fields' confidence near the pixel (none below 0.2) times the patches'
   agreement times how close the pick is to the confident vectors around it
   (none 3 finest-level pixels away), w = the patches' mean difference.
   info.xy = field size, weights.x = t, weights.y = main target pixels per
   finest-level pixel, weights.zw = 1 / level size. */
inline const char kFlowSelectShader[] =
    "float patch(float2 at, float2 v) {\n"
    " float sum = 0;\n"
    " [unroll] for (int i = 0; i < 9; ++i) {\n"
    "  float2 o = at + float2(i % 3, i / 3) - 1;\n"
    "  sum += abs(previous.SampleLevel(previous_sampler, (o - weights.x * v) * weights.zw, 0).r -\n"
    "   current.SampleLevel(current_sampler, (o + (1 - weights.x) * v) * weights.zw, 0).r); }\n"
    " return sum / 9; }\n"
    "float stray(float2 v, float2 around) { return 0.03 * saturate(length(v - around) / (4 * weights.y)); }\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " float2 q = p.xy / 8 - 0.5, f = q - floor(q), confidence = 0, motion = 0;\n"
    " float4 candidates[8];\n"
    " float3 sum = 0;\n"
    " [unroll] for (int i = 0; i < 8; ++i) {\n"
    "  int2 corner = int2(i & 1, i >> 1 & 1), at = clamp(int2(floor(q)) + corner, 0, info.xy - 1);\n"
    "  float4 v = i < 4 ? forward.Load(int3(at, 0)) : backward.Load(int3(at, 0)) * float4(-1, -1, 1, 1);\n"
    "  float2 near = lerp(1 - f, f, float2(corner));\n"
    "  confidence[i >> 2] += near.x * near.y * v.z;\n"
    "  sum += float3(v.xy, 1) * near.x * near.y * v.z;\n"
    "  candidates[i] = v; }\n"
    " float2 around = sum.xy / max(sum.z, 1e-4);\n"  // the confident vectors' mean
    " float best = patch(p.xy, 0), best_cost = best + stray(0, around);\n"
    " [unroll] for (int j = 0; j < 8; ++j) {\n"
    "  float4 v = candidates[j];\n"
    "  float error = patch(p.xy, v.xy / weights.y);\n"
    "  float cost = error + 0.05 * (1 - v.z) + stray(v.xy, around) + 1.0 / 255;\n"
    "  if (cost < best_cost) { best_cost = cost; best = error; motion = v.xy; } }\n"
    " float agree = saturate((0.15 - best) / 0.1);\n"                               // none at 15% mean difference
    " float sure = saturate((max(confidence.x, confidence.y) - 0.2) / 0.6);\n"  // none below 0.2
    " float steady = saturate(1.5 - 0.5 * length(motion - around) / weights.y);\n"
    " float trust = sure * agree * steady;\n"
    " return float4(motion * trust, trust, best); }\n";

/* The generated picture t of the way from N-1 to N: N-1 sampled back by t
   along the motion (kFlowSelectShader) and N on by 1 - t, weighted by
   temporal distance and faded toward the crossfade as trust drops. Zero
   motion is the crossfade itself. The motion is sampled bilinearly and
   weighted by trust, so motion and trust change smoothly between
   finest-level pixels instead of in steps of them.
   The HUD mask (framegenCover) keeps the 2D layer still and out of the
   scene: a pixel the 2D layer covers in either frame (a quarter of the
   scene hidden counts as covered) shows the nearer real frame, so a timer
   or counter that changes stays one sharp number, and a sample that
   reads any of it in its own frame gives way to the other frame's, or with
   neither, to the crossfade. The mask sampled bilinearly where a picture is
   sampled is one less the share of 2D layer in that sample, so a steep
   threshold leaves under 1/256 of it. At a cut (kFlowCutShader) the picture
   is N-1 with its HUD, so the cut lands with real frame N, as the game shows
   it. info.xy = finest level size, weights.x = t, weights.y = main target
   pixels per finest-level pixel, weights.zw = 1 / main target size. */
inline const char kFlowWarpShader[] =
    "float covered(float scene, float steep) { return saturate(steep - steep * scene); }\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " if (cut.Load(int3(0, 0, 0)).r > 0.5) return previous.Load(int3(p.xy, 0));\n"
    " float t = weights.x;\n"
    " float4 m = motion.SampleLevel(motion_sampler, p.xy / weights.y / float2(info.xy), 0);\n"
    " m.xy /= max(m.z, 1e-3);\n"
    " float4 earlier = previous.Load(int3(p.xy, 0)), later = current.Load(int3(p.xy, 0));\n"
    " float4 crossfade = lerp(earlier, later, t);\n"
    " float2 from = p.xy - t * m.xy, to = p.xy + (1 - t) * m.xy;\n"
    " float2 hud = float2(covered(previous_cover.SampleLevel(motion_sampler, from * weights.zw, 0).r, 64),\n"
    "  covered(current_cover.SampleLevel(motion_sampler, to * weights.zw, 0).r, 64));\n"
    " float2 w = float2(1 - t, t) * (1 - hud);\n"
    " float4 warped = (w.x * previous.SampleLevel(previous_sampler, from * weights.zw, 0) +\n"
    "  w.y * current.SampleLevel(current_sampler, to * weights.zw, 0)) / max(w.x + w.y, 1e-4);\n"
    " float4 scene = lerp(crossfade, warped, m.z * (1 - hud.x * hud.y));\n"
    " float here = min(previous_cover.SampleLevel(motion_sampler, p.xy * weights.zw, 0).r,\n"
    "  current_cover.SampleLevel(motion_sampler, p.xy * weights.zw, 0).r);\n"
    " return lerp(scene, t < 0.5 ? earlier : later, covered(here, 4)); }\n";

/* RECOMP_D3D_FRAMEGEN=flow: a field as color, hue = direction, brightness =
   motion up to weights.y main target pixels, dark = low confidence, and the
   HUD mask of N whitened, all of it tinted red at a cut. weights.x = main
   target pixels per block, weights.zw = 1 / main target size, info.xy = field
   size. */
inline const char kFlowViewShader[] =
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " float4 flow = field.Load(int3(min(int2(p.xy / weights.x), info.xy - 1), 0));\n"
    " float3 hue = saturate(abs(frac(atan2(flow.y, flow.x) / 6.2831853 + float3(1, 2.0 / 3, 1.0 / 3)) * 6 - 3) - 1);\n"
    " float magnitude = saturate(length(flow.xy) / weights.y);\n"
    " float3 shown = lerp((0.15).xxx, hue, magnitude) * lerp(0.1, 1, flow.z);\n"
    " shown = lerp(shown, (1).xxx, 0.6 - 0.6 * cover.SampleLevel(cover_sampler, p.xy * weights.zw, 0).r);\n"
    " return float4(lerp(shown, float3(1, 0, 0), 0.5 * cut.Load(int3(0, 0, 0)).r), 1); }\n";

} // namespace recomp_framegen

#endif
