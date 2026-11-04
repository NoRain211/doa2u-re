/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_state_adapter.h"
#include "d3d8_adapter.h"
#include "d3d_light_model.h"
#include "d3d_vertex_shader_model.h"
#include "kernel_abi.h"
#include "stop_report.h"

#include <math.h>
#include <string.h>

enum {
    D3D_DIRTY = 0x00355058u,
    D3D_RENDER_STATE = 0x00355260u,
    D3D_STATE_DEFAULTS = 0x003536e0u,
    D3D_SIMPLE_ENCODING = 0x003c18d0u,
    D3D_DEFERRED_DIRTY = 0x003c16b0u,
    D3D_FOG_COLOR = 0x00355488u,
    G_FILL_MODE = 0x0035548cu,
    G_NORMALIZE = 0x00355498u,
    G_ZENABLE = 0x0035549cu,
    G_STENCIL = 0x003554a0u,
    G_CULL = 0x003554acu,
    G_EDGE_AA = 0x003554bcu,
    G_MSAA = 0x003554c0u,
    DEV_DECLARATION = 0x794u,
    DEV_SHADER_HANDLE = 0x798u,
    DEV_PROGRAM_SLOT = 0x79cu,
    DEV_MATERIAL = 0xf00u,
    DEV_TEXTURE_STAGE = 0xf88u,
    D3D_OUT_OF_MEMORY = 0x8007000eu,
};

static RecompD3dRenderStateModel render_state;

const RecompD3dRenderStateModel *recomp_d3d8_state_model(void)
{
    return &render_state;
}

void recomp_d3d8_state_reset(void)
{
    recomp_d3d_render_state_reset(&render_state);
}

bool recomp_d3d8_state_set_simple(uint32_t method, uint32_t value)
{
    return recomp_d3d_set_simple_render_state(&render_state, method, value);
}

/* CDevice::Init runs SetStateDefaults (0x34bc80): states 0x39..0xa5 from the
   SDK table at 0x3536e0, except 0x7e/0x7f/0x9a, go through
   SetRenderStateNotInline (0x346270), and ZENABLE (0x8f) follows the auto
   depth surface. Games save and restore states by reading this shadow back. */
void recomp_d3d8_state_write_defaults(bool auto_depth)
{
    for (uint32_t state = 0x39u; state < 0xa6u; ++state) {
        if (state == 0x7eu || state == 0x7fu || state == 0x9au) continue;
        const uint32_t value = state == 0x8fu ? (uint32_t)auto_depth :
            *recomp_memory_u32(D3D_STATE_DEFAULTS + (state - 0x39u) * 4u);
        if (state < 0x5cu) {
            const uint32_t method = *recomp_memory_u32(D3D_SIMPLE_ENCODING + state * 4u);
            if (!recomp_d3d_set_simple_render_state(&render_state, method, value))
                recomp_stop(2, "d3d8:default-simple-state:%02x:%08x", state, method);
        } else if (state < 0x88u) {
            *recomp_memory_u32(D3D_DIRTY) |= *recomp_memory_u32(D3D_DEFERRED_DIRTY + state * 4u);
        }
        /* shortcut: complex defaults (0x88+) reach only the shadow, not their
           setters' model state; replay the setters when a draw needs one. */
        *recomp_memory_u32(D3D_RENDER_STATE + state * 4u) = value;
    }
}

static void set_gamma(void)
{
    uint32_t device = recomp_d3d8_require_device();
    uint32_t flags = kernel_arg(1u), ramp = kernel_arg(2u);
    uint32_t bank = *recomp_memory_u32(device + 0x240cu);
    RecompD3dPresenterCommand command = {0};
    if (bank > 1u) recomp_stop(2, "d3d8:invalid-gamma-bank");
    command.type = RECOMP_D3D_PRESENTER_COMMAND_GAMMA;
    recomp_guest_load(command.data.gamma, ramp, sizeof command.data.gamma);
    if (memcmp(command.data.gamma, recomp_memory(device + 0x1e04u + bank * 0x300u,
            sizeof command.data.gamma),
            sizeof command.data.gamma) != 0) {
        bank = *recomp_memory_u32(device + 0x2478u) & 1u;
        recomp_guest_store(device + 0x1e04u + bank * 0x300u,
            command.data.gamma, sizeof command.data.gamma);
        *recomp_memory_u32(device + 0x240cu) = bank;
        if ((flags & 2u) == 0u) *recomp_memory_u32(device + 0x2404u + bank * 4u) = 1u;
        recomp_d3d8_submit(&command);
    }
    recomp_runtime.registers.esp += 12u;
}

static void set_transform(void)
{
    static const uint32_t dirty[10] = {
        0xff0200u, 0x200u, 0x400u, 0x400u, 0x400u,
        0x400u, 0x200u, 0x200u, 0x200u, 0x200u,
    };
    uint32_t device = recomp_d3d8_require_device();
    uint32_t state = kernel_arg(1u), matrix = kernel_arg(2u);
    float m[16];
    if (state >= 10u) recomp_stop(2, "d3d8:unsupported-transform:%u", state);
    recomp_guest_load(m, matrix, sizeof m);
    recomp_guest_store(device + 0xc60u + state * 0x40u, m, sizeof m);
    if (state == 1u) {
        float near_z = 0.0f, far_z = 1.0f, inverse_far = 1.0f;
        uint32_t m44;
        if (m[10] != 0.0f && m[10] != m[11]) {
            near_z = (float)(m[15] - ((double)m[14] / m[10]) * m[11]);
            far_z = (float)(m[15] + (((double)m[15] - m[14]) /
                ((double)m[10] - m[11])) * m[11]);
            inverse_far = 1.0f / far_z;
        }
        recomp_guest_store(device + 0x93cu, &near_z, sizeof near_z);
        recomp_guest_store(device + 0x940u, &far_z, sizeof far_z);
        recomp_guest_store(device + 0x944u, &inverse_far, sizeof inverse_far);
        memcpy(&m44, &m[15], sizeof m44);
        uint32_t *mode = recomp_memory_u32(device + 8u);
        *mode = (*mode & ~2u) | (m[3] == 0.0f && m[7] == 0.0f &&
            m[11] == 0.0f && m44 == 0x3f800000u ? 2u : 0u);
    }
    *recomp_memory_u32(D3D_DIRTY) |= dirty[state];
    recomp_runtime.registers.esp += 12u;
}

static void set_fog_color(void)
{
    recomp_d3d8_require_device();
    *recomp_memory_u32(D3D_FOG_COLOR) = kernel_arg(1u);
    recomp_runtime.registers.esp += 8u;
}

/* 5849 ShadowFunc (0x346580) writes state 0x9c and pushes value - 0x200.
   Depth-texture sampling is guarded by attach_texture, so no host draw uses it. */
static void shadow_func(void)
{
    recomp_d3d8_require_device();
    const uint32_t value = kernel_arg(1u);
    /* shortcut: only the observed default is bound, extend with depth-texture sampling. */
    if (value != 0x200u) recomp_stop(2, "d3d8:unsupported-shadow-func:%08x", value);
    *recomp_memory_u32(0x3554d0u) = value;
    recomp_runtime.registers.esp += 8u;
}

static void set_screen_space_offset(void)
{
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t bits[2] = {kernel_arg(1u), kernel_arg(2u)};
    float offsets[2];
    memcpy(offsets, bits, sizeof offsets);
    for (uint32_t i = 0u; i < 2u; ++i) {
        if (!isfinite(offsets[i])) recomp_stop(2, "d3d8:screen-space-offset");
        offsets[i] += 0.53125f; /* SDK raster bias; the draw snapshot exports only the API offset. */
    }
    recomp_guest_store(device + 0xef8u, offsets, sizeof offsets);
    recomp_runtime.registers.esp += 12u;
}

static void set_scissors(void)
{
    const uint32_t device = recomp_d3d8_require_device();
    const uint32_t count = kernel_arg(1u), exclusive = kernel_arg(2u), source = kernel_arg(3u);
    /* shortcut: multiple or exclusive regions need a union/complement clip path. */
    if (count > 1u || (count != 0u && exclusive != 0u))
        recomp_stop(2, "d3d8:unsupported-scissors:%u:%u", count, exclusive);
    if (count != 0u) {
        int32_t rect[4];
        if (source == 0u) recomp_stop(2, "d3d8:scissor-pointer");
        recomp_guest_load(rect, source, sizeof rect);
        if (rect[0] < 0 || rect[1] < 0 || rect[2] < rect[0] || rect[3] < rect[1] ||
            rect[2] > 32767 || rect[3] > 32767)
            recomp_stop(2, "d3d8:scissor-rectangle");
        recomp_guest_store(device + 0x1b80u, rect, sizeof rect);
    }
    *recomp_memory_u32(device + 0x1c00u) = count;
    *recomp_memory_u32(device + 0x1c04u) = exclusive;
    recomp_runtime.registers.esp += 16u;
}

static uint32_t light_record(uint32_t device, uint32_t index)
{
    uint32_t array = *recomp_memory_u32(device + 0x7a4u);
    uint32_t capacity = *recomp_memory_u32(device + 0x7a8u);
    if (capacity > RECOMP_XBOX_RAM_SIZE / sizeof(RecompD3dLightRecord) ||
        (capacity != 0u && array == 0u)) {
        recomp_stop(2, "d3d8:invalid-light-array");
    }
    if (capacity != 0u) recomp_memory(array, capacity * sizeof(RecompD3dLightRecord));
    if (index >= capacity) {
        uint64_t grown = ((uint64_t)index + 16u) & ~UINT64_C(15);
        uint64_t bytes = grown * sizeof(RecompD3dLightRecord);
        if (bytes > RECOMP_XBOX_RAM_SIZE) return 0u;
        uint32_t replacement = recomp_kernel_allocate_pool((uint32_t)bytes);
        if (replacement == 0u) return 0u;
        if (capacity != 0u) {
            recomp_guest_memcpy(replacement, array, capacity * sizeof(RecompD3dLightRecord));
        }
        for (uint32_t i = capacity; i < grown; ++i) {
            RecompD3dLightRecord empty = {0};
            empty.slot = UINT32_MAX;
            recomp_guest_store(replacement + i * sizeof empty, &empty, sizeof empty);
        }
        for (uint32_t slot = 0u; slot < 8u; ++slot) {
            uint32_t *active = recomp_memory_u32(device + 0x7acu + slot * 4u);
            if (*active != 0u) {
                uint32_t offset = *active - array;
                if (offset >= capacity * sizeof(RecompD3dLightRecord) ||
                    offset % sizeof(RecompD3dLightRecord) != 0u) {
                    recomp_stop(2, "d3d8:invalid-light-slot");
                }
                *active = replacement + offset;
            }
        }
        *recomp_memory_u32(device + 0x7a4u) = replacement;
        *recomp_memory_u32(device + 0x7a8u) = (uint32_t)grown;
        if (array != 0u) recomp_kernel_free_pool(array);
        array = replacement;
    }
    return array + index * sizeof(RecompD3dLightRecord);
}

static void mark_light(uint32_t device, uint32_t address,
    const RecompD3dLightRecord *record)
{
    if (record->slot == UINT32_MAX) return;
    uint32_t slot = record->slot;
    if (slot >= 8u || *recomp_memory_u32(device + 0x7acu + slot * 4u) != address) {
        recomp_stop(2, "d3d8:invalid-light-slot");
    }
    uint32_t *types = recomp_memory_u32(device + 0x7ccu);
    *types = (*types & ~(3u << (slot * 2u))) |
        (recomp_d3d_light_slot_type(record->desc.type) << (slot * 2u));
    *recomp_memory_u32(D3D_DIRTY) |= 0x10000u << slot;
}

static uint32_t store_light(uint32_t device, uint32_t index,
    const RecompD3dLightDesc *desc)
{
    /* shortcut: spot coefficients need this SDK's approximation tables; stop until observed. */
    if (desc->type == 2u) recomp_stop(2, "d3d8:unsupported-spot-light");
    if (recomp_d3d_light_slot_type(desc->type) == 0u) {
        recomp_stop(2, "d3d8:unsupported-light-type:%u", desc->type);
    }
    uint32_t address = light_record(device, index);
    if (address == 0u) return D3D_OUT_OF_MEMORY;
    RecompD3dLightRecord record;
    recomp_guest_load(&record, address, sizeof record);
    if (!recomp_d3d_light_set(&record, desc)) recomp_stop(2, "d3d8:invalid-light-direction");
    recomp_guest_store(address, &record, sizeof record);
    mark_light(device, address, &record);
    return 0u;
}

static void set_light(void)
{
    uint32_t device = recomp_d3d8_require_device();
    RecompD3dLightDesc desc;
    recomp_guest_load(&desc, kernel_arg(2u), sizeof desc);
    kernel_return(2u, store_light(device, kernel_arg(1u), &desc));
}

static void light_enable(void)
{
    uint32_t device = recomp_d3d8_require_device();
    uint32_t index = kernel_arg(1u), enable = kernel_arg(2u);
    uint32_t address = light_record(device, index);
    if (address == 0u) { kernel_return(2u, D3D_OUT_OF_MEMORY); return; }
    RecompD3dLightRecord record;
    recomp_guest_load(&record, address, sizeof record);
    if ((record.flags & 1u) == 0u) {
        RecompD3dLightDesc desc;
        recomp_d3d_light_default(&desc);
        uint32_t result = store_light(device, index, &desc);
        if (result != 0u) { kernel_return(2u, result); return; }
        recomp_guest_load(&record, address, sizeof record);
    }
    if (recomp_d3d_light_slot_type(record.desc.type) == 0u) {
        recomp_stop(2, "d3d8:unsupported-light-type:%u", record.desc.type);
    }
    record.flags |= 4u;
    if (enable != 0u && record.slot == UINT32_MAX) {
        uint32_t types = *recomp_memory_u32(device + 0x7ccu), slot;
        for (slot = 0u; slot < 8u && ((types >> (slot * 2u)) & 3u) != 0u; ++slot) {}
        if (slot == 8u) recomp_stop(2, "d3d8:unsupported-active-light-count");
        record.slot = slot;
        *recomp_memory_u32(device + 0x7acu + slot * 4u) = address;
        mark_light(device, address, &record);
    } else if (enable == 0u && record.slot != UINT32_MAX) {
        uint32_t slot = record.slot;
        mark_light(device, address, &record);
        *recomp_memory_u32(device + 0x7acu + slot * 4u) = 0u;
        *recomp_memory_u32(device + 0x7ccu) &= ~(3u << (slot * 2u));
        record.slot = UINT32_MAX;
    }
    recomp_guest_store(address, &record, sizeof record);
    kernel_return(2u, 0u);
}

/* 5849 SetVertexShader on a program handle runs LoadVertexShader(handle, 0)
   (0x348fb0) then SelectVertexShader(handle, 0) (0x349010). The object at
   handle - 1 comes from CreateVertexShader (0x348c80): flags +4, instruction
   count +8, upload word count +0xc, upload blocks +0x114. */
static void set_vertex_program(uint32_t device, uint32_t handle, uint32_t declaration)
{
    const uint32_t object = handle - 1u;
    const uint32_t flags = *recomp_memory_u32(object + 4u);
    const uint32_t count = *recomp_memory_u32(object + 8u);
    const uint32_t words = *recomp_memory_u32(object + 0xcu);
    uint32_t program[136][4];
    /* shortcut: declaration-only handles and declaration constant uploads stay
       guarded; bind them when a run reaches one. */
    if ((flags & 0x10u) == 0u || words > 680u ||
        !recomp_d3d_unpack_vertex_program(
            (const uint32_t *)(const void *)recomp_memory(object + 0x114u, words * 4u),
            words, count, program))
        recomp_stop(2, "d3d8:vertex-program-unsupported:%08x", handle);
    /* Load copies code into the device program shadow unless the device
       defers it (+8 bit 0x10); constants come from SetVertexShaderConstant. */
    if ((*recomp_memory_u32(device + 8u) & 0x10u) == 0u)
        recomp_guest_store(device + 0x10a8u, program, count * sizeof program[0]);
    const uint32_t old_flags = *recomp_memory_u32(declaration + 4u);
    *recomp_memory_u32(D3D_DIRTY) |= ((flags ^ old_flags) & 0x13u) != 0u ? 0x1070u : 0x70u;
    *recomp_memory_u32(device + DEV_DECLARATION) = object;
    *recomp_memory_u32(device + DEV_SHADER_HANDLE) = handle;
    *recomp_memory_u32(device + DEV_PROGRAM_SLOT) = 0u;
}

static void set_vertex_shader(void)
{
    uint32_t device = recomp_d3d8_require_device();
    uint32_t handle = kernel_arg(1u);
    uint32_t declaration = *recomp_memory_u32(device + DEV_DECLARATION);
    if (declaration == 0u) recomp_stop(2, "d3d8:missing-declaration");
    if ((handle & 1u) != 0u) {
        set_vertex_program(device, handle, declaration);
        kernel_return(1u, 0u);
        return;
    }
    const uint32_t old_flags = *recomp_memory_u32(declaration + 4u);
    const uint32_t old_mask = *recomp_memory_u32(declaration + 0x10u);
    uint32_t *image = (uint32_t *)(void *)recomp_memory(
        RECOMP_D3D_DEFAULT_DECLARATION, RECOMP_D3D_VERTEX_DECLARATION_SIZE);
    if (!recomp_d3d_build_fixed_function_declaration(handle, image)) {
        recomp_stop(2, "d3d8:invalid-fvf:%08x", handle);
    }
    uint32_t dirty = *recomp_memory_u32(D3D_DIRTY) | 0x70u;
    if (old_mask != *recomp_memory_u32(RECOMP_D3D_DEFAULT_DECLARATION + 0x10u)) dirty |= 0x400u;
    if (old_flags != *recomp_memory_u32(RECOMP_D3D_DEFAULT_DECLARATION + 4u)) dirty |= 0x1600u;
    *recomp_memory_u32(D3D_DIRTY) = dirty;
    *recomp_memory_u32(device + DEV_DECLARATION) = RECOMP_D3D_DEFAULT_DECLARATION;
    *recomp_memory_u32(device + DEV_SHADER_HANDLE) = handle;
    *recomp_memory_u32(device + DEV_PROGRAM_SLOT) = 0u;
    kernel_return(1u, 0u);
}

static void set_render_state_simple(void)
{
    recomp_d3d8_require_device();
    if (!recomp_d3d_set_simple_render_state(&render_state,
            recomp_runtime.registers.ecx, recomp_runtime.registers.edx)) {
        recomp_stop(2, "d3d8:invalid-simple-state:%08x", recomp_runtime.registers.ecx);
    }
    recomp_runtime.registers.esp += 4u;
}

/* 5849 SetVertexShaderConstant1/4 (0x348de0/0x348e40) take the register in
   ECX and the source in EDX. Keep only the 192-entry shadow at 0x354440,
   which program draws read; the console push is GPU-only. */
static void vertex_constants(uint32_t count)
{
    const uint32_t index = recomp_runtime.registers.ecx;
    if (index > 192u - count) recomp_stop(2, "d3d8:vertex-constant-range:%u", index);
    recomp_guest_memcpy(0x354440u + index * 16u, recomp_runtime.registers.edx, count * 16u);
    recomp_runtime.registers.esp += 4u;
}

static void vertex_constant1(void) { vertex_constants(1u); }
static void vertex_constant4(void) { vertex_constants(4u); }

static void get_transform(void)
{
    uint32_t device = recomp_d3d8_require_device();
    uint32_t state = kernel_arg(1u), output = kernel_arg(2u);
    if (state >= 10u || output == 0u) recomp_stop(2, "d3d8:get-transform-arguments");
    recomp_guest_memcpy(output, device + 0xc60u + state * 64u, 64u);
    recomp_runtime.registers.esp += 12u;
}

static void set_state_global(uint32_t address,
    bool (*setter)(RecompD3dRenderStateModel *, uint32_t))
{
    recomp_d3d8_require_device();
    const uint32_t value = kernel_arg(1u);
    if (!setter(&render_state, value)) {
        recomp_stop(2, "d3d8:unsupported-state-value:%08x:%08x", address, value);
    }
    *recomp_memory_u32(address) = value;
    kernel_return(1u, 0u);
}

static void multi_sample_anti_alias(void)
{
    /* shortcut: this device has one sample; enabling MSAA needs target recreation. */
    if (kernel_arg(1u) != 0u) recomp_stop(2, "d3d8:unsupported-state-value:multisample");
    set_state_global(G_MSAA, recomp_d3d_set_multisample_antialias);
}
static void z_enable(void) { set_state_global(G_ZENABLE, recomp_d3d_set_z_enable); }

/* MultiSampleMode/RenderTargetMode (0x3477a0/0x3477d0) pick 1X/2X/4X buffer
   samples per shaded pixel (0x347130). The pass still covers every sample, as
   the host's full-rate rendering does, so only the guest shadow is kept. */
static void multi_sample_mode(uint32_t address)
{
    recomp_d3d8_require_device();
    const uint32_t value = kernel_arg(1u);
    if (value > 2u) recomp_stop(2, "d3d8:unsupported-state-value:%08x:%08x", address, value);
    *recomp_memory_u32(address) = value;
    kernel_return(1u, 0u);
}
static void back_buffer_sample_mode(void) { multi_sample_mode(0x3554c8u); }
static void target_sample_mode(void) { multi_sample_mode(0x3554ccu); }
static void stencil_enable(void) { set_state_global(G_STENCIL, recomp_d3d_set_stencil_enable); }
static void edge_anti_alias(void) { set_state_global(G_EDGE_AA, recomp_d3d_set_edge_antialias); }
static void cull_mode(void) { set_state_global(G_CULL, recomp_d3d_set_cull_mode); }

static void fill_mode(void)
{
    recomp_d3d8_require_device();
    const uint32_t value = kernel_arg(1u);
    /* D3DFILL_POINT/SOLID/WIREFRAME; the SDK forwards SOLID when a
       fill-mode override global is active. Only the shadow is semantic. */
    if (!recomp_d3d_set_fill_mode(&render_state, value)) {
        recomp_stop(2, "d3d8:unsupported-fill-mode:%08x", value);
    }
    *recomp_memory_u32(G_FILL_MODE) = value;
    kernel_return(1u, 0u);
}

static void normalize_normals(void)
{
    recomp_d3d8_require_device();
    const uint32_t value = kernel_arg(1u);
    if (!recomp_d3d_set_normalize_normals(&render_state, value))
        recomp_stop(2, "d3d8:unsupported-normalize:%08x", value);
    *recomp_memory_u32(G_NORMALIZE) = value;
    *recomp_memory_u32(D3D_DIRTY) |= 0x200u;
    kernel_return(1u, 0u);
}

static void set_material(void)
{
    uint32_t device = recomp_d3d8_require_device();
    const uint32_t source = kernel_arg(1u);
    /* D3DMATERIAL8 is 17 DWORDs; recomp_guest_memcpy validates the span. */
    if (source == 0u) recomp_stop(2, "d3d8:material-pointer");
    recomp_guest_memcpy(device + DEV_MATERIAL, source, 68u);
    *recomp_memory_u32(D3D_DIRTY) |= 0x9000u;
    kernel_return(1u, 0u);
}

static void texture_factor(void)
{
    set_state_global(0x3554b0u, recomp_d3d_set_texture_factor);
}

static void line_width(void)
{
    recomp_d3d8_require_device();
    const uint32_t value = kernel_arg(1u);
    float width;
    memcpy(&width, &value, sizeof width);
    /* The draw adapter widens pretransformed lines; the SDK clamps the width. */
    if (!isfinite(width) || width < 0.0f) recomp_stop(2, "d3d8:unsupported-line-width:%08x", value);
    *recomp_memory_u32(0x3554d4u) = value;
    kernel_return(1u, 0u);
}

static void stencil_fail(void)
{
    recomp_d3d8_require_device();
    uint32_t value = kernel_arg(1u);
    RecompD3dStencilOp op;
    if (!recomp_d3d_stencil_op_from_nv(value, &op))
        recomp_stop(2, "d3d8:invalid-stencil-fail:%08x", value);
    recomp_d3d_set_simple_render_state(&render_state, 0x40370u, value);
    *recomp_memory_u32(0x3554a4u) = value;
    kernel_return(1u, 0u);
}

static void vertex_blend(void)
{
    recomp_d3d8_require_device();
    uint32_t value = kernel_arg(1u);
    /* Xbox implicit-remainder modes encode one, two, three weights as 1, 3, 5. */
    if (value > 5u || (value != 0u && (value & 1u) == 0u))
        recomp_stop(2, "d3d8:unsupported-vertex-blend:%08x", value);
    *recomp_memory_u32(0x355484u) = value;
    *recomp_memory_u32(D3D_DIRTY) |= 0x200u;
    kernel_return(1u, 0u);
}

static void texcoord_index(void)
{
    uint32_t device = recomp_d3d8_require_device();
    uint32_t stage = kernel_arg(1u), value = kernel_arg(2u);
    if (stage >= 4u || (value & 0xffffu) >= 8u || (value >> 16u) > 3u)
        recomp_stop(2, "d3d8:unsupported-texcoord:%u:%08x", stage, value);
    *recomp_memory_u32(0x3550d0u + stage * 128u) = value;
    *recomp_memory(0x7c0a79u + stage, 1u) = (uint8_t)(9u + value);
    const uint32_t previous = *recomp_memory_u32(device + 0x950u);
    uint32_t generated = (previous & ~(1u << stage)) |
        ((value >> 16u == 1u || value >> 16u == 3u) ? 1u << stage : 0u);
    *recomp_memory_u32(device + 0x950u) = generated;
    *recomp_memory_u32(D3D_DIRTY) |= 0x47fu | ((previous == 0u && generated != 0u) ? 0x200u : 0u);
    kernel_return(2u, 0u);
}

/* SetTextureState_BumpEnv (0x346bc0, stdcall stage/state/value): BUMPENVMAT00
   through BUMPENVLOFFSET (22..27) land in the deferred stage array 0x355060;
   the NV2A method it also pushes has no host consumer. */
static void bump_env(void)
{
    (void)recomp_d3d8_require_device();
    uint32_t stage = kernel_arg(1u), state = kernel_arg(2u);
    if (stage >= 4u || state < 22u || state > 27u)
        recomp_stop(2, "d3d8:unsupported-bump-env:%u:%u", stage, state);
    *recomp_memory_u32(0x355060u + (stage * 32u + state) * 4u) = kernel_arg(3u);
    kernel_return(3u, 0u);
}

/* SetTextureState_BorderColor (0x346c30, stdcall stage/color) shadows
   D3DTSS_BORDERCOLOR (29) for BORDER addressing; its NV2A method 0x1b24 has no
   host consumer. */
static void border_color(void)
{
    (void)recomp_d3d8_require_device();
    const uint32_t stage = kernel_arg(1u);
    if (stage >= 4u) recomp_stop(2, "d3d8:texture-stage:%u", stage);
    *recomp_memory_u32(0x355060u + (stage * 32u + 29u) * 4u) = kernel_arg(2u);
    kernel_return(2u, 0u);
}

RecompFunction recomp_d3d8_state_lookup_manual(uint32_t address)
{
    /* Verified 5849 API entries; unsupported state values stop explicitly. */
    switch (address) {
    case 0x00344180u: return set_gamma;
    case 0x00344210u: return set_transform;
    case 0x00344330u: return get_transform;
    case 0x003443a0u: return set_material;
    case 0x003465c0u: return set_fog_color;
    case 0x00345040u: return set_light;
    case 0x00345300u: return light_enable;
    case 0x003459f0u: return set_scissors;
    case 0x00345ea0u: return set_screen_space_offset;
    case 0x00346240u: return set_render_state_simple;
    case 0x00346540u: return edge_anti_alias;
    case 0x00346580u: return shadow_func;
    case 0x00346610u: return cull_mode;
    case 0x003466c0u: return normalize_normals;
    case 0x00346700u: return texture_factor;
    case 0x00346760u: return line_width;
    case 0x00346a60u: return vertex_blend;
    case 0x00346ab0u: return texcoord_index;
    case 0x00346bc0u: return bump_env;
    case 0x00346c30u: return border_color;
    case 0x003475c0u: return stencil_fail;
    case 0x00346930u: return fill_mode;
    case 0x00347490u: return z_enable;
    case 0x00347520u: return stencil_enable;
    case 0x00347800u: return multi_sample_anti_alias;
    case 0x003477a0u: return back_buffer_sample_mode;
    case 0x003477d0u: return target_sample_mode;
    case 0x00348de0u: return vertex_constant1;
    case 0x00348e40u: return vertex_constant4;
    case 0x00349320u: return set_vertex_shader;
    default: return NULL;
    }
}
