/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_draw_adapter.h"
#include "d3d8_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d_device_model.h"
#include "d3d_draw_model.h"
#include "d3d_light_model.h"
#include "d3d_vertex_shader_model.h"
#include "kernel_abi.h"
#include "stop_report.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 5849 API layout verified from instructions in the selected DOA2 image. */
enum {
    DIRTY = 0x355058u, STAGES = 0x355060u, PIXEL_SHADOW = 0x355260u,
    STREAMS = 0x3579a8u, PIXEL_HANDLE = 0x784u,
    PIXEL_DEFINITION_SIZE = 0xf0u,
};

static uint32_t definition(uint32_t handle)
{
    if (handle == 0u || *recomp_memory_u32(handle) == 0u)
        recomp_stop(2, "d3d8:invalid-pixel-shader:%08x", handle);
    const uint32_t data = *recomp_memory_u32(handle + 8u);
    if (data == 0u) recomp_stop(2, "d3d8:invalid-pixel-definition");
    recomp_memory(data, PIXEL_DEFINITION_SIZE);
    return data;
}

static void pixel_create(void)
{
    const uint32_t source = kernel_arg(1u), output = kernel_arg(2u);
    if (source == 0u || output == 0u) recomp_stop(2, "d3d8:pixel-create-pointer");
    recomp_memory(source, PIXEL_DEFINITION_SIZE);
    recomp_memory(output, 4u);
    const uint32_t handle = recomp_kernel_allocate_pool(0xfcu);
    if (handle == 0u) { kernel_return(2u, RECOMP_D3D_OUT_OF_MEMORY); return; }
    *recomp_memory_u32(handle) = 1u;
    *recomp_memory_u32(handle + 4u) = 1u;
    *recomp_memory_u32(handle + 8u) = handle + 12u;
    recomp_guest_memcpy(handle + 12u, source, PIXEL_DEFINITION_SIZE);
    *recomp_memory_u32(output) = handle;
    kernel_return(2u, 0u);
}

static void pixel_delete(void)
{
    const uint32_t handle = kernel_arg(1u);
    definition(handle);
    if (--*recomp_memory_u32(handle) == 0u && *recomp_memory_u32(handle + 4u) != 0u)
        recomp_kernel_free_pool(handle);
    recomp_runtime.registers.esp += 8u;
}

static void pixel_get(void)
{
    const uint32_t device = recomp_d3d8_require_device(), output = kernel_arg(1u);
    if (output == 0u) recomp_stop(2, "d3d8:pixel-get-pointer");
    *recomp_memory_u32(output) = *recomp_memory_u32(device + PIXEL_HANDLE);
    recomp_runtime.registers.esp += 8u;
}

static void pixel_method(uint32_t method, uint32_t value)
{
    if (!recomp_d3d8_state_set_simple(method, value))
        recomp_stop(2, "d3d8:pixel-method:%08x:%08x", method, value);
}

static void pixel_set(void)
{
    const uint32_t device = recomp_d3d8_require_device(), handle = kernel_arg(1u);
    if (handle == 0u) {
        *recomp_memory_u32(DIRTY) |= 0x4800u;
        if (*recomp_memory_u32(device + 0x788u) != 0u) *recomp_memory_u32(DIRTY) |= 0x2000u;
    } else {
        const uint32_t data = definition(handle);
        const uint32_t inputs = *recomp_memory_u32(data + 0x20u) | *recomp_memory_u32(data + 0x24u);
        if (inputs == 0u && *recomp_memory_u32(device + 0x788u) != 0u)
            *recomp_memory_u32(DIRTY) |= 0x2000u;
        *recomp_memory_u32(device + 0x788u) = inputs;
        *recomp_memory_u32(device + 0x78cu) = *recomp_memory_u32(data + 0xecu) & 0x100u;
        *recomp_memory_u32(device + 0x790u) = *recomp_memory_u32(data + 0xd8u);
        *recomp_memory_u32(DIRTY) |= 0x4000u;
        if ((*recomp_memory_u32(device + 8u) & 0x10u) == 0u) {
            recomp_guest_memcpy(PIXEL_SHADOW, data, 0xe4u);
            *recomp_memory_u32(0x355480u) = *recomp_memory_u32(data + 0xd8u);
        }
        for (uint32_t i = 0u; i < 8u; ++i) {
            pixel_method(0x40260u + i * 4u, *recomp_memory_u32(data + i * 4u));
            pixel_method(0x40a60u + i * 4u, *recomp_memory_u32(data + 0x28u + i * 4u));
            pixel_method(0x40a80u + i * 4u, *recomp_memory_u32(data + 0x48u + i * 4u));
            pixel_method(0x40aa0u + i * 4u, *recomp_memory_u32(data + 0x68u + i * 4u));
            pixel_method(0x40ac0u + i * 4u, *recomp_memory_u32(data + 0x88u + i * 4u));
            pixel_method(0x41e40u + i * 4u, *recomp_memory_u32(data + 0xb4u + i * 4u));
        }
        pixel_method(0x417f8u, *recomp_memory_u32(data + 0xa8u));
        pixel_method(0x41e20u, *recomp_memory_u32(data + 0xacu));
        pixel_method(0x41e24u, *recomp_memory_u32(data + 0xb0u));
        pixel_method(0x41e60u, *recomp_memory_u32(data + 0xd4u));
        pixel_method(0x41e74u, *recomp_memory_u32(data + 0xdcu));
        pixel_method(0x41e78u, *recomp_memory_u32(data + 0xe0u));
        if (inputs != 0u) {
            pixel_method(0x40288u, *recomp_memory_u32(data + 0x20u));
            pixel_method(0x4028cu, *recomp_memory_u32(data + 0x24u));
        }
    }
    *recomp_memory_u32(device + PIXEL_HANDLE) = handle;
    recomp_runtime.registers.esp += 8u;
}

static void pixel_constant(void)
{
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t data = definition(*recomp_memory_u32(device + PIXEL_HANDLE));
    const uint32_t first = kernel_arg(1u), source = kernel_arg(2u), count = kernel_arg(3u);
    if (first > 8u || count > 8u - first) recomp_stop(2, "d3d8:pixel-constant-range");
    const float *values = (const float *)(const void *)recomp_memory(source, count * 16u);
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t packed = 0u;
        static const uint32_t shifts[4] = {16u, 8u, 0u, 24u};
        for (uint32_t channel = 0u; channel < 4u; ++channel) {
            const float value = fmaxf(0.0f, fminf(1.0f, values[i * 4u + channel]));
            packed |= (uint32_t)nearbyintf(value * 255.0f) << shifts[channel];
        }
        const uint32_t index = first + i;
        *recomp_memory_u32(device + 0x8e4u + index * 4u) = packed;
        for (uint32_t bank = 0u; bank < 3u; ++bank) {
            const uint32_t mapping = *recomp_memory_u32(data + 0xe4u + bank * 4u);
            const uint32_t slots = bank == 2u ? 2u : 8u;
            for (uint32_t slot = 0u; slot < slots; ++slot) {
                if (((mapping >> (slot * 4u)) & 15u) != index) continue;
                const uint32_t base = bank == 2u ? 0x41e20u : 0x40a60u + bank * 0x20u;
                const uint32_t shadow = bank == 2u ? PIXEL_SHADOW + 0xacu : PIXEL_SHADOW + 0x28u + bank * 0x20u;
                *recomp_memory_u32(shadow + slot * 4u) = packed;
                pixel_method(base + slot * 4u, packed);
            }
        }
    }
    recomp_runtime.registers.esp += 16u;
}

static uint32_t stream_vertices(uint32_t first, uint32_t *stride)
{
    const uint32_t buffer = *recomp_memory_u32(STREAMS + 8u);
    *stride = *recomp_memory_u32(STREAMS);
    if (buffer == 0u || (*recomp_memory_u32(buffer) & 0x70000u) != 0u)
        recomp_stop(2, "d3d8:draw-vertex-buffer");
    uint32_t address;
    if (!recomp_d3d_texture_resolve_cpu_address(*recomp_memory_u32(buffer + 4u), &address))
        recomp_stop(2, "d3d8:draw-vertex-address");
    const uint64_t offset = (uint64_t)first * *stride + address;
    if (offset > UINT32_MAX) recomp_stop(2, "d3d8:draw-vertex-overflow");
    return (uint32_t)offset;
}

static void multiply(const float left[16], const float right[16], float result[16], const char *site)
{
    for (uint32_t row = 0u; row < 4u; ++row) {
        for (uint32_t column = 0u; column < 4u; ++column) {
            float value = 0.0f;
            for (uint32_t k = 0u; k < 4u; ++k) value += left[row * 4u + k] * right[k * 4u + column];
            if (!isfinite(value)) {
                uint32_t bad_left = 0u, bad_right = 0u;
                for (uint32_t i = 0u; i < 16u; ++i) {
                    bad_left += !isfinite(left[i]);
                    bad_right += !isfinite(right[i]);
                }
                recomp_stop(2, "d3d8:nonfinite-transform:%s:cell=%u:left=%u:right=%u:ret=%08x", site,
                    row * 4u + column, bad_left, bad_right, *recomp_memory_u32(recomp_runtime.registers.esp));
            }
            result[row * 4u + column] = value;
        }
    }
}

static void attach_texture(uint32_t device, uint32_t stage, RecompD3dPresenterDrawCommand *draw)
{
    const uint32_t resource = *recomp_memory_u32(device + 0xf88u + stage * 4u);
    if (!recomp_d3d8_resource_inline_texture(stage, &draw->texture)) {
        if (resource == 0u) return;
        if (!recomp_d3d8_resource_describe(resource, &draw->texture)) {
            const uint32_t common = *recomp_memory_u32(resource), type = common & 0x70000u;
            const uint32_t format = type == 0x40000u || type == 0x50000u ?
                *recomp_memory_u32(resource + 12u) : 0u;
            recomp_stop(2, "d3d8:draw-texture-description:stage=%u:resource=%08x:common=%08x:format=%08x:fvf=%x:ret=%08x",
                stage, resource, common, format, draw->fvf, *recomp_memory_u32(recomp_runtime.registers.esp));
        }
    }
    if (draw->texture.depth)
        recomp_stop(2, "d3d8:draw-texture-description:depth:stage=%u:resource=%08x:fvf=%x:ret=%08x",
            stage, resource, draw->fvf, *recomp_memory_u32(recomp_runtime.registers.esp));
    const RecompD3dTextureDesc *desc = &draw->texture;
    if (desc->volume_depth != 0u && stage != 0u)
        recomp_stop(2, "d3d8:volume-texture-stage:%u", stage);
    uint64_t bytes = recomp_d3d_texture_compressed_mip_span(desc);
    if (bytes == 0u) bytes = desc->linear ? (uint64_t)desc->pitch * desc->height :
        (uint64_t)desc->width * desc->height * (desc->volume_depth ? desc->volume_depth : 1u) *
            desc->bits_per_pixel / 8u;
    if (bytes == 0u || bytes > RECOMP_XBOX_RAM_SIZE)
        recomp_stop(2, "d3d8:draw-texture-span");
    uint32_t address;
    if (!recomp_d3d_texture_resolve_cpu_address(desc->data, &address))
        recomp_stop(2, "d3d8:draw-texture-address");
    draw->texture_bytes = recomp_memory(address, (size_t)bytes);
    draw->texture_byte_count = (uint32_t)bytes;
    draw->has_texture = true;
    draw->address_u = *recomp_memory_u32(STAGES + stage * 128u);
    draw->address_v = *recomp_memory_u32(STAGES + stage * 128u + 4u);
    draw->address_w = *recomp_memory_u32(STAGES + stage * 128u + 8u);
    if (desc->format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8) {
        const uint32_t palette = *recomp_memory_u32(device + 0xf98u + stage * 4u);
        if (palette == 0u) recomp_stop(2, "d3d8:draw-missing-palette");
        if (!recomp_d3d_texture_resolve_cpu_address(*recomp_memory_u32(palette + 4u), &address))
            recomp_stop(2, "d3d8:draw-palette-address");
        draw->palette_bytes = recomp_memory(address, 1024u);
        draw->palette_byte_count = 1024u;
    }
    for (uint32_t slot = 0u; slot < 2u; ++slot) {
        const uint32_t header = device + 0x1a24u + slot * 24u;
        if ((*recomp_memory_u32(header + 4u) & 0x7fffffffu) == (desc->data & 0x7fffffffu)) {
            draw->texture_is_backbuffer = true;
            draw->texture_is_front_buffer = slot == 1u;
        }
    }
}

static bool four_tap(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    /* Weighted four-source combiner: r0 = c0*t0 + c1*t1 and r1 = c0*t2 + c1*t3 with
       unique per-stage factors, then r0 + r1. The stage sums are nonnegative, so the
       signed and unsigned stage-2 inputs of the two measured variants agree. */
    static const uint32_t expected[][2] = {
        {0x20u, 0xcu}, {0x24u, 0x1c80u}, {0xd4u, 0x11103u}, {0xd8u, 0x8421u},
    };
    static const uint32_t offsets[8] = {0x08u, 0x68u, 0x6cu, 0x70u, 0x90u, 0xb4u, 0xb8u, 0xbcu};
    /* Doubled stages with a halved sum (the DOA3 contract), or identity scales. */
    static const struct { uint32_t words[8]; float scale, sum_scale; } variants[] = {
        {{0xdc30dd30u, 0x10c00u, 0x10d00u, 0x30c00u, 0xcc20cd20u, 0x10c00u, 0x10d00u, 0x30c00u},
            2.0f, 0.5f},
        {{0x1c301d30u, 0xc00u, 0xd00u, 0xc00u, 0x0c200d20u, 0xc00u, 0xd00u, 0xc00u}, 1.0f, 1.0f},
    };
    static const uint32_t simple[][2] = {
        {0x40260u, 0xd1d8d2d9u}, {0x40264u, 0xd1dad2dbu},
        {0x40ac0u, 0xc1c8c2c9u}, {0x40ac4u, 0xc1cac2cbu},
    };
    /* C0/C1 of stage 0 weight t0/t1; C0/C1 of stage 1 weight t2/t3. */
    static const uint32_t factors[4] = {0x40a60u, 0x40a80u, 0x40a64u, 0x40a84u};
    /* Single-level sources make point and disabled mip filtering equivalent. */
    static const uint32_t sampler[][2] = {
        {0u, 3u}, {1u, 3u}, {3u, 2u}, {4u, 2u},
        {7u, 0u}, {9u, 0u}, {10u, 0u}, {11u, 0u}, {21u, 0u},
    };
    if (!draw->has_texture || ((draw->fvf >> 8u) & 15u) != 4u ||
        draw->texture.bits_per_pixel != 32u || draw->texture.mip_levels != 1u ||
        !((draw->texture.format_byte == 0x12u && draw->texture.linear) ||
          (draw->texture.format_byte == 6u && !draw->texture.linear))) return false;
    for (uint32_t i = 0u; i < sizeof expected / sizeof expected[0]; ++i) {
        if (*recomp_memory_u32(PIXEL_SHADOW + expected[i][0]) != expected[i][1]) return false;
    }
    uint32_t variant = 0u;
    for (; variant < 2u; ++variant) {
        uint32_t i = 0u;
        while (i < 8u && *recomp_memory_u32(PIXEL_SHADOW + offsets[i]) == variants[variant].words[i]) ++i;
        if (i == 8u) break;
    }
    if (variant == 2u) return false;
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    for (uint32_t i = 0u; i < sizeof simple / sizeof simple[0]; ++i) {
        uint32_t value;
        if (!recomp_d3d_get_simple_render_state(state, simple[i][0], &value) || value != simple[i][1])
            return false;
    }
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (!recomp_d3d_get_simple_render_state(state, factors[i], &draw->four_tap_factors[i]))
            return false;
    }
    for (uint32_t stage = 0u; stage < 4u; ++stage) {
        RecompD3dTextureDesc desc;
        if ((!recomp_d3d8_resource_inline_texture(stage, &desc) &&
             !recomp_d3d8_resource_describe(*recomp_memory_u32(device + 0xf88u + stage * 4u), &desc)) ||
            desc.data != draw->texture.data || desc.format_byte != draw->texture.format_byte ||
            desc.width != draw->texture.width || desc.height != draw->texture.height ||
            desc.pitch != draw->texture.pitch || desc.mip_levels != 1u) return false;
        for (uint32_t i = 0u; i < sizeof sampler / sizeof sampler[0]; ++i) {
            if (*recomp_memory_u32(STAGES + stage * 128u + sampler[i][0] * 4u) != sampler[i][1])
                return false;
        }
        if (*recomp_memory_u32(STAGES + stage * 128u + 20u) > 1u) return false;
        float bias;
        recomp_guest_load(&bias, STAGES + stage * 128u + 24u, sizeof bias);
        if (!isfinite(bias) || *recomp_memory_u32(STAGES + stage * 128u + 112u) != stage) return false;
    }
    draw->four_tap_filter = true;
    draw->four_tap_scale = variants[variant].scale;
    draw->four_tap_sum_scale = variants[variant].sum_scale;
    return true;
}

/* The lazy texture-state upload (0x34f400) encodes MIPMAPLODBIAS as
   CVTTSS2SI(bias * 256 + 0.5) clamped to the 13-bit filter field
   [-0x1000, 0xfff]; NaN and overflow give 0x80000000, so they clamp to -16.
   D3D11 caps the top at 15.99. shortcut: omits the supersample bias
   device+0x968 (table 0x353124, 0 at 1x); add it when a scaled back buffer
   samples textures. */
static float lod_bias(uint32_t bits)
{
    float bias;
    memcpy(&bias, &bits, sizeof bias);
    const float scaled = bias * 256.0f + 0.5f;
    int32_t fixed = scaled > -2147483648.0f && scaled < 2147483648.0f ? (int32_t)scaled : INT32_MIN;
    fixed = fixed < -0x1000 ? -0x1000 : fixed > 0xfff ? 0xfff : fixed;
    return fminf(fixed / 256.0f, 15.99f);
}

static bool bright_pass(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    /* Verified combiner: r1 = t0 - c0; r0 = 2 * max(0, r1) - 1; out = max(0, r0).
       Two stages, unique C0/C1, MSB mux; only stage 0 samples (PROJECT2D). */
    static const uint32_t simple[][2] = {
        {0x40260u, 0xd830d150u}, {0x40264u, 0x5d301010u},
        {0x40ac0u, 0xc820c140u}, {0x40ac4u, 0x4d200000u},
        {0x40aa0u, 0xd00u}, {0x40aa4u, 0xc0u}, {0x41e40u, 0xd00u}, {0x41e44u, 0xc0u},
        {0x41e60u, 0x11102u}, {0x40288u, 0xcu}, {0x4028cu, 0x1c80u},
    };
    /* Gaussian-cubic min/mag, no mips, UV0, no color key, sign, kill or transform. */
    static const uint32_t sampler[][2] = {
        {3u, 5u}, {4u, 5u}, {5u, 0u}, {9u, 0u}, {10u, 0u}, {11u, 0u}, {21u, 0u}, {28u, 0u},
    };
    if (!draw->has_texture || ((draw->fvf >> 8u) & 15u) != 1u ||
        *recomp_memory_u32(device + 0x790u) != 1u) return false;
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    uint32_t value;
    for (uint32_t i = 0u; i < sizeof simple / sizeof simple[0]; ++i) {
        if (!recomp_d3d_get_simple_render_state(state, simple[i][0], &value) || value != simple[i][1])
            return false;
    }
    for (uint32_t i = 0u; i < sizeof sampler / sizeof sampler[0]; ++i) {
        if (*recomp_memory_u32(STAGES + sampler[i][0] * 4u) != sampler[i][1]) return false;
    }
    if (!recomp_d3d_get_simple_render_state(state, 0x40a60u, &value)) return false;
    draw->bright_pass = true;
    draw->bright_pass_threshold = value;
    return true;
}

/* Screen tint (definition 0xa74f0, drawn by 0xa75d0 over the back buffer):
   stage 0 writes r1 = dot(t0, c0) to RGB and, by AB_BLUE_TO_ALPHA, alpha;
   stage 1 writes r0 = C0 * r1 (alpha from r1.b). The final words are zero, so
   with fog off 0x34f5f0 outputs r0 + (SpecularEnable ? v1 : 0) and r0.a. */
static bool luminance_tint(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    static const uint32_t simple[][2] = {
        {0x40260u, 0u}, {0x40264u, 0xd1cd1010u}, {0x40aa0u, 0u}, {0x40aa4u, 0xc0u},
        {0x40ac0u, 0xc8c10000u}, {0x40ac4u, 0xc1cd0000u}, {0x41e40u, 0x820d0u}, {0x41e44u, 0xc0u},
        {0x41e60u, 0x11102u}, {0x417f8u, 0u}, {0x41e74u, 0u}, {0x41e78u, 0u},
    };
    /* Linear min/mag, no mips, UV0, no color key, kill or transform. */
    static const uint32_t sampler[][2] = {
        {3u, 2u}, {4u, 2u}, {5u, 0u}, {9u, 0u}, {10u, 0u}, {11u, 0u}, {21u, 0u}, {28u, 0u},
    };
    /* An absent specular attribute reads zero (0x3493de), as for the distortion pass. */
    if (!draw->has_texture || ((draw->fvf >> 8u) & 15u) != 1u ||
        *recomp_memory_u32(device + 0x788u) != 0u || *recomp_memory_u32(device + 0x790u) != 1u ||
        *recomp_memory_u32(0x3553d0u) != 0u ||
        ((draw->fvf & 0x80u) != 0u && *recomp_memory_u32(0x3553fcu) != 0u)) return false;
    for (uint32_t i = 0u; i < sizeof sampler / sizeof sampler[0]; ++i)
        if (*recomp_memory_u32(STAGES + sampler[i][0] * 4u) != sampler[i][1]) return false;
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    uint32_t value;
    for (uint32_t i = 0u; i < sizeof simple / sizeof simple[0]; ++i)
        if (!recomp_d3d_get_simple_render_state(state, simple[i][0], &value) || value != simple[i][1])
            return false;
    if (!recomp_d3d_get_simple_render_state(state, 0x40a60u, &draw->luminance_tint_factors[0]) ||
        !recomp_d3d_get_simple_render_state(state, 0x40a64u, &draw->luminance_tint_factors[1]))
        return false;
    draw->luminance_tint = true;
    return true;
}

/* Stage distortion pass (FUN_0027b070, definition 0x7af5d8): t0 PROJECT2D,
   t1 DOTPRODUCT and t2 DOT_ST with MINUS1_TO_1_D3D dot mapping, one combiner
   r0 = t2 and zero final-combiner words. With fog off, 0x34f5f0 outputs
   r0 + (SpecularEnable ? v1 : 0) and r0.a. For the caller's FVF 0x140404,
   SetVertexShader (0x3493de) supplies zero for the absent specular attribute. */
static bool distortion_shader(uint32_t device)
{
    static const uint32_t words[][2] = {
        {0x00u, 0xda301010u}, {0x20u, 0u}, {0x24u, 0u}, {0x68u, 0xc0u}, {0x88u, 0xca200000u},
        {0xa8u, 0u}, {0xb4u, 0xc0u}, {0xd4u, 0x00011101u}, {0xd8u, 0x2621u}, {0xdcu, 0x11u},
        {0xe0u, 0u},
    };
    const uint32_t handle = *recomp_memory_u32(device + PIXEL_HANDLE);
    if (handle == 0u) return false;
    const uint32_t data = definition(handle);
    for (uint32_t i = 0u; i < sizeof words / sizeof words[0]; ++i)
        if (*recomp_memory_u32(data + words[i][0]) != words[i][1]) return false;
    return *recomp_memory_u32(0x3553d0u) == 0u;
}

/* DOT_ST samples t2 at (dot(tc1.str, n), dot(tc2.str, n)) with n the signed
   t0 texel. The presenter draws it as FVF 0x404: t2 becomes the primary
   texture, t0 the second, and the 56-byte vertices (UV0, two STR sets, unused
   UV3) pack into four float2 sets: UV0, tc1.st, (tc1.r, tc2.s), tc2.tr. */
static uint8_t *distortion(uint32_t device, uint32_t stride, RecompD3dPresenterDrawCommand *draw)
{
    RecompD3dPresenterDrawCommand scene = {0};
    attach_texture(device, 2u, &scene);
    if (stride != 56u || !draw->has_texture || !scene.has_texture || draw->palette_bytes != NULL ||
        scene.palette_bytes != NULL || draw->texture.linear)
        recomp_stop(2, "d3d8:distortion-inputs");
    draw->reflection_texture = draw->texture;
    draw->reflection_bytes = draw->texture_bytes;
    draw->reflection_byte_count = draw->texture_byte_count;
    draw->reflection_address_u = draw->address_u;
    draw->reflection_address_v = draw->address_v;
    draw->reflection_point_mip_filter = *recomp_memory_u32(STAGES + 20u) == 1u;
    draw->reflection_mip_lod_bias = lod_bias(*recomp_memory_u32(STAGES + 24u));
    draw->texture = scene.texture;
    draw->texture_bytes = scene.texture_bytes;
    draw->texture_byte_count = scene.texture_byte_count;
    draw->address_u = scene.address_u;
    draw->address_v = scene.address_v;
    draw->point_mip_filter = *recomp_memory_u32(STAGES + 256u + 20u) == 1u;
    draw->mip_lod_bias = lod_bias(*recomp_memory_u32(STAGES + 256u + 24u));
    draw->texture_is_backbuffer = scene.texture_is_backbuffer;
    draw->texture_is_front_buffer = scene.texture_is_front_buffer;
    float *packed = malloc((size_t)draw->vertex_count * 48u);
    if (packed == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        memcpy(packed + i * 12u, (const uint8_t *)draw->vertex_bytes + i * 56u, 48u);
    draw->vertex_bytes = packed;
    draw->vertex_stride = 48u;
    draw->distortion = true;
    return (uint8_t *)packed;
}

/* The stage effect's bump shader (definition built by FUN_0027ffc0, bound by
   FUN_002807f0): t0 PROJECT2D, t1-t3 BUMPENVMAP of t0 (PSInputTexture 0).
   Combiners 0-1 square T3.a twice, 2 sets r0 = t2 * (1 - t3.a), 3 sets
   r0 *= v0; the final combiner is lerp(fog.rgb, r0, fog.a) with v0.a. T1 is
   never read. 0x34f5f0 keeps these final words because they are nonzero.
   The water shader (built by FUN_0026bd20, bound by the unanalyzed draw at
   0x26c980) has t0 PROJECT2D, t1 and t3 BUMPENVMAP of t0 and no t2 (0x300c1).
   Its seven combiners give s = max(0, dot(expand v0, expand t3))^8, then
   r0 = v1 t1.a, r0 = (s - 0.5) + (r0 - 0.5), r0 += t3.a t1 and r0.a =
   t3.a v0.a, with the same final combiner. Returns 1 (effect), 2 (water) or 0. */
static uint32_t bump_shader(uint32_t device)
{
    static const uint32_t effect[][2] = {
        {0x00u, 0xdbdb1010u}, {0x04u, 0xdbdb1010u}, {0x08u, 0u}, {0x0cu, 0u},
        {0x20u, 0x130c0300u}, {0x24u, 0x1480u}, {0x68u, 0xb0u}, {0x6cu, 0xb0u}, {0x70u, 0u},
        {0x74u, 0u}, {0x88u, 0u}, {0x8cu, 0u}, {0x90u, 0xca3b0000u}, {0x94u, 0xccc40000u},
        {0xa8u, 0u}, {0xb4u, 0u}, {0xb8u, 0u}, {0xbcu, 0xc0u}, {0xc0u, 0xc0u},
        {0xd4u, 0x11104u}, {0xd8u, 0x318c1u}, {0xdcu, 0u}, {0xe0u, 0u},
    };
    static const uint32_t water[][2] = {
        {0x00u, 0u}, {0x04u, 0x1d1d1010u}, {0x08u, 0xdddd1010u}, {0x0cu, 0xdddd1010u},
        {0x10u, 0u}, {0x14u, 0u}, {0x18u, 0xdbd41010u}, {0x20u, 0x130c0300u}, {0x24u, 0x1c80u},
        {0x68u, 0u}, {0x6cu, 0xd0u}, {0x70u, 0xd0u}, {0x74u, 0xd0u}, {0x78u, 0u}, {0x7cu, 0u},
        {0x80u, 0xc0u}, {0x88u, 0x444b0000u}, {0x8cu, 0x0d0d0000u}, {0x90u, 0xcdcd0000u},
        {0x94u, 0xc5d90000u}, {0x98u, 0x9d208c20u}, {0x9cu, 0xdbc9200cu}, {0xa0u, 0u},
        {0xa8u, 0u}, {0xb4u, 0x820d0u}, {0xb8u, 0xd0u}, {0xbcu, 0xd0u}, {0xc0u, 0xc0u},
        {0xc4u, 0xc00u}, {0xc8u, 0xc00u}, {0xccu, 0u}, {0xd4u, 0x11107u}, {0xd8u, 0x300c1u},
        {0xdcu, 0u}, {0xe0u, 0u},
    };
    const uint32_t handle = *recomp_memory_u32(device + PIXEL_HANDLE);
    if (handle == 0u) return 0u;
    const uint32_t data = definition(handle);
    bool is_effect = true, is_water = true;
    for (uint32_t i = 0u; i < sizeof effect / sizeof effect[0]; ++i)
        is_effect = is_effect && *recomp_memory_u32(data + effect[i][0]) == effect[i][1];
    for (uint32_t i = 0u; i < sizeof water / sizeof water[0]; ++i)
        is_water = is_water && *recomp_memory_u32(data + water[i][0]) == water[i][1];
    return is_effect ? 1u : is_water ? 2u : 0u;
}

/* FUN_002807f0 binds a 16-bit V8U8 (format 0x28) map on stage 0 with R/G/B
   COLORSIGN (0x355088 = 0xe0000000), the scene on stage 2 and a mask on
   stage 3, each with linear min/mag, and BUMPENVMAT00-11 on stages 1-3.
   The water draw (0x26c980) binds the same kind of map, its reflection on
   stage 1 and an environment map on stage 3, with matrices on stages 1 and 3,
   fog off and SPECULARENABLE on. */
static void bump_stages(uint32_t device, bool water, RecompD3dPresenterDrawCommand *draw)
{
    const uint32_t first = water ? 1u : 2u;
    uint32_t stage[4][32];
    recomp_guest_load(stage, STAGES, sizeof stage);
    const RecompD3dTextureDesc map = draw->texture;
    const void *map_bytes = draw->texture_bytes;
    const uint32_t map_byte_count = draw->texture_byte_count;
    const bool has_map = draw->has_texture && draw->palette_bytes == NULL;
    draw->has_texture = draw->texture_is_backbuffer = draw->texture_is_front_buffer = false;
    attach_texture(device, first, draw);
    RecompD3dPresenterDrawCommand mask = {0};
    attach_texture(device, 3u, &mask);
    bool admitted = has_map && draw->has_texture && mask.has_texture && map.format_byte == 0x28u &&
        !map.linear && map.bits_per_pixel == 16u && !mask.texture.linear &&
        draw->palette_bytes == NULL && mask.palette_bytes == NULL && stage[0][10] == 0xe0000000u;
    const uint32_t sampled[3] = {0u, first, 3u};
    for (uint32_t i = 0u; i < 3u; ++i) {
        const uint32_t *t = stage[sampled[i]];
        admitted = admitted && t[3] == 2u && t[4] == 2u && t[9] == 0u && t[11] == 0u &&
            (i == 0u || t[10] == 0u);
    }
    if (!admitted)
        recomp_stop(2, "d3d8:stage-bump-inputs:%u:%x:%u:%x:%u:%u", first, map.format_byte,
            map.bits_per_pixel, stage[0][10], draw->has_texture, mask.has_texture);
    /* shortcut: water V1 is oD1 only as observed with SPECULARENABLE on; other
       states stop until the hardware's V1 source without it is known. */
    if (water && *recomp_memory_u32(0x3553fcu) == 0u) recomp_stop(2, "d3d8:stage-water-specular");
    draw->alpha_mask = map;
    draw->alpha_mask_bytes = map_bytes;
    draw->alpha_mask_byte_count = map_byte_count;
    draw->bump_address_u = stage[0][0];
    draw->bump_address_v = stage[0][1];
    draw->point_mip_filter = stage[first][5] == 1u;
    draw->mip_lod_bias = lod_bias(stage[first][6]);
    draw->reflection_texture = mask.texture;
    draw->reflection_bytes = mask.texture_bytes;
    draw->reflection_byte_count = mask.texture_byte_count;
    draw->reflection_address_u = mask.address_u;
    draw->reflection_address_v = mask.address_v;
    draw->reflection_point_mip_filter = stage[3][5] == 1u;
    draw->reflection_mip_lod_bias = lod_bias(stage[3][6]);
    for (uint32_t i = 0u; i < 2u; ++i) {
        memcpy(draw->bump_matrices[i], &stage[i == 0u ? first : 3u][22], sizeof draw->bump_matrices[i]);
        for (uint32_t k = 0u; k < 4u; ++k)
            if (!isfinite(draw->bump_matrices[i][k])) recomp_stop(2, "d3d8:stage-bump-matrix");
    }
    /* 0x34f5f0 programs LINEAR (table mode 3) as bias end/(end - start) + 1
       and scale -1/(end - start) (8192 for end == start), and mode 0 as bias
       and scale 1 over the program's oFog. Both read as D3D fog only if the
       fog unit subtracts 1 from the bias: f = bias - 1 + scale * oFog.
       shortcut: the minus-one bias is inferred from D3D fog semantics, not
       measured; compare against a capture if the effect fades wrongly. */
    const uint32_t fog = *recomp_memory_u32(0x3553d0u), mode = *recomp_memory_u32(0x3553d4u);
    float bias = 0.0f, scale = 1.0f;
    if (fog != 0u && mode == 3u) {
        float range[2];
        recomp_guest_load(range, 0x3553d8u, sizeof range);
        scale = range[1] != range[0] ? 1.0f / (range[1] - range[0]) : 8192.0f;
        bias = range[1] * scale;
        scale = -scale;
    } else if (fog != 0u && mode != 0u) {
        recomp_stop(2, "d3d8:stage-bump-fog:%u", mode);
    }
    if (!isfinite(bias) || !isfinite(scale)) recomp_stop(2, "d3d8:stage-bump-fog-range");
    /* shortcut: FOG.a with FOG_ENABLE 0 is taken as 1 (no fog); check a
       hardware capture if the effect should tint toward the fog color. */
    draw->bump_fog[0] = bias;
    draw->bump_fog[1] = scale;
    draw->bump_fog[2] = fog != 0u ? 1.0f : 0.0f;
    draw->fog_color = *recomp_memory_u32(0x355488u);
    draw->bump_env = true;
    draw->bump_water = water;
}

/* Stage-0 color and alpha as products of texture and diffuse or TFACTOR: the first
   stage's flag 0x10 (0x352aba) makes the arg mapper (0x3529c0) read CURRENT
   as V0, the diffuse color. With alpha_tfactor, alpha MODULATE may take
   TFACTOR as one argument, which the presenter applies as a factor. */
static bool stage0_select(const uint32_t *stage, float select[4], bool *alpha_tfactor)
{
    /* 0x352b1e ends the chain with diffuse RGB/alpha when COLOROP is DISABLE. */
    if (stage[12] == 1u) {
        select[0] = select[2] = 0.0f;
        select[1] = select[3] = 1.0f;
        return true;
    }
    for (uint32_t channel = 0u; channel < 2u; ++channel) {
        uint32_t op = stage[12u + channel * 4u];
        uint32_t arg1 = stage[14u + channel * 4u], arg2 = stage[15u + channel * 4u];
        if (arg1 == 1u) arg1 = 0u;
        if (arg2 == 1u) arg2 = 0u;
        if ((op == 2u && arg1 == 3u) || (op == 3u && arg2 == 3u)) {
            select[channel * 2u] = 0.0f;
            select[channel * 2u + 1u] = 2.0f;
            continue;
        }
        /* Without a separate alpha factor, select TFACTOR per channel. */
        if (alpha_tfactor == NULL && op == 4u &&
            ((arg1 == 2u && arg2 == 3u) || (arg1 == 3u && arg2 == 2u))) {
            select[channel * 2u] = 1.0f;
            select[channel * 2u + 1u] = 2.0f;
            continue;
        }
        if (alpha_tfactor != NULL && channel == 1u && op == 4u && (arg1 == 3u || arg2 == 3u)) {
            op = arg1 == 3u ? 3u : 2u;
            *alpha_tfactor = true;
        }
        const bool first = op == 2u || op == 4u, second = op == 3u || op == 4u;
        if ((!first && !second) || (first && arg1 != 0u && arg1 != 2u) ||
            (second && arg2 != 0u && arg2 != 2u))
            return false;
        select[channel * 2u] = (first && arg1 == 2u) || (second && arg2 == 2u);
        select[channel * 2u + 1u] = (first && arg1 == 0u) || (second && arg2 == 0u);
    }
    return true;
}

static void attach_lighting(uint32_t device, const RecompD3dVertexLayout *layout,
    RecompD3dPresenterDrawCommand *draw)
{
    if (layout->pretransformed || *recomp_memory_u32(0x3553f8u) == 0u) return;
    /* Missing FVF colors resolve to material even when COLORVERTEX is enabled. */
    /* shortcut: vertex-color sources and two-sided lighting need additional presenter inputs. */
    if (layout->normal_offset == RECOMP_D3D_FVF_ABSENT ||
        layout->diffuse_offset != RECOMP_D3D_FVF_ABSENT ||
        layout->specular_offset != RECOMP_D3D_FVF_ABSENT ||
        *recomp_memory_u32(0x355494u) != 0u)
        recomp_stop(2, "d3d8:lighting-layout-or-source");
    /* Inline Simple setters bypass the guest shadow; absent state retains the SDK smooth default. */
    uint32_t shade = 0x1d01u;
    (void)recomp_d3d_get_simple_render_state(recomp_d3d8_state_model(), 0x4037cu, &shade);
    if (shade != 0x1d01u)
        recomp_stop(2, "d3d8:lighting-shade-mode");
    RecompD3dDirectionalLighting result = {0};
    const uint32_t *stage = recomp_memory_u32(STAGES);
    if (draw->has_texture && !stage0_select(stage, result.stage, NULL))
        recomp_stop(2, "d3d8:lighting-stage:%u,%u,%u/%u,%u,%u:fvf=%x:ps=%x:ret=%08x:s1=%u,%u",
            stage[12], stage[14], stage[15], stage[16], stage[18], stage[19], draw->fvf,
            *recomp_memory_u32(device + PIXEL_HANDLE), *recomp_memory_u32(recomp_runtime.registers.esp),
            *recomp_memory_u32(STAGES + 128u + 48u), *recomp_memory_u32(STAGES + 128u + 64u));
    float material[17];
    recomp_guest_load(material, device + 0xf00u, sizeof material);
    /* 0x34fc80 sends diffuse alpha unchanged as the lit alpha (NV097 0x3b4),
       where a passthrough NaN saturates by its sign bit. */
    /* shortcut: sign saturation is observed for vertex-color passthrough, not
       material alpha; verify on hardware if a lit NaN alpha looks wrong. */
    if (isnan(material[3])) material[3] = signbit(material[3]) ? 0.0f : 1.0f;
    const bool specular = *recomp_memory_u32(0x3553fcu) != 0u || (*recomp_memory_u32(device + 8u) & 0x40u) != 0u;
    /* The light upload (0x34fb90) reads diffuse, ambient and specular RGB;
       0x34fc80 reads emissive RGB and diffuse alpha; the specular LUT
       (0x34fe40) reads power. Specular terms and the ambient, specular and
       emissive alphas never reach the image when unused. */
    for (uint32_t i = 0u; i < 17u; ++i) {
        const bool used = i <= 6u || (i >= 12u && i <= 14u) ||
            (specular && ((i >= 8u && i <= 10u) || i == 16u));
        uint32_t bits;
        memcpy(&bits, &material[i], sizeof bits);
        if (used && !isfinite(material[i])) recomp_stop(2, "d3d8:lighting-material:%u:%08x", i, bits);
    }
    const uint32_t ambient = *recomp_memory_u32(0x35542cu);
    for (uint32_t c = 0u; c < 3u; ++c)
        result.ambient_emissive[c] = material[12u + c] + material[4u + c] *
            ((ambient >> (16u - 8u * c)) & 255u) / 255.0f;
    memcpy(result.material_diffuse, material, sizeof result.material_diffuse);
    const uint32_t array = *recomp_memory_u32(device + 0x7a4u);
    const uint32_t capacity = *recomp_memory_u32(device + 0x7a8u);
    if (capacity > RECOMP_XBOX_RAM_SIZE / sizeof(RecompD3dLightRecord) ||
        (capacity != 0u && array == 0u)) recomp_stop(2, "d3d8:lighting-array");
    if (capacity != 0u) recomp_memory(array, capacity * sizeof(RecompD3dLightRecord));
    for (uint32_t slot = 0u; slot < 8u; ++slot) {
        const uint32_t address = *recomp_memory_u32(device + 0x7acu + slot * 4u);
        if (address == 0u) continue;
        if (address < array || (address - array) / sizeof(RecompD3dLightRecord) >= capacity ||
            (address - array) % sizeof(RecompD3dLightRecord) != 0u)
            recomp_stop(2, "d3d8:lighting-slot");
        RecompD3dLightRecord record;
        recomp_guest_load(&record, address, sizeof record);
        const RecompD3dLightDesc *light = &record.desc;
        if (record.slot != slot || (light->type != 1u && light->type != 3u))
            recomp_stop(2, "d3d8:lighting-type-or-slot");
        const bool point = light->type == 1u;
        const float *values = (const float *)(const void *)((const uint8_t *)light + 4u);
        for (uint32_t i = 0u; i < 25u; ++i) {
            /* 0x34fb90 reads RGB only; 0x34ff80 selects direction or point data. */
            const bool used = i < 3u || (i >= 8u && i <= 10u) ||
                (specular && i >= 4u && i <= 6u) ||
                (point && ((i >= 12u && i <= 14u) || i == 18u || (i >= 20u && i <= 22u)));
            if (used && !isfinite(values[i])) {
                uint32_t bits;
                memcpy(&bits, &values[i], sizeof bits);
                recomp_stop(2, "d3d8:lighting-nonfinite:slot=%u:type=%u:field=%u:bits=%08x:fvf=%x:spec=%u:ret=%08x",
                    slot, light->type, i, bits, draw->fvf, specular,
                    *recomp_memory_u32(recomp_runtime.registers.esp));
            }
        }
        const uint32_t i = result.count++;
        for (uint32_t c = 0u; c < 3u; ++c) {
            result.colors[i][c] = light->diffuse[c];
            result.specular[i][c] = specular ? light->specular[c] * material[8u + c] : 0.0f;
            result.ambient[i][c] = light->ambient[c] * material[4u + c];
            result.positions[i][c] = point ? light->position[c] : 0.0f;
            result.directions[i][c] = point ? 0.0f : record.normalized_direction[c];
            result.attenuation[i][c] = point ? light->attenuation[c] : 0.0f;
            if (!isfinite(result.directions[i][c])) recomp_stop(2, "d3d8:lighting-direction");
        }
        result.positions[i][3] = point;
        result.attenuation[i][3] = point ? light->range : 0.0f;
    }
    for (uint32_t i = 0u; i <= layout->blend_weight_count; ++i) {
        recomp_guest_load(result.world_transforms[i], device + 0xde0u + i * 64u, 64u);
        if (!recomp_d3d_normal_transform(result.world_transforms[i], result.normal_transforms[i])) {
            for (uint32_t j = 0u; j < 16u; ++j)
                if (!isfinite(result.world_transforms[i][j])) recomp_stop(2, "d3d8:lighting-normal-transform");
            /* The SDK inverse (0x34b530) fails on a zero determinant and uploads
               an unset matrix. Such a world collapses an axis; zero normals keep
               only ambient and emissive light. */
            memset(result.normal_transforms[i], 0, sizeof result.normal_transforms[i]);
        }
    }
    if (specular) {
        if (*recomp_memory_u32(0x355400u) != 0u) recomp_stop(2, "d3d8:lighting-local-viewer");
        float view[16];
        recomp_guest_load(view, device + 0xc60u, sizeof view);
        const float length = sqrtf(view[2] * view[2] + view[6] * view[6] + view[10] * view[10]);
        if (!isfinite(length) || length <= 0.0f) recomp_stop(2, "d3d8:lighting-view");
        for (uint32_t c = 0u; c < 3u; ++c) result.view[c] = -view[2u + 4u * c] / length;
        /* shortcut: zero power uses epsilon until the presenter has a separate specular-enable bit. */
        result.view[3] = material[16] > 0.0f ? material[16] : 1e-6f;
    }
    result.normalize = *recomp_memory_u32(0x355498u) != 0u;
    result.stage_mode = result.enabled = true;
    draw->directional = result;
}

/* Camera-space reflection inputs: world-view, its normal matrix and the
   texture matrix of the generating stage (TEXTURE0 0xce0, TEXTURE1 0xd20). */
/* World 3x3 rank of at most one: every row pair has a zero cross product, up to
   rounding. 0xea6e0 scales rows 0 and 2 of the stack top by zero (0x283d30), so
   each child world entry is one rounded product of the surviving row. Rounding
   each entry once moves a 2x2 minor by at most FLT_EPSILON times its two terms
   (exact in double); this allows twice that. Issue #32's squash reached 0.26. */
static bool world_collapses_to_line(const float world[16])
{
    for (uint32_t a = 0u; a < 2u; ++a)
        for (uint32_t b = a + 1u; b < 3u; ++b)
            for (uint32_t c = 0u; c < 3u; ++c) {
                const uint32_t x = (c + 1u) % 3u, y = (c + 2u) % 3u;
                const double p = (double)world[a * 4u + x] * world[b * 4u + y];
                const double q = (double)world[a * 4u + y] * world[b * 4u + x];
                if (!(fabs(p - q) <= 2.0 * FLT_EPSILON * (fabs(p) + fabs(q)))) return false;
            }
    return true;
}

/* Every edge texel of a single-level DXT5 texture holds one value. Only
   endpoint indices count, whose decoded value no decoder rounding can change. */
static bool dxt5_uniform_edge(const uint8_t *bytes, uint32_t width, uint32_t height)
{
    int64_t first = -1;
    for (uint32_t y = 0u; y < height; ++y)
        for (uint32_t x = 0u; x < width; x += (y == 0u || y == height - 1u || x == width - 1u) ? 1u : width - 1u) {
            const uint8_t *block = bytes + ((y / 4u) * (width / 4u) + x / 4u) * 16u;
            const uint32_t texel = (y % 4u) * 4u + x % 4u;
            uint64_t alpha_bits = 0u;
            memcpy(&alpha_bits, block + 2u, 6u);
            const uint32_t a = (uint32_t)(alpha_bits >> (3u * texel)) & 7u;
            const uint32_t c = (block[12u + y % 4u] >> (2u * (x % 4u))) & 3u;
            int64_t alpha;
            if (a < 2u) alpha = block[a];
            else if (block[0] <= block[1] && a >= 6u) alpha = a == 6u ? 0 : 255;
            else return false;
            if (c > 1u) return false;
            const int64_t value = alpha << 16 | block[8u + 2u * c] | block[9u + 2u * c] << 8;
            if (first >= 0 && value != first) return false;
            first = value;
        }
    return first >= 0;
}

/* 0x1df200's projector divides by (size - 50)^2, so TEXTURE0 holds infinities
   and NaNs for one frame. Camera-space position has w = 1, so a non-finite
   translation makes S and T non-finite at every vertex under any NaN or
   zero-times-infinity rule. CLAMP sends such a coordinate to an edge, whether
   the texture unit reads NaN as zero or a saturated end, so a uniform edge
   makes the sample exact: the corner at (-1, -1). */
static bool clamped_edge_transform(RecompD3dPresenterDrawCommand *draw)
{
    const uint32_t *stage = recomp_memory_u32(STAGES);
    const float *m = draw->reflection_transform;
    if (stage[28] != 0x20000u || (stage[21] != 2u && stage[21] != 3u) ||
        isfinite(m[12]) || isfinite(m[13]) ||
        stage[0] != 3u || stage[1] != 3u ||
        !draw->has_texture || draw->texture.volume_depth != 0u || draw->texture.mip_levels != 1u ||
        draw->texture.format_byte != RECOMP_D3D_TEXTURE_FORMAT_DXT5 ||
        draw->texture.width % 4u != 0u || draw->texture.height % 4u != 0u ||
        !dxt5_uniform_edge(draw->texture_bytes, draw->texture.width, draw->texture.height))
        return false;
    memset(draw->reflection_transform, 0, sizeof draw->reflection_transform);
    draw->reflection_transform[12] = draw->reflection_transform[13] = -1.0f;
    draw->reflection_transform[15] = 1.0f;
    return true;
}

/* columns: how many texture-matrix columns the SDK upload (0x34f7d0) sends for
   the stage's TEXTURETRANSFORMFLAGS count; the rest never reach the GPU. */
static bool reflection_inputs(uint32_t device, uint32_t texture_matrix, uint32_t columns,
    bool needs_normal, RecompD3dPresenterDrawCommand *draw)
{
    float world[16], view[16];
    recomp_guest_load(view, device + 0xc60u, sizeof view);
    draw->reflection_normalize = *recomp_memory_u32(0x355498u) != 0u;
    /* 0x3502d0 uploads world-view and normal matrices for every blend influence. */
    for (uint32_t i = 0u; i <= draw->blend_weight_count; ++i) {
        float *wv = i == 0u ? draw->reflection_world_view : draw->reflection_blend_world_view[i - 1u];
        float *normal = i == 0u ? draw->reflection_normal : draw->reflection_blend_normal[i - 1u];
        recomp_guest_load(world, device + 0xde0u + i * 64u, sizeof world);
        multiply(world, view, wv, "reflection-world-view");
        /* Position texgen and the no-normal reflection shader never read an inverse. */
        if (!needs_normal) continue;
        /* 0x34b530 divides by det only without normalization (0x355498). */
        if (!(draw->reflection_normalize ? recomp_d3d_normal_adjugate(wv, normal, NULL)
                : recomp_d3d_normal_transform(wv, normal))) {
            /* A lone world of rank one or less (0xea6e0's squash with a zero
               animation scale) puts every vertex on one eye-space line, so no
               triangle has area; host snapping slivers are rounding. Otherwise
               only exact raster coverage can prove the undefined normal unused. */
            if (draw->primitive_type >= 5u && draw->primitive_type <= 7u &&
                ((draw->blend_weight_count == 0u && world_collapses_to_line(world)) ||
                 !recomp_d3d8_draw_has_coverage(draw))) return false;
            /* The stop id is capped at 160 characters, too short for the matrix that
               shows which object triggered it, so the matrix goes on the line before. */
            fprintf(stderr, "recomp d3d8: reflection-normal world influence=%u prim=%u"
                " rows=[%.9g %.9g %.9g|%.9g %.9g %.9g|%.9g %.9g %.9g] translation=[%.9g %.9g %.9g]\n",
                i, draw->primitive_type, world[0], world[1], world[2], world[4], world[5], world[6],
                world[8], world[9], world[10], world[12], world[13], world[14]);
            recomp_stop(2, "d3d8:reflection-normal:fvf=%x:matrix=%x:influence=%u/%u:normalize=%u:ret=%08x",
                draw->fvf, texture_matrix, i, draw->blend_weight_count, draw->reflection_normalize,
                *recomp_memory_u32(recomp_runtime.registers.esp));
        }
    }
    recomp_guest_load(draw->reflection_transform, device + texture_matrix, sizeof draw->reflection_transform);
    if (texture_matrix == 0xce0u && clamped_edge_transform(draw)) return true;
    for (uint32_t i = 0u; i < 16u; ++i)
        if (i % 4u < columns && !isfinite(draw->reflection_transform[i]))
            recomp_stop(2, "d3d8:reflection-transform:fvf=%x:matrix=%x:field=%u:bits=%08x:tci=%x,%x:ret=%08x",
                draw->fvf, texture_matrix, i, *recomp_memory_u32(device + texture_matrix + i * 4u),
                *recomp_memory_u32(STAGES + 28u * 4u), *recomp_memory_u32(STAGES + 128u + 28u * 4u),
                *recomp_memory_u32(recomp_runtime.registers.esp));
    return true;
}

static bool attach_reflection(uint32_t device, const RecompD3dVertexLayout *layout,
    RecompD3dPresenterDrawCommand *draw)
{
    uint32_t stages[4][32];
    recomp_guest_load(stages, STAGES, sizeof stages);
    if (!recomp_d3d_reflection_material(stages)) return true;
    /* Without a normal stream the presenter's zero inline normal applies: no SDK
       path writes v2 inline data (only 0x34aac0's v0/v9 via SetVertexData2f). */
    if (layout->pretransformed || layout->texcoord_count == 0u ||
        !draw->has_texture ||
        draw->texture.linear || draw->texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 ||
        draw->palette_bytes != NULL)
        recomp_stop(2, "d3d8:reflection-layout:fvf=%x:gen=%x,%x:tex=%u,%u,%x,%u", draw->fvf,
            stages[0][28], stages[1][28], draw->has_texture, draw->texture.linear,
            draw->texture.format_byte, draw->palette_bytes != NULL);
    /* Each stage samples with its own mip filter and LOD bias (FUN_002807f0
       writes a stage-0 bias alone). shortcut: MAXMIPLEVEL needs a sampler MinLOD. */
    if (stages[0][7] != 0u || stages[1][7] != 0u)
        recomp_stop(2, "d3d8:reflection-sampler:maxmip=%u,%u", stages[0][7], stages[1][7]);
    RecompD3dPresenterDrawCommand texture = {0};
    attach_texture(device, 1u, &texture);
    if (!texture.has_texture || texture.texture.linear || texture.palette_bytes != NULL ||
        texture.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8)
        recomp_stop(2, "d3d8:reflection-texture");
    if (texture.texture.mip_levels > 1u && stages[1][5] != 1u && stages[1][5] != 2u)
        recomp_stop(2, "d3d8:reflection-sampler:mip=%u", stages[1][5]);
    draw->reflection_point_mip_filter = stages[1][5] == 1u;
    draw->reflection_mip_lod_bias = lod_bias(stages[1][6]);
    draw->reflection_texture = texture.texture;
    draw->reflection_address_u = texture.address_u;
    draw->reflection_address_v = texture.address_v;
    draw->reflection_bytes = texture.texture_bytes;
    draw->reflection_byte_count = texture.texture_byte_count;
    if (!reflection_inputs(device, 0xd20u, 4u,
        layout->normal_offset != RECOMP_D3D_FVF_ABSENT &&
        (stages[1][28] == 0x30000u || (stages[2][12] == 14u && stages[2][28] == 0x30000u)), draw))
        return false;
    recomp_guest_load(draw->reflection_diffuse, device + 0xf00u, sizeof draw->reflection_diffuse);
    for (uint32_t i = 0u; i < 4u; ++i)
        if (!isfinite(draw->reflection_diffuse[i])) recomp_stop(2, "d3d8:reflection-material");
    draw->reflection_mesh_uv = stages[1][28] == 0u;
    draw->reflection_premultiplied = stages[1][12] == 16u;
    draw->reflection_color_only = stages[1][16] == 2u;
    draw->reflection_alpha_kill = stages[1][11] == 4u;
    draw->reflection_stage2 = stages[2][12] == 14u;
    if (draw->reflection_stage2) {
        RecompD3dPresenterDrawCommand third = {0};
        attach_texture(device, 2u, &third);
        if (!third.has_texture || third.texture.linear || third.palette_bytes != NULL ||
            third.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8)
            recomp_stop(2, "d3d8:reflection-stage2-layout");
        if (stages[2][7] != 0u ||
            (third.texture.mip_levels > 1u && stages[2][5] != 1u && stages[2][5] != 2u))
            recomp_stop(2, "d3d8:reflection-stage2-sampler");
        draw->reflection_stage2_texture = third.texture;
        draw->reflection_stage2_bytes = third.texture_bytes;
        draw->reflection_stage2_byte_count = third.texture_byte_count;
        draw->reflection_stage2_address_u = third.address_u;
        draw->reflection_stage2_address_v = third.address_v;
        draw->reflection_stage2_point_mip_filter = stages[2][5] == 1u;
        draw->reflection_stage2_mip_lod_bias = lod_bias(stages[2][6]);
        /* Mesh-UV stage 2 (coordinate mode 0) never reads TEXTURE2. */
        draw->reflection_stage2_mesh_uv = stages[2][28] == 0u;
        if (!draw->reflection_stage2_mesh_uv) {
            recomp_guest_load(draw->reflection_stage2_transform, device + 0xd60u,
                sizeof draw->reflection_stage2_transform);
            for (uint32_t i = 0u; i < 16u; ++i)
                if (!isfinite(draw->reflection_stage2_transform[i]))
                    recomp_stop(2, "d3d8:reflection-stage2-transform");
        }
    }
    /* shortcut: stage-2 texture MODULATE stays guarded; bind it when a run reaches it. */
    draw->reflection_skip_diffuse = !draw->reflection_stage2 && stages[2][14] == 2u;
    if (draw->reflection_skip_diffuse && *recomp_memory_u32(device + 0xf90u) != 0u)
        recomp_stop(2, "d3d8:reflection-stage2-texture");
    draw->has_reflection = true;
    return true;
}

/* Stage-1 alpha mask over UV1. An op that reads an empty texture slot
   compiles to a CURRENT pass-through (0x352e46), so an unbound mask is a no-op.
   kill_only: the stage's ops ignore its texture, which applies only its
   ALPHAKILL at UV0 (FUN_0027be10 binds it at 0x27c370). */
static void stage1_mask(uint32_t device, const RecompD3dVertexLayout *layout, uint32_t stage[][32],
    bool kill_only, RecompD3dPresenterDrawCommand *draw)
{
    RecompD3dPresenterDrawCommand mask = {0};
    attach_texture(device, 1u, &mask);
    if (!mask.has_texture) return;
    if ((kill_only ? layout->texcoord_count == 0u || stage[1][28] != 0u
                   : layout->texcoord_count != 2u || stage[1][28] != 1u) ||
        stage[1][21] != 0u || stage[1][20] != 0u ||
        mask.texture.linear || mask.palette_bytes != NULL || stage[1][3] != 2u || stage[1][4] != 2u ||
        stage[1][9] != 0u || stage[1][10] != 0u || (stage[1][11] != 0u && stage[1][11] != 4u) ||
        (mask.texture.mip_levels > 1u && stage[1][5] != 1u && stage[1][5] != 2u))
        recomp_stop(2, "d3d8:stage1-mask:tc=%u:tci=%x:ttf=%x:fmt=%x,%u,%u:filter=%u,%u,%u:kill=%u",
            layout->texcoord_count, stage[1][28], stage[1][21], mask.texture.format_byte,
            mask.texture.linear, mask.palette_bytes != NULL, stage[1][3], stage[1][4], stage[1][5],
            stage[1][11]);
    draw->reflection_texture = mask.texture;
    draw->reflection_bytes = mask.texture_bytes;
    draw->reflection_byte_count = mask.texture_byte_count;
    draw->reflection_address_u = mask.address_u;
    draw->reflection_address_v = mask.address_v;
    draw->reflection_point_mip_filter = stage[1][5] == 1u;
    draw->reflection_mip_lod_bias = lod_bias(stage[1][6]);
    draw->stage1_alpha_mask = true;
    draw->stage1_alpha_kill = stage[1][11] == 4u;
    draw->stage1_kill_only = kill_only;
}

/* Tina's Story ending (0x257ee0) adds up to three projected light textures to stage
   0's: stages 1-3 ADD(TEXTURE, CURRENT) over camera-space position, COUNT3|PROJECTED,
   each through its own TEXTUREn. A DISABLE stage ends the chain, and a stage without
   a texture compiles to a CURRENT pass-through (0x352e46). Stages share the three 2D
   slots by texture and sampler state, so a fourth distinct pair stops. */
static void projected_stages(uint32_t device, const uint32_t stage[4][32],
    RecompD3dPresenterDrawCommand *draw)
{
    /* Lighting was skipped, so stage 0 must not read lit RGB: SELECTARG1(TEXTURE) color
       and SELECTARG1(DIFFUSE or CURRENT) alpha. */
    const bool lit = *recomp_memory_u32(0x3553f8u) != 0u;
    if (stage[0][21] != 0x103u || stage[0][7] != 0u || draw->texture.linear ||
        (lit && (stage[0][12] != 2u || stage[0][14] != 2u || stage[0][16] != 2u || stage[0][18] > 1u)) ||
        draw->texture.volume_depth != 0u || draw->palette_bytes != NULL ||
        draw->modulate_texture_factor || draw->modulate_texture_factor_alpha ||
        draw->texture_factor_modulate || draw->texture_factor_color_scale != 0u)
        recomp_stop(2, "d3d8:projected-stages:%x:%u:%x", stage[0][21], stage[0][7], draw->texture.format_byte);
    struct Slot {
        RecompD3dTextureDesc texture;
        const void *bytes;
        uint32_t byte_count, u, v;
        bool point;
        float bias;
    } slots[3] = {{draw->texture, draw->texture_bytes, draw->texture_byte_count,
        draw->address_u, draw->address_v, draw->point_mip_filter, draw->mip_lod_bias}};
    uint32_t used = 1u;
    for (uint32_t i = 1u; i < 4u && stage[i][12] != 1u; ++i) {
        const uint32_t *s = stage[i];
        if (s[12] != 7u || s[14] != 2u || s[15] != 1u || s[16] != 2u || s[18] != 1u ||
            s[21] != 0x103u || s[28] != 0x20000u || s[7] != 0u || s[11] != 0u || s[3] != 2u || s[4] != 2u)
            recomp_stop(2, "d3d8:projected-stage:%u:%u,%u,%u/%u,%u:%x,%x:maxmip=%u:kill=%u:filter=%u,%u",
                i, s[12], s[14], s[15], s[16], s[18], s[21], s[28], s[7], s[11], s[3], s[4]);
        draw->projected_stage_count = i;
        draw->projected_slots[i - 1u] = -1;
        RecompD3dPresenterDrawCommand bound = {0};
        attach_texture(device, i, &bound);
        if (!bound.has_texture) continue;
        const RecompD3dTextureDesc *t = &bound.texture;
        if (t->linear || bound.palette_bytes != NULL || t->format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 ||
            (t->mip_levels > 1u && s[5] != 1u && s[5] != 2u))
            recomp_stop(2, "d3d8:projected-stage-texture:%u:%x:mip=%u", i, t->format_byte, s[5]);
        const struct Slot wanted = {*t, bound.texture_bytes, bound.texture_byte_count,
            bound.address_u, bound.address_v, s[5] == 1u, lod_bias(s[6])};
        uint32_t slot = 0u;
        while (slot < used && !(slots[slot].texture.data == t->data &&
            slots[slot].texture.format_byte == t->format_byte && slots[slot].texture.width == t->width &&
            slots[slot].texture.height == t->height && slots[slot].texture.mip_levels == t->mip_levels &&
            slots[slot].u == wanted.u && slots[slot].v == wanted.v &&
            slots[slot].point == wanted.point && slots[slot].bias == wanted.bias)) ++slot;
        if (slot == 3u) recomp_stop(2, "d3d8:projected-stage-slots");
        if (slot == used) slots[used++] = wanted;
        draw->projected_slots[i - 1u] = (int32_t)slot;
        float *m = draw->projected_transforms[i - 1u];
        recomp_guest_load(m, device + 0xce0u + i * 64u, 64u);
        for (uint32_t k = 0u; k < 16u; ++k)
            if (k % 4u < 3u && !isfinite(m[k])) recomp_stop(2, "d3d8:projected-stage-transform:%u", i);
    }
    if (used > 1u) {
        draw->reflection_texture = slots[1].texture;
        draw->reflection_bytes = slots[1].bytes;
        draw->reflection_byte_count = slots[1].byte_count;
        draw->reflection_address_u = slots[1].u;
        draw->reflection_address_v = slots[1].v;
        draw->reflection_point_mip_filter = slots[1].point;
        draw->reflection_mip_lod_bias = slots[1].bias;
    }
    if (used > 2u) {
        draw->reflection_stage2_texture = slots[2].texture;
        draw->reflection_stage2_bytes = slots[2].bytes;
        draw->reflection_stage2_byte_count = slots[2].byte_count;
        draw->reflection_stage2_address_u = slots[2].u;
        draw->reflection_stage2_address_v = slots[2].v;
        draw->reflection_stage2_point_mip_filter = slots[2].point;
        draw->reflection_stage2_mip_lod_bias = slots[2].bias;
    }
    if (lit) {
        /* Lit RGB is never read; V0.a is the material diffuse alpha (0x34fc80). */
        float alpha;
        recomp_guest_load(&alpha, device + 0xf0cu, sizeof alpha);
        if (isnan(alpha)) alpha = signbit(alpha) ? 0.0f : 1.0f;
        if (!isfinite(alpha)) recomp_stop(2, "d3d8:projected-stages-material-alpha");
        draw->material_alpha = alpha;
        draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
    }
}

static bool fixed_stages(uint32_t device, const RecompD3dVertexLayout *layout,
    RecompD3dPresenterDrawCommand *draw)
{
    uint32_t stage[4][32];
    recomp_guest_load(stage, STAGES, sizeof stage);
    draw->use_texture_factor = recomp_d3d_texture_factor_selected(stage[0][12], stage[0][14],
        stage[0][16], stage[0][18]);
    if (!draw->use_texture_factor && draw->has_texture) {
        if (layout->texcoord_count == 0u || stage[0][0] < 1u || stage[0][0] > 3u ||
            stage[0][1] < 1u || stage[0][1] > 3u || stage[0][3] != 2u || stage[0][4] != 2u ||
            stage[0][9] != 0u || stage[0][10] != 0u ||
            /* ALPHAKILL is DISABLE (0) or ENABLE (4), before the texture combine. */
            (stage[0][11] != 0u && stage[0][11] != 4u) ||
            (draw->texture.mip_levels > 1u && stage[0][5] != 1u && stage[0][5] != 2u))
            recomp_stop(2, "d3d8:draw-texture-sampler:%u,%u:%u,%u,%u:%u,%u,%u:mips=%u:tc=%u",
                stage[0][0], stage[0][1], stage[0][3], stage[0][4], stage[0][5], stage[0][9],
                stage[0][10], stage[0][11], draw->texture.mip_levels, layout->texcoord_count);
        draw->alpha_kill = stage[0][11] == 4u;
        draw->point_mip_filter = stage[0][5] == 1u;
        draw->mip_lod_bias = lod_bias(stage[0][6]);
        const bool color_modulate = stage[0][12] == 4u && stage[0][14] == 2u &&
            (stage[0][15] == 0u || stage[0][15] == 1u);
        const bool alpha_modulate = stage[0][16] == 4u && stage[0][18] == 2u &&
            (stage[0][19] == 0u || stage[0][19] == 1u);
        const bool texture_alpha = stage[0][16] == 2u && stage[0][18] == 2u;
        if (texture_alpha) draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE;
        const bool white = layout->diffuse_offset == RECOMP_D3D_FVF_ABSENT;
        const bool vertex_alpha = !white &&
            ((stage[0][16] == 2u && stage[0][18] <= 1u) ||
             (stage[0][16] == 3u && stage[0][19] <= 1u));
        if (vertex_alpha) draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
        /* Xbox D3D compiles stage-0 ALPHAOP DISABLE as R0.a = V0.a * 1 (0x352b1e), and
           the final combiner reads R0.a, so it matches SELECTARG1(DIFFUSE). */
        const bool diffuse_alpha = stage[0][16] == 1u;
        if (diffuse_alpha) {
            draw->material_alpha_mode = white ? RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE
                : RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
            draw->material_alpha = 1.0f;
        }
        /* Scaled texture-times-factor reads no diffuse; this path requires white vertices. */
        if (white && (stage[0][12] == 5u || stage[0][12] == 6u) &&
            stage[0][14] == 2u && stage[0][15] == 3u)
            draw->texture_factor_color_scale = stage[0][12] == 5u ? 2u : 4u;
        /* The trail strips (setup 0x23a42d, draw 0x23ca7e) use 1x MODULATE. */
        draw->texture_factor_modulate = !draw->directional.enabled && white && stage[0][12] == 4u &&
            ((stage[0][14] == 2u && stage[0][15] == 3u) || (stage[0][14] == 3u && stage[0][15] == 2u));
        const bool white_alpha = white && layout->pretransformed &&
            recomp_d3d_texture_material_alpha_mode(stage[0][12], stage[0][14], stage[0][15],
                stage[0][16], stage[0][18], stage[0][19]) == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
        if (white_alpha) {
            draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
            draw->material_alpha = 1.0f;
        }
        /* Unlit texture select or texture-times-factor over vertex color uses
           the same per-channel selection as lighting; factor replaces diffuse. */
        bool alpha_tfactor = false;
        const bool factor_color = stage[0][12] == 4u &&
            ((stage[0][14] == 2u && stage[0][15] == 3u) || (stage[0][14] == 3u && stage[0][15] == 2u));
        const bool factor_alpha = stage[0][16] == 4u &&
            ((stage[0][18] == 2u && stage[0][19] == 3u) || (stage[0][18] == 3u && stage[0][19] == 2u));
        const bool unlit_select = !draw->directional.enabled && !draw->has_reflection &&
            (!white || (factor_color && factor_alpha) || stage[0][12] == 1u) &&
            (factor_color || stage[0][12] == 1u || (stage[0][12] == 2u && stage[0][14] == 2u)) &&
            stage0_select(stage[0], draw->directional.stage, factor_color ? NULL : &alpha_tfactor);
        if (unlit_select) {
            draw->directional.stage_mode = true;
            draw->texture_factor_modulate = false;
            draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_NONE;
        } else alpha_tfactor = false;
        if (!draw->directional.enabled && !draw->has_reflection && !unlit_select &&
            (!(color_modulate || draw->texture_factor_color_scale || draw->texture_factor_modulate ||
               (white && stage[0][12] == 2u && stage[0][14] == 2u)) ||
             !(alpha_modulate || white_alpha || texture_alpha || vertex_alpha || diffuse_alpha)))
            recomp_stop(2, "d3d8:draw-texture-combine:%u,%u,%u/%u,%u,%u:fvf=%x:ret=%08x",
                stage[0][12], stage[0][14], stage[0][15], stage[0][16], stage[0][18], stage[0][19],
                draw->fvf, *recomp_memory_u32(recomp_runtime.registers.esp));
        /* TEXTURETRANSFORMFLAGS COUNT2 over TCI_CAMERASPACEREFLECTIONVECTOR, alone
           or under a reflection material. */
        const bool texgen = stage[0][21] == 2u && stage[0][28] == 0x30000u &&
            !layout->pretransformed;
        /* COUNT2 over 2D UV0: the texture-matrix upload (0x34f7d0, case 0x220)
           emits s = (m00, m10, 0, m20), t = (m01, m11, 0, m21) and q = 1. */
        const bool uv_transform = (stage[0][21] == 2u ||
            (draw->texture.volume_depth != 0u && stage[0][21] == 3u)) && stage[0][28] == 0u &&
            !draw->has_reflection && !layout->pretransformed;
        /* SDK 0x34f7d0: COUNT2/3 sample 2D S/T with Q=1; COUNT3|PROJECTED
           takes Q from column 2. Volume COUNT3 also consumes column 2 as R. */
        const bool camera_position = (stage[0][21] == 2u || stage[0][21] == 3u || stage[0][21] == 0x103u) &&
            stage[0][28] == 0x20000u &&
            (draw->texture.volume_depth == 0u || stage[0][21] == 3u) &&
            !draw->has_reflection && !layout->pretransformed && layout->blend_weight_count == 0u;
        if (stage[0][20] != 0u ||
            (!texgen && !uv_transform && !camera_position && (stage[0][21] != 0u || stage[0][28] != 0u)))
            recomp_stop(2, "d3d8:draw-texture-coordinates:fvf=%x:flags=%x,%x:tci=%x:reflection=%u:pre=%u:blend=%u:ret=%08x",
                draw->fvf, stage[0][20], stage[0][21], stage[0][28], draw->has_reflection,
                layout->pretransformed, layout->blend_weight_count,
                *recomp_memory_u32(recomp_runtime.registers.esp));
        if (texgen || camera_position) {
            /* Both stages generating: stage 1 keeps TEXTURE1 (FUN_00289680 sets only TEXTURE0). */
            if (draw->has_reflection && !draw->reflection_mesh_uv)
                memcpy(draw->reflection_stage1_transform, draw->reflection_transform,
                    sizeof draw->reflection_stage1_transform);
            /* COUNT2 (case 0x320) uploads columns 0-1 and zeros; COUNT3 adds column 2. */
            if (!reflection_inputs(device, 0xce0u, stage[0][21] & 0xffu,
                texgen && layout->normal_offset != RECOMP_D3D_FVF_ABSENT, draw)) return false;
            draw->texgen_reflection = texgen;
            draw->texgen_projected = camera_position;
            if (camera_position && draw->texture.volume_depth == 0u && (stage[0][21] & 0x100u) == 0u) {
                draw->reflection_transform[2] = draw->reflection_transform[6] =
                    draw->reflection_transform[10] = 0.0f;
                draw->reflection_transform[14] = 1.0f;
            }
        }
        if (uv_transform) {
            recomp_guest_load(draw->reflection_transform, device + 0xce0u, sizeof draw->reflection_transform);
            for (uint32_t i = 0u; i < 16u; ++i)
                if (!isfinite(draw->reflection_transform[i])) recomp_stop(2, "d3d8:texture-transform");
            if (stage[0][21] == 2u) {
                draw->reflection_transform[2] = draw->reflection_transform[6] =
                    draw->reflection_transform[10] = draw->reflection_transform[14] = 0.0f;
            }
            draw->texture_transform_uv = true;
        }
        if (stage[0][12] == 1u) {
            draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_NONE;
            return true;
        }
        const uint32_t final[6] = {stage[1][12], stage[1][14], stage[1][15],
            stage[1][16], stage[1][18], stage[1][19]};
        draw->modulate_texture_factor = recomp_d3d_texture_factor_modulate_selected(final, stage[2][12]);
        /* Stage 1 keeps CURRENT color and modulates its alpha by TFACTOR or
           by its own texture (FUN_0027be10, FUN_0027c400). */
        const bool stage1_alpha = !draw->has_reflection && stage[1][12] == 2u && stage[1][14] == 1u &&
            stage[1][16] == 4u && stage[1][19] == 1u && stage[2][12] == 1u &&
            ((stage[1][18] == 3u && !alpha_tfactor && (stage[1][11] == 0u || stage[1][11] == 4u)) ||
             stage[1][18] == 2u);
        if (stage1_alpha && stage[1][18] == 3u) alpha_tfactor = true;
        if (stage1_alpha && (stage[1][18] == 2u || stage[1][11] == 4u))
            stage1_mask(device, layout, stage, stage[1][18] == 3u, draw);
        draw->modulate_texture_factor_alpha = alpha_tfactor;
        /* The presenter applies one texture-factor mode per draw. */
        if ((alpha_tfactor && (draw->modulate_texture_factor || draw->texture_factor_modulate ||
                draw->texture_factor_color_scale)) ||
            (draw->modulate_texture_factor && draw->texture_factor_modulate))
            recomp_stop(2, "d3d8:texture-factor-twice");
        if (draw->texgen_projected && stage[1][12] == 7u)
            projected_stages(device, (const uint32_t (*)[32])stage, draw);
        else if (stage[1][12] != 1u && !draw->modulate_texture_factor && !draw->has_reflection && !stage1_alpha)
            recomp_stop(2, "d3d8:draw-texture-stages:%u,%u,%u/%u,%u,%u:%u,%u,%u/%u,%u,%u:%u,%u:"
                "s2=%u,%u/%u,%u:s3=%u:t2=%u:addr=%u,%u,%u,%u:kill=%u,%u:tc=%x,%x,%x,%x:tc2=%x,%x,%x",
                stage[0][12], stage[0][14], stage[0][15], stage[0][16], stage[0][18], stage[0][19],
                stage[1][12], stage[1][14], stage[1][15], stage[1][16], stage[1][18], stage[1][19],
                stage[2][12], stage[2][16], stage[2][14], stage[2][15], stage[2][18], stage[2][19],
                stage[3][12], *recomp_memory_u32(device + 0xf90u) != 0u, stage[0][0], stage[0][1],
                stage[1][0], stage[1][1], stage[0][11], stage[1][11], stage[0][21], stage[0][28],
                stage[1][21], stage[1][28], stage[2][21], stage[2][28], stage[3][21]);
    }
    return true;
}

/* A bound 5849 vertex program (see set_vertex_program). Code comes from the
   CreateVertexShader object, constants from the 192-entry shadow 0x354440
   written by SetVertexShaderConstant1/4 (0x348de0/0x348e40). c58/c59 hold the
   viewport scale and offset as GetViewportOffsetAndScale (0x3462d0) computes
   them. shortcut: only the observed declarations, as FVFs: position/normal/UV
   (0x112), the projected shadow's position/normal (0x7af4d0, 0x012),
   position/color/UV (0x7ae238, 0x142) and the stage effect's position/color
   with four UV sets v9-v12 (0x7afd0c, 0x442); admit others when a run
   reaches them. */
static void attach_program(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    const uint32_t handle = draw->fvf;
    uint32_t words[0x114u / 4u];
    recomp_guest_load(words, handle - 1u, sizeof words);
    const bool normal = words[5u + 2u * 4u + 2u] != 2u, color = words[5u + 3u * 4u + 2u] != 2u;
    uint32_t texcoords = 0u;
    while (texcoords < 4u && words[5u + (9u + texcoords) * 4u + 2u] != 2u) ++texcoords;
    const uint32_t fvf = 0x002u | (normal ? 0x010u : 0u) | (color ? 0x040u : 0u) | texcoords << 8u;
    for (uint32_t i = 0u; i < 16u; ++i) {
        const uint32_t *slot = words + 5u + i * 4u;
        const uint32_t type = i == 0u || (i == 2u && normal) ? 0x32u : i == 3u && color ? 0x40u :
            i >= 9u && i < 9u + texcoords ? 0x22u : 2u;
        const uint32_t offset = i == 0u ? 0u :
            i >= 9u ? 12u + (normal ? 12u : 0u) + (color ? 4u : 0u) + (i - 9u) * 8u : 12u;
        if ((fvf != 0x112u && fvf != 0x012u && fvf != 0x142u && fvf != 0x442u) || slot[2] != type ||
            (type != 2u && (slot[0] != 0u || slot[1] != offset))) {
            char bound[16u * 20u] = "";
            size_t used = 0u;
            for (uint32_t r = 0u; r < 16u && used < sizeof bound - 20u; ++r) {
                const uint32_t *in = words + 5u + r * 4u;
                if (in[2] != 2u)
                    used += (size_t)snprintf(bound + used, sizeof bound - used, ",%u:%x@%u/%u",
                        r, in[2], in[1], in[0]);
            }
            recomp_stop(2, "d3d8:vertex-program-declaration:%08x:%u:%s", handle, i, bound);
        }
    }
    if (words[3] > 680u || !recomp_d3d_unpack_vertex_program(
            (const uint32_t *)(const void *)recomp_memory(handle - 1u + 0x114u, words[3] * 4u),
            words[3], words[2], draw->program))
        recomp_stop(2, "d3d8:vertex-program-unsupported:%08x", handle);
    draw->program_count = words[2];
    recomp_guest_load(draw->program_constants, 0x354440u, sizeof draw->program_constants);
    RecompD3dViewport viewport;
    float scale[2], framebuffer[2], offset[2], depth;
    recomp_guest_load(&viewport, device + 0xee0u, sizeof viewport);
    recomp_guest_load(scale, device + 0x95cu, sizeof scale);
    recomp_guest_load(framebuffer, device + 0x96cu, sizeof framebuffer);
    recomp_guest_load(&depth, device + 0x948u, sizeof depth);
    recomp_guest_load(offset, device + 0xef8u, sizeof offset);
    /* The host draws the back buffer at its logical size, without the
       supersample factor the SDK applies to the guest viewport transform. */
    if (!draw->target.offscreen) {
        scale[0] /= framebuffer[0];
        scale[1] /= framebuffer[1];
    }
    if ((*recomp_memory_u32(device + 8u) & 0x8000u) != 0u && *recomp_memory_u32(0x3554c0u) != 0u) {
        offset[0] -= 0.5f;
        offset[1] -= 0.5f;
    }
    float *s = draw->program_constants[58], *o = draw->program_constants[59];
    s[0] = (float)viewport.width * scale[0] * 0.5f;
    s[1] = (float)viewport.height * scale[1] * -0.5f;
    s[2] = (viewport.max_z - viewport.min_z) * depth;
    s[3] = 0.0f;
    o[0] = s[0] + (float)viewport.x * scale[0] + offset[0];
    o[1] = -s[1] + (float)viewport.y * scale[1] + offset[1];
    o[2] = viewport.min_z * depth;
    o[3] = 0.0f;
    for (uint32_t i = 0u; i < 3u; ++i)
        if (!isfinite(s[i]) || !isfinite(o[i])) recomp_stop(2, "d3d8:vertex-program-viewport");
    draw->fvf = fvf;
}

/* The presenter's program path samples stage 0 and optionally masks alpha
   with stage 1, as in DOA3's water passes. Other combiners stop. */
static void program_stages(uint32_t device, RecompD3dPresenterDrawCommand *draw)
{
    uint32_t stage[3][32];
    recomp_guest_load(stage, STAGES, sizeof stage);
    const uint32_t alpha_mask[6] = {2u, 1u, 0u, 4u, 1u, 2u};
    const uint32_t fields[6] = {12u, 14u, 15u, 16u, 18u, 19u};
    uint32_t args[2][6];
    for (uint32_t i = 0u; i < 2u; ++i)
        for (uint32_t k = 0u; k < 6u; ++k) args[i][k] = stage[i][fields[k]];
    const bool mask_combiner = memcmp(args[1], alpha_mask, sizeof args[1]) == 0;
    /* Stage 0 selects texture and oD0 per channel; alpha may MODULATE by
       TFACTOR (the projected shadow FUN_0027a760, the effect FUN_002718e0). */
    bool alpha_tfactor = false;
    const bool select_texture = stage[0][12] != 1u &&
        stage0_select(stage[0], draw->directional.stage, &alpha_tfactor);
    draw->directional.stage_mode = true;
    draw->modulate_texture_factor_alpha = alpha_tfactor;
    /* 5849's combiner compiler (0x352e46) compiles an op that reads a NULL
       texture as a CURRENT pass-through, so an unbound mask leaves alpha as is. */
    draw->program_alpha_mask = mask_combiner && *recomp_memory_u32(device + 0xf8cu) != 0u;
    const uint32_t pixel = *recomp_memory_u32(device + PIXEL_HANDLE);
    if (pixel != 0u || !draw->has_texture ||
        !select_texture || stage[2][12] != 1u ||
        (args[1][0] != 1u && !mask_combiner))
        recomp_stop(2, "d3d8:vertex-program-stages:%u:%u:%u:%u:%u:%u:%u:ps=%08x:n=%u", args[0][0],
            args[0][1], args[0][3], args[0][4], args[0][5], args[1][0], stage[2][12], pixel,
            draw->program_count);
    if (draw->program_alpha_mask) {
        RecompD3dPresenterDrawCommand mask = {0};
        attach_texture(device, 1u, &mask);
        if (mask.palette_bytes != NULL) recomp_stop(2, "d3d8:vertex-program-mask-palette");
        draw->reflection_texture = mask.texture;
        draw->reflection_bytes = mask.texture_bytes;
        draw->reflection_byte_count = mask.texture_byte_count;
    }
    /* Admitted: linear min/mag, point mip on the mask, point or linear mip on
       stage 0, optional stage-0 zero-alpha kill, no color key, sign or transform. */
    for (uint32_t i = 0u; i < (draw->program_alpha_mask ? 2u : 1u); ++i) {
        const uint32_t *t = stage[i];
        if ((i == 1u && t[0] != t[1]) || t[0] < 1u || t[0] > 3u || t[1] < 1u || t[1] > 3u ||
            t[3] != 2u || t[4] != 2u ||
            (i == 1u && t[5] != 1u) || t[7] != 0u || t[9] != 0u || t[10] != 0u ||
            (t[11] != 0u && (i != 0u || t[11] != 4u)) ||
            t[21] != 0u || (i == 0u && draw->texture.mip_levels != 1u && t[5] != 1u && t[5] != 2u)) {
            recomp_stop(2, "d3d8:vertex-program-sampler:%u:uv=%u,%u:filter=%u,%u,%u:ttf=%x:"
                "maxmip=%u:key=%x:sign=%x:kill=%u", i, t[0], t[1], t[3], t[4], t[5], t[21],
                t[7], t[9], t[10], t[11]);
        }
        if (i == 1u) memcpy(&draw->program_mask_lod_bias, &t[6], sizeof draw->program_mask_lod_bias);
    }
    draw->alpha_kill = stage[0][11] == 4u;
    draw->point_mip_filter = stage[0][5] == 1u;
    draw->mip_lod_bias = lod_bias(stage[0][6]);
    if (!isfinite(draw->program_mask_lod_bias) || draw->program_mask_lod_bias < -16.0f ||
        draw->program_mask_lod_bias > 15.99f)
        recomp_stop(2, "d3d8:vertex-program-lod-bias");
}

static float line_width(void)
{
    float width;
    recomp_guest_load(&width, 0x3554d4u, sizeof width);
    return fminf(floorf(width * 8.0f + 0.5f), 511.0f) / 8.0f;
}

/* D3D11 rasterizes one-pixel lines only. A wider line list becomes quads in
   screen space, widened across the minor axis like aliased NV2A lines, with
   the width the SDK programs (0x346760: 1/8-pixel steps, at most 0x1ff). */
static uint8_t *widen_lines(bool transformed, RecompD3dPresenterDrawCommand *draw)
{
    const float width = line_width();
    if (width <= 1.0f) return NULL;
    /* shortcut: other transformed layouts still need the native line path;
       XYZ|DIFFUSE lines use the presenter's clip-space expansion instead. */
    const uint32_t lines = draw->index_count / 2u, stride = draw->vertex_stride;
    if (transformed || lines > 16384u) recomp_stop(2, "d3d8:draw-wide-lines:%u", lines);
    uint8_t *storage = malloc((size_t)lines * (12u + 4u * stride));
    if (storage == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
    uint16_t *indices = (uint16_t *)(void *)storage;
    uint8_t *quads = storage + (size_t)lines * 12u;
    const uint16_t *source = draw->index_bytes;
    const uint8_t *vertices = draw->vertex_bytes;
    static const uint16_t corner[6] = {0u, 1u, 3u, 0u, 3u, 2u};
    for (uint32_t i = 0u; i < lines; ++i) {
        float a[2], b[2];
        memcpy(a, vertices + source[i * 2u] * stride, sizeof a);
        memcpy(b, vertices + source[i * 2u + 1u] * stride, sizeof b);
        const uint32_t axis = fabsf(b[0] - a[0]) >= fabsf(b[1] - a[1]) ? 1u : 0u;
        for (uint32_t k = 0u; k < 4u; ++k) {
            uint8_t *vertex = quads + (i * 4u + k) * stride;
            float position;
            memcpy(vertex, vertices + source[i * 2u + k / 2u] * stride, stride);
            memcpy(&position, vertex + axis * 4u, sizeof position);
            position += (k & 1u ? 0.5f : -0.5f) * width;
            memcpy(vertex + axis * 4u, &position, sizeof position);
        }
        for (uint32_t k = 0u; k < 6u; ++k) indices[i * 6u + k] = (uint16_t)(i * 4u + corner[k]);
    }
    draw->vertex_bytes = quads;
    draw->vertex_count = lines * 4u;
    draw->index_bytes = indices;
    draw->index_count = lines * 6u;
    draw->primitive_type = 5u;
    draw->triangle_count = lines * 2u;
    draw->cull_mode = RECOMP_D3D_CULL_NONE;
    return storage;
}

/* Xbox point sprites (POINTSPRITEENABLE 0x355438). The 5849 combiner compiler
   (0x352a60) then compiles only stage 3, whose texture gets the sprite
   coordinates. Each point becomes an eye-space quad, square in pixels, drawn
   with the projection alone. POINTSCALEENABLE sizes follow D3D8, as the SDK's
   point parameters (0x34ef10) encode them: Vh * Si / sqrt(A + B*De + C*De^2),
   clamped to [MIN, MAX]. */
static uint8_t *point_sprites(uint32_t device, const RecompD3dVertexLayout *layout,
    RecompD3dPresenterDrawCommand *draw)
{
    float point[8];
    recomp_guest_load(point, 0x355430u, sizeof point);
    const uint32_t *stage = recomp_memory_u32(STAGES + 3u * 128u);
    const uint32_t points = draw->index_count;
    /* shortcut: unscaled sizes multiply by device+0x964, unverified here; bind them when a caller needs one. */
    if (*recomp_memory_u32(0x355438u) == 0u || *recomp_memory_u32(0x35543cu) == 0u ||
        layout->pretransformed || layout->blend_weight_count != 0u || points > 16384u ||
        !isfinite(point[0]) || !(point[1] >= 0.0f) || !(point[7] >= point[1]))
        recomp_stop(2, "d3d8:draw-points-unbound");
    /* SELECTARG1 of TEXTURE or DIFFUSE, or their MODULATE. The presenter modulates both,
       so a texture-only channel whitens diffuse and a diffuse-only color whitens the texel. */
    const bool color_texture = stage[12] == 2u && stage[14] == 2u;
    const bool color_diffuse = stage[12] == 2u && stage[14] == 0u;
    const bool color_modulate = stage[12] == 4u && ((stage[14] == 2u && stage[15] == 0u) ||
        (stage[14] == 0u && stage[15] == 2u));
    const bool alpha_texture = stage[16] == 2u && stage[18] == 2u;
    const bool alpha_modulate = stage[16] == 4u && ((stage[18] == 2u && stage[19] == 0u) ||
        (stage[18] == 0u && stage[19] == 2u));
    const bool color_factor = stage[12] == 4u && ((stage[14] == 2u && stage[15] == 3u) ||
        (stage[14] == 3u && stage[15] == 2u));
    const bool alpha_factor = stage[16] == 4u && ((stage[18] == 2u && stage[19] == 3u) ||
        (stage[18] == 3u && stage[19] == 2u));
    if (!(color_texture || color_diffuse || color_modulate || color_factor) ||
        !(alpha_texture || alpha_modulate || alpha_factor) ||
        stage[3] != 2u || stage[4] != 2u || stage[9] != 0u || stage[10] != 0u ||
        stage[11] != 0u || stage[20] != 0u || stage[21] != 0u)
        recomp_stop(2, "d3d8:point-sprite-stage:%u,%u,%u/%u,%u,%u:filter=%u,%u:key=%x:sign=%x:"
            "kill=%u:tc=%u,%x:ret=%08x", stage[12], stage[14], stage[15], stage[16], stage[18],
            stage[19], stage[3], stage[4], stage[9], stage[10], stage[11], stage[20], stage[21],
            *recomp_memory_u32(recomp_runtime.registers.esp));
    const bool lighting = *recomp_memory_u32(0x3553f8u) != 0u;
    if (lighting && (!(color_texture || color_factor) || *recomp_memory_u32(0x355494u) != 0u))
        recomp_stop(2, "d3d8:point-sprite-lighting");
    if (color_factor || alpha_factor)
        draw->directional.stage_mode = stage0_select(stage, draw->directional.stage, NULL);
    if (lighting && alpha_modulate) {
        /* 0x34fc80 supplies material diffuse alpha even with vertex-color sources;
           texture-only RGB does not consume the lighting RGB calculation. */
        float alpha;
        recomp_guest_load(&alpha, device + 0xf0cu, sizeof alpha);
        if (isnan(alpha)) alpha = signbit(alpha) ? 0.0f : 1.0f;
        if (!isfinite(alpha)) recomp_stop(2, "d3d8:point-sprite-material-alpha");
        draw->material_alpha = alpha;
        draw->material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE;
    }
    draw->has_texture = false;
    draw->palette_bytes = NULL;
    draw->texture_is_backbuffer = draw->texture_is_front_buffer = false;
    attach_texture(device, 3u, draw);
    if (!draw->has_texture || (draw->texture.mip_levels > 1u && stage[5] != 1u && stage[5] != 2u))
        recomp_stop(2, "d3d8:point-sprite-texture");
    draw->point_mip_filter = stage[5] == 1u;
    draw->mip_lod_bias = lod_bias(stage[6]);
    draw->texture_alpha_only = color_diffuse;

    float world[16], view[16], world_view[16], p[16];
    recomp_guest_load(world, device + 0xde0u, sizeof world);
    recomp_guest_load(view, device + 0xc60u, sizeof view);
    recomp_guest_load(p, device + 0xca0u, sizeof p);
    multiply(world, view, world_view, "point-sprite-world-view");
    /* The quad offsets assume eye x and y reach only clip x and y. */
    if (p[1] != 0.0f || p[3] != 0.0f || p[4] != 0.0f || p[7] != 0.0f || p[0] == 0.0f || p[5] == 0.0f)
        recomp_stop(2, "d3d8:point-sprite-projection");
    const float width = draw->viewport[2], height = draw->viewport[3];
    typedef struct { float x, y, z; uint32_t diffuse; float u, v; } Corner;
    uint8_t *storage = malloc((size_t)points * (12u + 4u * sizeof(Corner)));
    if (storage == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
    uint16_t *indices = (uint16_t *)(void *)storage;
    Corner *corners = (Corner *)(void *)(storage + (size_t)points * 12u);
    const uint16_t *source = draw->index_bytes;
    const uint8_t *vertices = draw->vertex_bytes;
    static const uint16_t order[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    for (uint32_t i = 0u; i < points; ++i) {
        const uint8_t *vertex = vertices + source[i] * draw->vertex_stride;
        float position[3], eye[3];
        memcpy(position, vertex, sizeof position);
        for (uint32_t c = 0u; c < 3u; ++c)
            eye[c] = position[0] * world_view[c] + position[1] * world_view[4u + c] +
                position[2] * world_view[8u + c] + world_view[12u + c];
        const float distance = sqrtf(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
        const float attenuation = point[4] + point[5] * distance + point[6] * distance * distance;
        float size = attenuation > 0.0f ? height * point[0] / sqrtf(attenuation) : point[7];
        size = fminf(fmaxf(size, point[1]), fminf(point[7], 64.0f));
        const float w = eye[0] * p[3] + eye[1] * p[7] + eye[2] * p[11] + p[15];
        const float dx = w > 0.0f && isfinite(size) ? size * w / (p[0] * width) : 0.0f;
        const float dy = w > 0.0f && isfinite(size) ? size * w / (p[5] * height) : 0.0f;
        uint32_t diffuse = 0xffffffffu;
        if (layout->diffuse_offset != RECOMP_D3D_FVF_ABSENT)
            memcpy(&diffuse, vertex + layout->diffuse_offset, sizeof diffuse);
        if (color_texture) diffuse |= 0x00ffffffu;
        if (alpha_texture || lighting) diffuse |= 0xff000000u;
        for (uint32_t k = 0u; k < 4u; ++k) {
            const float u = (float)(k & 1u), v = (float)(k >> 1u);
            corners[i * 4u + k] = (Corner){eye[0] + (2.0f * u - 1.0f) * dx,
                eye[1] + (1.0f - 2.0f * v) * dy, eye[2], diffuse, u, v};
        }
        for (uint32_t k = 0u; k < 6u; ++k) indices[i * 6u + k] = (uint16_t)(i * 4u + order[k]);
    }
    draw->fvf = 0x142u;
    draw->vertex_stride = sizeof(Corner);
    draw->vertex_bytes = corners;
    draw->vertex_count = points * 4u;
    draw->index_bytes = indices;
    draw->index_count = points * 6u;
    draw->primitive_type = 5u;
    draw->triangle_count = points * 2u;
    draw->cull_mode = RECOMP_D3D_CULL_NONE;
    memcpy(draw->transform, p, sizeof p);
    return storage;
}

/* Stage 1 ADDs a texture over stage 0's camera-space projection, as Tina's Story
   ending (0x257ee0) does on lit meshes without normals. Every stage's color reads
   its texture, and V0 supplies only alpha, which 0x34fc80 sends as material
   diffuse alpha, so lighting is not evaluated (see projected_stages). */
static bool projected_add_stages(uint32_t device)
{
    const uint32_t *stage = recomp_memory_u32(STAGES);
    return *recomp_memory_u32(device + PIXEL_HANDLE) == 0u && stage[28] == 0x20000u &&
        stage[32u + 12u] == 7u && stage[32u + 14u] == 2u && stage[32u + 15u] == 1u &&
        stage[32u + 28u] == 0x20000u;
}

static void submit_draw(uint32_t primitive, uint32_t count, uint32_t vertices,
    uint32_t stride, uint32_t index_address)
{
    const uint32_t device = recomp_d3d8_require_device();
    const bool quads = primitive == 8u;
    if (quads) count &= ~3u;
    /* An even-count quad strip covers the triangles of a strip, in the same winding. */
    if (primitive == 9u) { primitive = 6u; count &= ~1u; }
    /* A line list draws whole pairs; the presenter maps it to D3D11 lines. */
    if (primitive == 2u) count &= ~1u;
    if (count == 0u) return;
    if (count > 65536u || vertices == 0u || stride == 0u)
        recomp_stop(2, "d3d8:draw-span");
    const bool points = primitive == 1u;
    const bool line_strip = primitive == 4u;
    const uint32_t triangles = points ? count * 2u : quads ? count / 2u : primitive == 2u ? count / 2u :
        line_strip ? count - 1u : recomp_d3d_draw_triangle_count(primitive, count);
    if (!points && !quads && !line_strip && primitive != 2u && (primitive < 5u || primitive > 7u))
        recomp_stop(2, "d3d8:draw-primitive:%u", primitive);
    if (triangles == 0u) return;
    RecompD3dPresenterCommand command = {0};
    RecompD3dPresenterDrawCommand *draw = &command.data.draw;
    draw->fvf = *recomp_memory_u32(device + 0x798u);
    const bool program = (draw->fvf & 1u) != 0u;
    recomp_d3d8_target(&draw->target);
    if (program) attach_program(device, draw);
    RecompD3dVertexLayout layout;
    /* The distortion pass's STR coordinate sets (TEXCOORDSIZE3 for sets 1-2). */
    const bool distorted = !program && draw->fvf == 0x140404u && distortion_shader(device);
    if (distorted) draw->fvf = 0x404u;
    /* With no stage-0 texture no texture coordinates are fetched: the menu
       (0xb0600) draws 20-byte XYZRHW|DIFFUSE lines under its quads' FVF 0x144. */
    if (!program && *recomp_memory_u32(device + 0xf88u) == 0u &&
        recomp_d3d_fvf_layout(draw->fvf, &layout) && layout.stride > stride)
        draw->fvf &= ~0xf00u;
    if ((draw->fvf & 1u) != 0u || !recomp_d3d_fvf_layout(draw->fvf, &layout) ||
        (draw->fvf & ~0xfdfu) != 0u)
        recomp_stop(2, "d3d8:draw-vertex-layout:%08x:%u:raw=%08x:program=%u:ret=%08x:prim=%u:n=%u", draw->fvf, stride,
            *recomp_memory_u32(device + 0x798u), draw->program_count,
            *recomp_memory_u32(recomp_runtime.registers.esp), primitive, count);
    for (uint32_t i = 1u; i < 16u; ++i) {
        if (*recomp_memory_u32(STREAMS + i * 12u + 8u) != 0u)
            recomp_stop(2, "d3d8:draw-multiple-streams");
    }
    const uint16_t *indices;
    uint16_t *sequential = NULL;
    uint32_t largest = count - 1u;
    uint32_t smallest = 0u;
    if (index_address != 0u) {
        indices = (const uint16_t *)(const void *)recomp_memory(index_address, count * 2u);
        largest = 0u;
        smallest = 0xffffu;
        for (uint32_t i = 0u; i < count; ++i) {
            if (indices[i] > largest) largest = indices[i];
            if (indices[i] < smallest) smallest = indices[i];
        }
    } else {
        sequential = malloc(count * sizeof *sequential);
        if (sequential == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
        for (uint32_t i = 0u; i < count; ++i) sequential[i] = (uint16_t)i;
        indices = sequential;
    }
    if (quads) {
        /* shortcut: use diagonal 0-2, verify the hardware split if quad interpolation differs. */
        static const uint32_t order[6] = {0u, 1u, 2u, 0u, 2u, 3u};
        uint16_t *expanded_indices = malloc(triangles * 3u * sizeof *expanded_indices);
        if (expanded_indices == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
        for (uint32_t i = 0u; i < count / 4u; ++i)
            for (uint32_t j = 0u; j < 6u; ++j)
                expanded_indices[i * 6u + j] = indices[i * 4u + order[j]];
        free(sequential);
        sequential = expanded_indices;
        indices = sequential;
        primitive = 5u;
        count = triangles * 3u;
    }
    if (line_strip) {
        /* Keep adjacent pairs in order for the existing width/antialias path. */
        uint16_t *pairs = malloc(triangles * 2u * sizeof *pairs);
        if (pairs == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
        for (uint32_t i = 0u; i < triangles; ++i) {
            pairs[i * 2u] = indices[i];
            pairs[i * 2u + 1u] = indices[i + 1u];
        }
        free(sequential);
        sequential = pairs;
        indices = pairs;
        primitive = 2u;
        count = triangles * 2u;
    }
    /* Stage meshes index a few hundred vertices deep inside shared buffers;
       send only the indexed range (about 10 of 12 MB per frame otherwise). */
    if (smallest != 0u) {
        if (sequential == NULL) {
            sequential = malloc(count * sizeof *sequential);
            if (sequential == NULL) recomp_stop(2, "d3d8:draw-index-allocation");
            memcpy(sequential, indices, count * sizeof *sequential);
            indices = sequential;
        }
        for (uint32_t i = 0u; i < count; ++i) sequential[i] = (uint16_t)(sequential[i] - smallest);
        vertices += smallest * stride;
        largest -= smallest;
    }
    draw->vertex_count = largest + 1u;
    const uint32_t bytes = recomp_d3d_draw_vertex_bytes(stride, largest);
    const uint32_t tail = layout.stride > stride ? layout.stride - stride : 0u;
    if (bytes == 0u || bytes > RECOMP_XBOX_RAM_SIZE - tail) recomp_stop(2, "d3d8:draw-vertex-span");
    draw->vertex_bytes = recomp_memory(vertices, bytes + tail);
    draw->index_bytes = indices;
    draw->index_count = count;
    draw->vertex_stride = stride;
    uint8_t *packed = NULL;
    if (tail != 0u) {
        /* SetStateUP (0x3506f0) and DrawVerticesUP copy each declaration, then
           add stride - declaration size; overlapping records are valid. */
        packed = malloc((size_t)layout.stride * draw->vertex_count);
        if (packed == NULL) recomp_stop(2, "d3d8:draw-vertex-allocation");
        for (uint32_t i = 0u; i < draw->vertex_count; ++i)
            memcpy(packed + i * layout.stride, (const uint8_t *)draw->vertex_bytes + i * stride, layout.stride);
        draw->vertex_bytes = packed;
        draw->vertex_stride = layout.stride;
    }
    draw->primitive_type = primitive;
    draw->triangle_count = triangles;
    if (!layout.pretransformed && !program) {
        float view[16], projection[16], world[16], vp[16], blend[3][16];
        recomp_guest_load(view, device + 0xc60u, sizeof view);
        recomp_guest_load(projection, device + 0xca0u, sizeof projection);
        recomp_guest_load(world, device + 0xde0u, sizeof world);
        if (layout.blend_weight_count > 3u) recomp_stop(2, "d3d8:draw-vertex-blend");
        recomp_guest_load(blend, device + 0xe20u, layout.blend_weight_count * sizeof blend[0]);
        /* The game occasionally poses a mesh at a NaN position (seen after fights,
           ret 0x28708b). NV2A vertices then become NaN and are clipped, so the
           draw shows nothing; drop it instead of stopping. */
        uint32_t nonfinite = 0u;
        for (uint32_t i = 0u; i < 16u; ++i) {
            nonfinite += !isfinite(view[i]) + !isfinite(projection[i]) + !isfinite(world[i]);
            for (uint32_t b = 0u; b < layout.blend_weight_count; ++b) nonfinite += !isfinite(blend[b][i]);
        }
        if (nonfinite != 0u) {
            static uint32_t dropped;
            ++dropped;
            if ((dropped & (dropped - 1u)) == 0u)
                fprintf(stderr, "recomp d3d8: dropped nonfinite-position draws=%u cells=%u ret=%08x\n",
                    dropped, nonfinite, *recomp_memory_u32(recomp_runtime.registers.esp));
            free(sequential);
            free(packed);
            return;
        }
        multiply(view, projection, vp, "view-projection");
        multiply(world, vp, draw->transform, "world");
        draw->has_transform = true;
        draw->blend_weight_count = layout.blend_weight_count;
        if (layout.blend_weight_count != 0u &&
            *recomp_memory_u32(0x355484u) != layout.blend_weight_count * 2u - 1u)
            recomp_stop(2, "d3d8:draw-vertex-blend");
        for (uint32_t i = 0u; i < layout.blend_weight_count; ++i)
            multiply(blend[i], vp, draw->blend_transforms[i], "blend-world");
    }
    RecompD3dViewport viewport;
    recomp_guest_load(&viewport, device + 0xee0u, sizeof viewport);
    const float rect[6] = {(float)viewport.x, (float)viewport.y, (float)viewport.width,
        (float)viewport.height, viewport.min_z, viewport.max_z};
    memcpy(draw->viewport, rect, sizeof rect);
    /* Program draws carry the offset in c59. */
    if (!program) {
        recomp_guest_load(draw->screen_space_offset, device + 0xef8u, sizeof draw->screen_space_offset);
        for (uint32_t i = 0u; i < 2u; ++i) draw->screen_space_offset[i] -= 0.53125f;
    }
    const uint32_t scissors = *recomp_memory_u32(device + 0x1c00u);
    if (scissors > 1u || (scissors != 0u && *recomp_memory_u32(device + 0x1c04u) != 0u))
        recomp_stop(2, "d3d8:draw-scissors");
    draw->has_scissor = true;
    if (scissors != 0u) {
        recomp_guest_load(draw->scissor, device + 0x1b80u, sizeof draw->scissor);
    } else {
        /* SDK count zero clips to the viewport, including pretransformed draws. */
        const int32_t clip[4] = {(int32_t)viewport.x, (int32_t)viewport.y,
            (int32_t)(viewport.x + viewport.width), (int32_t)(viewport.y + viewport.height)};
        memcpy(draw->scissor, clip, sizeof clip);
    }
    const RecompD3dRenderStateModel *state = recomp_d3d8_state_model();
    const bool native_line = primitive == 2u && !program && draw->fvf == 0x42u &&
        *recomp_memory_u32(device + 0xf88u) == 0u && *recomp_memory_u32(0x3553f8u) == 0u;
    /* shortcut: polygon EdgeAntiAlias (the mirror shards at 0x277950) draws
       without edge alpha coverage; host MSAA smooths the edges instead. */
    if ((state->fill_mode != 0u && state->fill_mode != 0x1b02u) ||
        (state->edge_antialias != 0u && !native_line && primitive < 5u) ||
        state->z_enable == 2u)
        recomp_stop(2, "d3d8:draw-raster-state:%x:%x:%x:ret=%08x", state->fill_mode,
            state->edge_antialias, state->z_enable, *recomp_memory_u32(recomp_runtime.registers.esp));
    if (primitive >= 5u && !recomp_d3d_polygon_offset(state,
            &draw->depth_bias, &draw->slope_scaled_depth_bias))
        recomp_stop(2, "d3d8:draw-depth-bias:ret=%08x", *recomp_memory_u32(recomp_runtime.registers.esp));
    recomp_d3d_depth_state(state, &draw->depth);
    recomp_d3d_blend_state(state, &draw->blend);
    draw->cull_mode = state->cull_mode == 0x900u ? RECOMP_D3D_CULL_CLOCKWISE :
        state->cull_mode == 0x901u ? RECOMP_D3D_CULL_COUNTER_CLOCKWISE : RECOMP_D3D_CULL_NONE;
    draw->texture_factor = state->texture_factor;
    attach_texture(device, 0u, draw);
    if (draw->texture.volume_depth != 0u &&
        (program || points || draw->fvf != 0x142u ||
         *recomp_memory_u32(device + PIXEL_HANDLE) != 0u ||
         draw->address_w < 1u || draw->address_w > 3u))
        recomp_stop(2, "d3d8:volume-draw-layout:fvf=%x:w=%u", draw->fvf, draw->address_w);
    const bool unlit_projected = !program && !points && draw->has_texture && projected_add_stages(device);
    if (!program && !points && !unlit_projected) attach_lighting(device, &layout, draw);
    uint8_t *expanded = NULL;
    if (points && (program || *recomp_memory_u32(device + PIXEL_HANDLE) != 0u)) {
        recomp_stop(2, "d3d8:draw-points-unbound");
    } else if (points) {
        expanded = point_sprites(device, &layout, draw);
    } else if (program && bump_shader(device) != 0u) {
        bump_stages(device, bump_shader(device) == 2u, draw);
    } else if (program) {
        program_stages(device, draw);
    } else if (*recomp_memory_u32(device + PIXEL_HANDLE) != 0u) {
        if (distorted) expanded = distortion(device, stride, draw);
        else if (!four_tap(device, draw) && !bright_pass(device, draw) && !luminance_tint(device, draw))
            recomp_stop(2, "d3d8:draw-pixel-shader-unsupported");
    } else {
        if (!attach_reflection(device, &layout, draw) || !fixed_stages(device, &layout, draw)) {
            free(sequential);
            free(packed);
            return;
        }
        if (unlit_projected && draw->projected_stage_count == 0u)
            recomp_stop(2, "d3d8:projected-stages-unrouted");
    }
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    if (native_line) {
        draw->line_width = line_width();
        draw->line_antialias = state->edge_antialias != 0u;
    } else if (primitive == 2u) expanded = widen_lines(program || !layout.pretransformed, draw);
    recomp_d3d8_submit(&command);
    free(sequential);
    free(expanded);
    free(packed);
}

static void draw_up(void)
{
    submit_draw(kernel_arg(1u), kernel_arg(2u), kernel_arg(3u), kernel_arg(4u), 0u);
    recomp_runtime.registers.esp += 20u;
}

static void draw_vertices(void)
{
    uint32_t stride;
    const uint32_t vertices = stream_vertices(kernel_arg(2u), &stride);
    submit_draw(kernel_arg(1u), kernel_arg(3u), vertices, stride, 0u);
    recomp_runtime.registers.esp += 16u;
}

static void draw_indexed(void)
{
    uint32_t stride;
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t vertices = stream_vertices(*recomp_memory_u32(device + 0x1cu), &stride);
    uint32_t indices = kernel_arg(3u);
    if (indices == 0u) recomp_stop(2, "d3d8:draw-index-pointer");
    submit_draw(kernel_arg(1u), kernel_arg(2u), vertices, stride, indices);
    recomp_runtime.registers.esp += 16u;
}

RecompFunction recomp_d3d8_draw_lookup_manual(uint32_t address)
{
    /* Whole 5849 SDK APIs; the renderer consumes semantic commands. */
    switch (address) {
    case 0x348240u: return draw_up;
    case 0x3483b0u: return draw_vertices;
    case 0x348450u: return draw_indexed;
    case 0x349490u: return pixel_create;
    case 0x3494e0u: return pixel_delete;
    case 0x349500u: return pixel_get;
    case 0x349520u: return pixel_set;
    case 0x349720u: return pixel_constant;
    default: return NULL;
    }
}
