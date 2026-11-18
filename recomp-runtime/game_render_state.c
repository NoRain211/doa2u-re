/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled renderer material, light and draw state, 0x286790-0x287000. Each
   replacement keeps the lifted function's guest RAM writes, register results
   and float rounding, so rollback peers and SyncTest see the same state.

   These four functions finish the renderer's per-object work: cull a mesh
   chunk against the camera's view cone, bind a material and its texture
   stages, then draw the chunk. The SDK state they set is kept in two guest
   blocks: 0x00355058 is the word of dirty bits the presenter watches, and
   0x00355060 is the 5849 deferred texture state, 128 bytes per stage, which
   the draw adapters read back when they translate a draw. The D3D8 entries
   themselves still go through guest dispatch, so their import adapters run. */
#include "game_decomp.h"
#include "kernel_abi.h"

#include <string.h>

/* The 5849 D3D8 and CRT entries these functions call. Each one has a host
   adapter bound to its address, so a call must not be inlined away. */
enum {
    SDK_SET_MATERIAL = 0x003443a0u,          /* stdcall (const D3DMATERIAL8 *) */
    SDK_SET_RENDER_STATE_SIMPLE = 0x00346240u, /* ECX = state, EDX = value */
    SDK_SET_CULL_MODE = 0x00346610u,         /* stdcall (D3DCULL) */
    SDK_SET_TEXTURE_FACTOR = 0x00346700u,    /* stdcall (D3DCOLOR) */
    SDK_SET_TEXTURE = 0x00345490u,           /* stdcall (stage, texture) */
    SDK_SET_TEXCOORD_INDEX = 0x00346ab0u,    /* stdcall (stage, coordinate) */
    SDK_SET_INDICES = 0x00345700u,           /* stdcall (index buffer, base) */
    SDK_SET_STREAM_SOURCE = 0x00348f20u,     /* stdcall (stream, data, offset) */
    SDK_SET_VERTEX_SHADER = 0x00349320u,     /* stdcall (handle) */
    SDK_DRAW_INDEXED = 0x00348450u,          /* stdcall (count, first, indices) */
    CRT_FTOL = 0x003354e0u,                  /* x87 ST(0) -> EDX:EAX */
};

/* The word the presenter watches (g_deferred_state_dirty); a bit marks a state
   that changed. */
enum {
    DEFERRED_DIRTY = 0x00355058u,
    DIRTY_COLOR_COMBINE = 0x0800u,
    DIRTY_TEXCOORD_INDEX = 0x0400u,
    DIRTY_FOG_ENABLE = 0x2000u,
    DIRTY_LIGHTING = 0x1200u,
    DIRTY_SPECULAR = 0x3000u,
};

/* The 5849 deferred texture state (D3D_g_DeferredTextureState), 128 bytes per
   stage. The offsets are the texture stage members the draw adapters read back
   as COLOROP, COLORARG1/2, ALPHAOP, ALPHAARG1/2, TEXCOORDINDEX and
   MIPMAPLODBIAS. */
enum {
    DEFERRED_TEXTURE_STATE = 0x00355060u,
    STAGE_STATE_STRIDE = 128u,
    STAGE_MIP_LOD_BIAS = 0x18u,
    STAGE_COLOR_OP = 0x30u,
    STAGE_COLOR_OP_TEXTURE_FACTOR = 0x34u,
    STAGE_COLOR_ARG1 = 0x38u,
    STAGE_COLOR_ARG2 = 0x3cu,
    STAGE_ALPHA_OP = 0x40u,
    STAGE_ALPHA_ARG1 = 0x48u,
    STAGE_ALPHA_ARG2 = 0x4cu,
    STAGE_TEXCOORD_INDEX = 0x54u,
    STAGE_COUNT = 4u,
};

/* Guest 0x00ea6710 (g_render_stage_cache): 8 bytes per stage holding what the
   SDK was last told. */
enum {
    STAGE_CACHE = 0x00ea6710u,
};

typedef struct StageCache {
    uint32_t texture;    /* +0x00 */
    uint8_t color_op;    /* +0x04 */
    uint8_t alpha_op;    /* +0x05 */
    uint8_t texcoord;    /* +0x06 */
    uint8_t mip_factor;  /* +0x07 */
} StageCache;

/* Guest 0x007affc8 (g_material_mode_stage_ops): the color and alpha op ids of
   a material mode. */
enum {
    MATERIAL_MODE_OPS = 0x007affc8u,
    MATERIAL_MODE_STRIDE = 0x08u,
    MATERIAL_MODE_COLOR = 0x00u,
    MATERIAL_MODE_ALPHA = 0x04u,
};
/* Guest 0x007b0078 (g_material_stage_ops): 12-byte combine records
   {op, arg1, arg2} by op id. */
enum {
    STAGE_OPS = 0x007b0078u,
    STAGE_OP_STRIDE = 0x0cu,
    STAGE_OP_KIND = 0x00u,
    STAGE_OP_ARG1 = 0x04u,
    STAGE_OP_ARG2 = 0x08u,
};
/* Guest 0x007b018c (g_material_texcoord_modes): 8-byte records
   {stage value, SDK value} by coordinate mode. */
enum {
    TEXCOORD_MODES = 0x007b018cu,
    TEXCOORD_MODE_STRIDE = 0x08u,
    TEXCOORD_MODE_STAGE = 0x00u,
    TEXCOORD_MODE_SDK = 0x04u,
};

/* A stage of the material the renderer passes in EAX: 16 bytes per stage. */
enum {
    STAGE_TEXTURE = 0x00u,
    STAGE_MODE = 0x08u,
    STAGE_MIP_FACTOR = 0x0du,
    STAGE_TEXCOORD = 0x0fu,
    STAGE_STRIDE = 0x10u,
};

/* D3DTOP and D3DTA values the stage binding reacts to. */
enum {
    D3DTOP_BLEND_FACTOR_ALPHA = 0x17u,
    D3DTOP_DISABLE = 2u,         /* the value the cache marks a reset stage with */
    D3DTOP_DISABLED_STATE = 1u,  /* D3DTOP_DISABLE as the deferred state stores it */
    D3DTA_TFACTOR = 3u,
    TEXCOORD_MODE_UNUPLOADED = 4u,
};

/* Guest 0x00ea66a8 (g_render_material_fade): the material's opacity, 1.0
   unless the object fades. */
enum {
    MATERIAL_OPACITY = 0x00ea66a8u,
    MATERIAL_OPAQUE = 0x003c1a44u,      /* 1.0f */
    MATERIAL_OPACITY_255 = 0x003c1de0u, /* 255.0f */
    MIP_BIAS_SCALE = 0x003c2dd0u,       /* -0.5f */
};

/* A material record: the D3DMATERIAL8 plus the renderer's own flags. */
enum {
    MATERIAL_FLAGS = 0x0cu,
    MATERIAL_RENDER_FLAGS = 0x68u,
    MATERIAL_START = 0x20u,             /* D3DMATERIAL8 */
    MATERIAL_SPECULAR_POWER = 0x2cu,    /* the specular alpha */
    /* The flag words the last bind left bound: g_render_cached_block_state and
       g_render_cached_block_state2. */
    MATERIAL_FLAGS_BOUND = 0x00ea670cu,
    RENDER_FLAGS_BOUND = 0x00ea6708u,
    MATERIAL_CULL_TABLE = 0x007b0278u,  /* g_material_cull_modes */
    /* Shadows of the render states those flags drive. */
    DEFERRED_FOG_ENABLE = 0x003553d0u,  /* D3DRS_FogEnable */
    DEFERRED_NORMALIZE_NORMALS = 0x00355360u,
    DEFERRED_LIGHTING = 0x003553f8u,    /* g_deferred_lighting */
    DEFERRED_SPECULAR = 0x003553fcu,    /* g_deferred_specular */
    RENDER_STATE_NORMALIZE_NORMALS = 0x4035cu,
};

/* The mesh a chunk belongs to. */
enum {
    MESH_STREAM_TABLE = 0x00ea6660u,   /* g_render_block_base: stream descriptors */
    MESH_STREAM_DATA = 0x04u,
    MESH_STREAM_INDICES = 0x0cu,
    MESH_STREAM_STRIDE = 0x10u,
    MESH_BOUND_ID = 0x00ea6664u,       /* g_render_vertex_shader: id last bound */
    MESH_SHADER_INDEX = 0x00ea6668u,   /* g_render_blend_set */
    /* 8-byte records by binding id and by shader index: the chunk's vertex
       shader/FVF and stream offset, exclusive-or and added together.
       g_chunk_shader_table and g_declaration_shader_table. */
    MESH_ID_SHADER = 0x007b01b8u,
    MESH_ID_OFFSET = 0x007b01bcu,
    MESH_SHADER_HANDLE = 0x007b0238u,
    MESH_SHADER_OFFSET = 0x007b023cu,
    INDEX_DATA = 0x00355054u,          /* g_bound_index_data */
};

/* One chunk of a mesh. */
enum {
    CHUNK_BINDING = 0x02u,
    CHUNK_INDEX_OFFSET = 0x04u,
    CHUNK_VERTEX_OFFSET = 0x08u,
};

/* The camera's clip and cone limits, set with the view matrix. */
enum {
    VIEW_NEAR_LIMIT = 0x00879c00u,     /* g_view_near_limit */
    VIEW_FAR_LIMIT = 0x008783a4u,      /* g_view_far_limit */
    VIEW_HALF_SIN_X = 0x00877c6cu,     /* g_view_half_sin_x */
    VIEW_HALF_SIN_Y = 0x00878300u,     /* g_view_half_sin_y */
    VIEW_HALF_TAN_X = 0x00879bf4u,     /* g_view_half_tan_x */
    VIEW_HALF_TAN_Y = 0x00877e9cu,     /* g_view_half_tan_y */
    VIEW_HALF_COS_X = 0x0087d228u,     /* g_view_half_cos_x */
    VIEW_HALF_COS_Y = 0x0087839cu,     /* g_view_half_cos_y */
    /* The renderer's current 4x4 (g_matrix_current) and the camera's view
       matrix, in the row-vector form 0x00284480 transforms a point with. */
    MATRIX_CURRENT = 0x00ea6080u,
    MATRIX_VIEW = 0x00879c10u,         /* g_view_matrix */
};

static float load_f32(uint32_t address)
{
    float value;

    memcpy(&value, recomp_memory(address, sizeof value), sizeof value);
    return value;
}

static void store_f32(uint32_t address, float value)
{
    memcpy(recomp_memory(address, sizeof value), &value, sizeof value);
}

/* x87 arithmetic rounds every result to the control word's precision; the
   lifted code applies the same rule (RECOMP_FP_PC in recomp_types.h). Only a
   product well inside the float range reaches this, where rounding to float
   precision is the whole of it. */
static double fp_precision(double value)
{
    return (recomp_runtime.fpu_control_word & 0x300u) != 0u
        ? value : (double)(float)value;
}

/* bind_material and bind_material_stages open with PUSH EBX, EBP, ESI, EDI
   and make every call below those four words; the words and the callees'
   return addresses stay below ESP, where later game code copies such leftover
   stack into guest RAM. Returns the entry ESP for leave_saved_registers. */
static uint32_t enter_saved_registers(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    uint32_t *const saves = (uint32_t *)(void *)recomp_memory(entry - 16u, 16u);

    saves[3] = recomp_runtime.registers.ebx;
    saves[2] = recomp_runtime.registers.ebp;
    saves[1] = recomp_runtime.registers.esi;
    saves[0] = recomp_runtime.registers.edi;
    recomp_runtime.registers.esp = entry - 16u;
    return entry;
}

static void leave_saved_registers(uint32_t entry)
{
    recomp_runtime.registers.esp = entry;
}

/* The CRT's float to 64-bit integer conversion: rounds ST(0) by the x87
   control word and leaves the result in EDX:EAX. The stage binding only keeps
   the low byte, but the rounding belongs to the guest's control word, so push
   the value and call it instead of rounding on the host. */
static uint32_t round_to_integer(double value)
{
    const uint32_t top = (recomp_runtime.fpu_top + 7u) & 7u;

    recomp_runtime.fpu_stack[top] = value;
    recomp_runtime.fpu_top = top;
    kernel_call_guest_at(0x00286e38u, CRT_FTOL, NULL, 0u);
    return recomp_runtime.registers.eax;
}

/* D3DCOLOR with the material's opacity in the alpha byte and white elsewhere,
   which is what a stage reading TEXTUREFACTOR needs re-sent. */
static uint32_t packed_texture_factor(void)
{
    const double scaled = fp_precision(
        (double)load_f32(MATERIAL_OPACITY) * (double)load_f32(MATERIAL_OPACITY_255));

    return (round_to_integer(scaled) << 24) | 0x00ffffffu;
}

static void set_texture(uint32_t return_address, uint32_t stage, uint32_t texture)
{
    const uint32_t arguments[2] = {stage, texture};

    kernel_call_guest_at(return_address, SDK_SET_TEXTURE, arguments, 2u);
}

static void set_texcoord_index(uint32_t return_address, uint32_t stage, uint32_t value)
{
    const uint32_t arguments[2] = {stage, value};

    kernel_call_guest_at(return_address, SDK_SET_TEXCOORD_INDEX, arguments, 2u);
}

/* Guest 0x00286790(ECX = bounding sphere {x, y, z, radius}): 1 when the sphere
   reaches into the camera's view cone, 0 when it is entirely outside.

   The centre is carried to view space through the renderer's current matrix
   and then the camera's, and tested in the original's order: a sphere wholly
   past the near limit or wholly past the far limit is out; when the centre is
   inside a side plane, how far the sphere reaches past that plane at its depth
   decides; and when the centre is behind both side planes, only the squared
   distance test can keep it. The radius is the untransformed fourth member, so
   the transform only has to produce the three the tests read. */
static uint32_t sphere_visible(void)
{
    float sphere[3];
    float matrix[16];
    float view[3];
    float radius;
    float sin_x;
    float sin_y;

    memcpy(sphere, recomp_memory(recomp_runtime.registers.ecx, sizeof sphere),
        sizeof sphere);
    radius = load_f32(recomp_runtime.registers.ecx + 3u * sizeof(float));

    memcpy(matrix, recomp_memory(MATRIX_CURRENT, sizeof matrix), sizeof matrix);
    for (int lane = 0; lane < 3; ++lane) {
        const float moved = matrix[8 + lane] * sphere[2] + matrix[12 + lane];

        view[lane] = (matrix[lane] * sphere[0] + matrix[4 + lane] * sphere[1]) + moved;
    }
    memcpy(matrix, recomp_memory(MATRIX_VIEW, sizeof matrix), sizeof matrix);
    for (int lane = 0; lane < 3; ++lane) {
        const float moved = matrix[8 + lane] * view[2] + matrix[12 + lane];

        sphere[lane] = (matrix[lane] * view[0] + matrix[4 + lane] * view[1]) + moved;
    }

    /* sphere[] is now the centre in view space: x, y, z. */
    if (load_f32(VIEW_NEAR_LIMIT) > radius + sphere[2]) {
        return 0u;
    }
    if (sphere[2] - radius > load_f32(VIEW_FAR_LIMIT)) {
        return 0u;
    }

    /* sin_x and sin_y are how far inside the two side planes the centre sits:
       both are the plane's slope over the radius added to the centre's depth. */
    sin_x = load_f32(VIEW_HALF_SIN_X) * radius + sphere[2];
    sin_y = load_f32(VIEW_HALF_SIN_Y) * radius + sphere[2];
    if (!(0.0f > sin_x) || !(0.0f > sin_y)) {
        /* The centre is inside a side plane, so how far the sphere reaches past
           that plane at this depth decides; the other plane's limit is negative
           and cannot reject it. */
        const float along_x = (0.0f < sphere[0] ? sphere[0] : 0.0f - sphere[0]) -
            load_f32(VIEW_HALF_COS_X) * radius;
        const float limit_x = load_f32(VIEW_HALF_TAN_X) * sin_x;
        const float along_y = (0.0f < sphere[1] ? sphere[1] : 0.0f - sphere[1]) -
            load_f32(VIEW_HALF_COS_Y) * radius;
        const float limit_y = load_f32(VIEW_HALF_TAN_Y) * sin_y;

        if (along_x > limit_x) {
            return 0u;
        }
        if (along_y > limit_y) {
            return 0u;
        }
        return 1u;
    }

    /* Behind both side planes: keep the sphere only while it still reaches the
       cone, which is the centre's squared distance against the radius. */
    {
        const float distance = (sphere[2] * sphere[2] + sphere[1] * sphere[1]) +
            sphere[0] * sphere[0];

        return !(distance > radius * radius) ? 1u : 0u;
    }
}

static void sphere_in_view_cone(void)
{
    kernel_return_caller_cleanup(sphere_visible());
}

/* Guest 0x00286d30(EAX = stage descriptors, stack = stage count): binds the
   texture stages of the material being drawn. A stage's texture, color and
   alpha combine, mip factor and texcoord index are compared with what the SDK
   was last told and only the differences are re-sent; the stages beyond the
   count are put back to the SDK's disabled stage. A color combine that reads
   the blend factor, and an alpha combine that reads TEXTUREFACTOR, also need
   the texture factor uploaded, which is why the alpha one clears its cache
   entry after binding it: the factor carries the material's opacity and can
   change between two draws of the same material. */
static void bind_material_stages(void)
{
    const uint32_t descriptor = recomp_runtime.registers.eax;
    const int32_t count = (int32_t)kernel_arg(1u);
    uint32_t *const dirty = recomp_memory_u32(DEFERRED_DIRTY);
    const uint32_t entry = enter_saved_registers();

    for (int32_t stage = 0; stage < count; ++stage) {
        const uint32_t index = (uint32_t)stage;
        const uint32_t stage_descriptor = descriptor + index * STAGE_STRIDE;
        const uint32_t state = DEFERRED_TEXTURE_STATE + index * STAGE_STATE_STRIDE;
        StageCache *const cache = (StageCache *)(void *)recomp_memory(
            STAGE_CACHE + index * sizeof(StageCache), sizeof(StageCache));
        const uint32_t texture = *recomp_memory_u32(stage_descriptor + STAGE_TEXTURE);

        if (cache->texture != texture) {
            cache->texture = texture;
            set_texture(0x00286d69u, index, texture);
        }

        const uint32_t mode = *recomp_memory_u32(stage_descriptor + STAGE_MODE);
        const uint32_t color_op = *recomp_memory_u32(
            MATERIAL_MODE_OPS + mode * MATERIAL_MODE_STRIDE + MATERIAL_MODE_COLOR);

        if ((uint32_t)cache->color_op != color_op) {
            const uint32_t record = STAGE_OPS + color_op * STAGE_OP_STRIDE;
            const uint32_t kind = *recomp_memory_u32(record + STAGE_OP_KIND);

            cache->color_op = (uint8_t)color_op;
            *recomp_memory_u32(state + STAGE_COLOR_OP) = kind;
            *recomp_memory_u32(state + STAGE_COLOR_ARG1) =
                *recomp_memory_u32(record + STAGE_OP_ARG1);
            *dirty |= DIRTY_COLOR_COMBINE;
            *recomp_memory_u32(state + STAGE_COLOR_ARG2) =
                *recomp_memory_u32(record + STAGE_OP_ARG2);
            if (kind == D3DTOP_BLEND_FACTOR_ALPHA) {
                *dirty |= DIRTY_COLOR_COMBINE;
                *recomp_memory_u32(state + STAGE_COLOR_OP_TEXTURE_FACTOR) = 1u;
            }
        }

        const uint32_t alpha_op = *recomp_memory_u32(
            MATERIAL_MODE_OPS + mode * MATERIAL_MODE_STRIDE + MATERIAL_MODE_ALPHA);

        if ((uint32_t)cache->alpha_op != alpha_op) {
            const uint32_t record = STAGE_OPS + alpha_op * STAGE_OP_STRIDE;
            const uint32_t arg1 = *recomp_memory_u32(record + STAGE_OP_ARG1);

            cache->alpha_op = (uint8_t)alpha_op;
            *recomp_memory_u32(state + STAGE_ALPHA_OP) = *recomp_memory_u32(record + STAGE_OP_KIND);
            *dirty |= DIRTY_COLOR_COMBINE;
            *recomp_memory_u32(state + STAGE_ALPHA_ARG1) = arg1;
            *recomp_memory_u32(state + STAGE_ALPHA_ARG2) =
                *recomp_memory_u32(record + STAGE_OP_ARG2);
            if (arg1 == D3DTA_TFACTOR) {
                const uint32_t arguments[1] = {packed_texture_factor()};

                kernel_call_guest_at(0x00286e46u, SDK_SET_TEXTURE_FACTOR, arguments, 1u);
                cache->alpha_op = 0xffu;
            }
        }

        const uint8_t mip_factor = *recomp_memory(
            stage_descriptor + STAGE_MIP_FACTOR, 1u);

        if (cache->mip_factor != mip_factor) {
            const float value = (float)(int32_t)mip_factor * load_f32(MIP_BIAS_SCALE);

            cache->mip_factor = mip_factor;
            *dirty |= 1u << index;
            store_f32(state + STAGE_MIP_LOD_BIAS, value);
            store_f32(entry + 4u, value); /* the original spills it over its argument */
        }

        const uint8_t texcoord = *recomp_memory(stage_descriptor + STAGE_TEXCOORD, 1u);

        if ((uint32_t)cache->texcoord != texcoord) {
            const uint32_t record = TEXCOORD_MODES + (uint32_t)texcoord * TEXCOORD_MODE_STRIDE;
            const uint32_t value = *recomp_memory_u32(record + TEXCOORD_MODE_SDK);

            cache->texcoord = texcoord;
            if ((uint32_t)texcoord != TEXCOORD_MODE_UNUPLOADED) {
                *dirty |= DIRTY_TEXCOORD_INDEX;
                *recomp_memory_u32(state + STAGE_TEXCOORD_INDEX) =
                    *recomp_memory_u32(record + TEXCOORD_MODE_STAGE);
                set_texcoord_index(0x00286ed1u, index, value);
            }
        }
    }

    for (int32_t stage = count; stage < (int32_t)STAGE_COUNT; ++stage) {
        const uint32_t index = (uint32_t)stage;
        const uint32_t state = DEFERRED_TEXTURE_STATE + index * STAGE_STATE_STRIDE;
        StageCache *const cache = (StageCache *)(void *)recomp_memory(
            STAGE_CACHE + index * sizeof(StageCache), sizeof(StageCache));

        if (cache->texture != 0u) {
            cache->texture = 0u;
            set_texture(0x00286f1eu, index, 0u);
        }
        if (cache->color_op != D3DTOP_DISABLE) {
            *dirty |= DIRTY_COLOR_COMBINE;
            cache->color_op = D3DTOP_DISABLE;
            *recomp_memory_u32(state + STAGE_COLOR_OP) = D3DTOP_DISABLED_STATE;
        }
        if (cache->alpha_op != D3DTOP_DISABLE) {
            *dirty |= DIRTY_COLOR_COMBINE;
            cache->alpha_op = D3DTOP_DISABLE;
            *recomp_memory_u32(state + STAGE_ALPHA_OP) = D3DTOP_DISABLED_STATE;
        }
        if (cache->mip_factor != 0u) {
            *dirty |= 1u << index;
            cache->mip_factor = 0u;
            store_f32(state + STAGE_MIP_LOD_BIAS, 0.0f);
            store_f32(entry + 4u, 0.0f);
        }
        if (cache->texcoord != 0u) {
            const uint32_t value =
                *recomp_memory_u32(TEXCOORD_MODES + TEXCOORD_MODE_SDK);

            *dirty |= DIRTY_TEXCOORD_INDEX;
            cache->texcoord = 0u;
            *recomp_memory_u32(state + STAGE_TEXCOORD_INDEX) =
                *recomp_memory_u32(TEXCOORD_MODES + TEXCOORD_MODE_STAGE);
            set_texcoord_index(0x00286fe0u, index, value);
        }
    }

    leave_saved_registers(entry);
    kernel_return(1u, 0u);
}

/* Guest 0x00286c00(stack = material): binds a material and the render states
   its flags select. The two flag words are compared with the ones the last
   bind left, so only a changed bit reaches the SDK: bit 2 is fog, bit 3 is
   normalize normals, bits 0-1 pick a cull mode from a table, and the second
   word's bits 0 and 1 are lighting and specular. The material itself is sent
   whole; while the object fades, its specular power is scaled for the call
   and put back afterwards so the record its owner holds never changes. */
static void bind_material(void)
{
    const uint32_t material = kernel_arg(1u);
    uint32_t *const dirty = recomp_memory_u32(DEFERRED_DIRTY);
    const uint32_t flags = *recomp_memory_u32(material + MATERIAL_FLAGS);
    const uint32_t changed = *recomp_memory_u32(MATERIAL_FLAGS_BOUND) ^ flags;
    const uint32_t entry = enter_saved_registers();

    if (changed != 0u) {
        if ((changed & 0x80000004u) != 0u) {
            *dirty |= DIRTY_FOG_ENABLE;
            *recomp_memory_u32(DEFERRED_FOG_ENABLE) = (~(flags >> 2u)) & 1u;
        }
        if ((changed & 0x80000008u) != 0u) {
            const uint32_t value = (~(flags >> 3u)) & 1u;

            recomp_runtime.registers.ecx = RENDER_STATE_NORMALIZE_NORMALS;
            recomp_runtime.registers.edx = value;
            kernel_call_guest_at(0x00286c54u, SDK_SET_RENDER_STATE_SIMPLE, NULL, 0u);
            *recomp_memory_u32(DEFERRED_NORMALIZE_NORMALS) = value;
        }
        if ((changed & 0x80000003u) != 0u) {
            const uint32_t arguments[1] = {
                *recomp_memory_u32(MATERIAL_CULL_TABLE + (flags & 3u) * 4u)};

            kernel_call_guest_at(0x00286c74u, SDK_SET_CULL_MODE, arguments, 1u);
        }
        *recomp_memory_u32(MATERIAL_FLAGS_BOUND) = flags;
    }

    const uint32_t render = *recomp_memory_u32(material + MATERIAL_RENDER_FLAGS);
    const uint32_t render_changed = *recomp_memory_u32(RENDER_FLAGS_BOUND) ^ render;

    if (render_changed != 0u) {
        if ((render_changed & 0x80000002u) != 0u) {
            *dirty |= DIRTY_SPECULAR;
            *recomp_memory_u32(DEFERRED_SPECULAR) = (render >> 1u) & 1u;
        }
        if ((render_changed & 0x80000001u) != 0u) {
            *dirty |= DIRTY_LIGHTING;
            *recomp_memory_u32(DEFERRED_LIGHTING) = render & 1u;
        }
        *recomp_memory_u32(RENDER_FLAGS_BOUND) = render;
    }

    {
        const uint32_t arguments[1] = {material + MATERIAL_START};
        const float opacity = load_f32(MATERIAL_OPACITY);

        if (opacity == load_f32(MATERIAL_OPAQUE)) {
            kernel_call_guest_at(0x00286d25u, SDK_SET_MATERIAL, arguments, 1u);
        } else {
            const float saved = load_f32(material + MATERIAL_SPECULAR_POWER);

            store_f32(entry + 4u, saved); /* the original keeps it over its argument */
            store_f32(material + MATERIAL_SPECULAR_POWER, saved * opacity);
            kernel_call_guest_at(0x00286d0au, SDK_SET_MATERIAL, arguments, 1u);
            store_f32(material + MATERIAL_SPECULAR_POWER, saved);
        }
    }

    leave_saved_registers(entry);
    kernel_return(1u, 0u);
}

/* Guest 0x00287000(stack = chunk): draws one chunk of the current mesh. The
   chunk's 16-bit binding id selects a vertex shader, stream and index buffer
   that every chunk with the same id shares, so those are only bound when the
   id changes; the draw itself is the chunk's two triangles at its own index
   and vertex offsets in the mesh's index buffer.

   The original pushes its caller's EBP and EDI, and EBX and ESI only to
   rebind, in the words below its return address, and the calls' arguments
   below those, so the deepest word its last call leaves is the chunk's
   primitive count. The renderer reads that scratch back after the draw (it
   ends up as the unused alpha of a light record at 0x00ea6608), so the
   replacement writes the same words and makes each call at the same depth. */
static void draw_mesh_chunk(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    uint32_t *const saves = (uint32_t *)(void *)recomp_memory(entry - 16u, 16u);
    const uint32_t chunk = kernel_arg(1u);
    const uint32_t binding = *recomp_memory_u16(chunk + CHUNK_BINDING);

    saves[3] = recomp_runtime.registers.ebp;
    saves[2] = recomp_runtime.registers.edi;

    if (binding != *recomp_memory_u32(MESH_BOUND_ID)) {
        const uint32_t stream = *recomp_memory_u32(MESH_STREAM_TABLE) +
            binding * MESH_STREAM_STRIDE;
        const uint32_t shader = *recomp_memory_u32(MESH_SHADER_INDEX);
        uint32_t arguments[3];

        /* The rebinding calls run below the four saved registers. */
        saves[1] = recomp_runtime.registers.ebx;
        saves[0] = recomp_runtime.registers.esi;
        recomp_runtime.registers.esp = entry - 16u;

        arguments[0] = *recomp_memory_u32(MESH_ID_SHADER + binding * 8u) ^
            *recomp_memory_u32(MESH_SHADER_HANDLE + shader * 8u);
        kernel_call_guest_at(0x00287045u, SDK_SET_VERTEX_SHADER, arguments, 1u);

        arguments[0] = 0u;
        arguments[1] = *recomp_memory_u32(stream + MESH_STREAM_DATA);
        arguments[2] = *recomp_memory_u32(MESH_ID_OFFSET + binding * 8u) +
            *recomp_memory_u32(MESH_SHADER_OFFSET + shader * 8u);
        kernel_call_guest_at(0x0028705du, SDK_SET_STREAM_SOURCE, arguments, 3u);

        arguments[0] = *recomp_memory_u32(stream + MESH_STREAM_INDICES);
        arguments[1] = 0u;
        kernel_call_guest_at(0x00287068u, SDK_SET_INDICES, arguments, 2u);
        *recomp_memory_u32(MESH_BOUND_ID) = binding;
    }

    {
        const uint32_t arguments[3] = {
            6u,
            *recomp_memory_u32(chunk + CHUNK_VERTEX_OFFSET) + 2u,
            *recomp_memory_u32(INDEX_DATA) +
                *recomp_memory_u32(chunk + CHUNK_INDEX_OFFSET) * 2u,
        };

        /* The original has dropped the rebinding registers by now, so the draw
           leaves its arguments two words higher. */
        recomp_runtime.registers.esp = entry - 8u;
        kernel_call_guest_at(0x0028708bu, SDK_DRAW_INDEXED, arguments, 3u);
    }

    recomp_runtime.registers.esp = entry;
    kernel_return(1u, 0u);
}

RecompFunction recomp_render_state_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x00286790u: return sphere_in_view_cone;
    case 0x00286c00u: return bind_material;
    case 0x00286d30u: return bind_material_stages;
    case 0x00287000u: return draw_mesh_chunk;
    default: return NULL;
    }
}
