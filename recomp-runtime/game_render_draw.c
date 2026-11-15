/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled renderer draw submission, 0x288a00-0x288e40. Each
   replacement keeps the lifted function's guest RAM writes, register results
   and float rounding, so rollback peers and SyncTest see the same state. */
#include "game_decomp.h"
#include "kernel_abi.h"

#include <string.h>

/* program_adapters.c: manual group hook, then generated body, then kernel
   import, with its own per-address cache. */
RecompFunction recomp_program_lookup(uint32_t guest_address);
/* Bumped when a reboot re-resolves the manual hooks (program_adapters.c). */
extern uint32_t recomp_program_lookup_generation;

/* ---- Guest render state ------------------------------------------------
   The renderer keeps one draw state at 0xea6694 and three depth-sorted queues
   of draw blocks, each a {head, tail} pair. 0x288a00 below flushes a frame's
   queues; 0x288aa0 and 0x288e40 queue or draw one model's block chain. */
enum {
    /* Block pool of the model being drawn: model + 0x20 + group * 0x20. */
    RENDER_BLOCK_BASE = 0x00ea6660u,
    /* Vertex shader id bound for the last draw record; -1 after a reset. */
    RENDER_VERTEX_SHADER = 0x00ea6664u,
    /* Vertex-blend matrix set, an index into the transform/blend-state tables. */
    RENDER_BLEND_SET = 0x00ea6668u,
    /* Block-class table of the current pass: 8 classes per block type. */
    RENDER_CLASS_TABLE = 0x00ea666cu,
    /* Queue heads; the tail pointer of each pair sits at head + 4. The first
       two hold blocks queued by class, the third the ones drawn last. */
    RENDER_LIST0 = 0x00ea66e4u,
    RENDER_LIST1 = 0x00ea66ecu,
    RENDER_LIST2 = 0x00ea66d0u,
    /* Last draw state a queued block applied (0x287640 re-applies one). */
    RENDER_SORT_KEY = 0x00ea66c0u,
    /* Model being drawn, and the sphere its cull test reads. */
    RENDER_OBJECT = 0x00ea6694u,
    /* Draw flags: 1 vertex blend, 2 alpha test, 4 normalize normals. */
    RENDER_FLAGS = 0x00ea6698u,
    /* Bits 20-22 of the draw flags skip the material, stages and draw. */
    RENDER_SKIP = 0x00ea669cu,
    /* First blend matrix and how many of them the draw applies. */
    RENDER_MATRIX_SOURCE = 0x00ea66a0u,
    RENDER_MATRIX_COUNT = 0x00ea66a4u,
    /* Material diffuse alpha scale, 1.0 while the model is opaque. */
    RENDER_FADE = 0x00ea66a8u,
    /* Raised while a queued model waits for the near-block promotion. */
    RENDER_PROMOTE_PENDING = 0x00ea6690u,
    /* Six installable frame hooks, noop (0x4ba60) unless the game sets one. */
    RENDER_HOOKS = 0x00ea6674u
};

/* A draw block of a model's chain, offsets from the block:
   0x00 flags; the low 3 bits are the block type, 0x40000000 draws without the
        visibility test
   0x04 byte offset to the next block when this one is skipped
   0x0c render states, compared against the cached ones by 0x286c00
   0x10 float x, y, z, radius of the block's bounding sphere
   0x20 D3DMATERIAL8, 68 bytes; its diffuse alpha at 0x2c is the fade scale
   0x64 material stage count
   0x68 more render states, read by 0x286c00
   0x70 the material stages, 0x10 bytes each
   after them a 0x10-byte draw record:
   0x00 unused, 0x02 vertex shader id, 0x04 vertex offset, 0x08 index offset.
   The next block follows the draw record. */
enum {
    BLOCK_FLAGS = 0x00u,
    BLOCK_NEXT_OFFSET = 0x04u,
    BLOCK_BOUNDS = 0x10u,
    BLOCK_STAGE_COUNT = 0x64u,
    BLOCK_STAGES = 0x70u,
    BLOCK_STAGE_BYTES = 0x10u,
    BLOCK_DRAW_RECORD_BYTES = 0x10u,
    BLOCK_ALWAYS_DRAW = 0x40000000u
};

/* Tables and scratch the draw state points at (Xbox image constants). */
enum {
    /* Pass 0's block-class table: [pass * 8 + block type] -> class 0..7. */
    RENDER_CLASS_TABLE_PASS0 = 0x007b0288u,
    RENDER_CLASS_TABLE_PASS_BYTES = 0x20u,
    /* World matrix this renderer draws static blocks with (g_matrix_current). */
    GUEST_MATRIX_CURRENT = 0x00ea6080u,
    /* Four 4x4 blend matrices, filled by the model setup. */
    RENDER_BLEND_MATRICES = 0x00ea6730u,
    RENDER_MATRIX_BYTES = 0x40u,
    /* Vertex-blend render state and world-matrix slot per blend matrix. */
    RENDER_VERTEX_BLEND_STATES = 0x007b0258u,
    RENDER_BLEND_TRANSFORMS = 0x007b0268u,
    /* Alpha test reference the game sets before a faded draw (0x243400). */
    RENDER_ALPHA_REF = 0x00ea6830u
};

/* D3D8 device shadow state the draw touches. */
enum {
    D3D_DIRTY = 0x00355058u,
    /* Per-stage shadow blocks, 0x80 bytes each: 0x4c carries the fade alpha. */
    D3D_STAGE_SHADOW = 0x00355040u,
    D3D_STAGE_SHADOW_BYTES = 0x0080u,
    D3D_STAGE_SHADOW_ALPHA = 0x004cu,
    /* Cached NV2A blend-enable and alpha-reference values. */
    D3D_BLEND_SHADOW = 0x0035534cu,
    D3D_ALPHA_REF_SHADOW = 0x00355354u,
    /* NV2A methods 0x304 and 0x340, as D3DDevice_SetRenderState_Simple takes
       them (ECX = method, EDX = value). */
    D3D_METHOD_BLEND_ENABLE = 0x00040304u,
    D3D_METHOD_ALPHA_REF = 0x00040340u,
    /* D3DTS_WORLD in this device's transform-slot numbering. */
    D3D_TRANSFORM_WORLD = 6u
};

/* ---- Guest calls ------------------------------------------------------- */

/* Callees, in table order. Each is resolved once per boot generation the way
   RECOMP_ABI_CALL does, so another group's replacement still runs while a draw
   block pays no indirect-dispatch table scan. */
enum {
    CALLEE_SPHERE_VISIBLE,       /* 0x00286790 bounding sphere vs the view cone */
    CALLEE_APPLY_MATERIAL,       /* 0x00286c00 bind a block's material and states */
    CALLEE_BIND_STAGES,          /* 0x00286d30 bind the material's texture stages */
    CALLEE_DRAW_RECORD,          /* 0x00287000 bind streams and draw the block */
    CALLEE_QUEUE_BLOCK,          /* 0x002871c0 queue a block for a sorted list */
    CALLEE_QUEUE_DEFERRED,       /* 0x00287440 queue a block for the deferred list */
    CALLEE_SORT_LIST,            /* 0x00289500 sort a queued list by view depth */
    CALLEE_PROMOTE_NEAR,         /* 0x00286b70 move near blocks to the front list */
    CALLEE_SUBMIT_LIST,          /* 0x00288900 submit one queued list */
    CALLEE_SET_TRANSFORM,        /* 0x00344210 D3DDevice_SetTransform */
    CALLEE_KICK,                 /* 0x00344440 D3DDevice_KickPushBuffer */
    CALLEE_SET_TEXTURE,          /* 0x00345490 D3DDevice_SetTexture */
    CALLEE_SET_INDICES,          /* 0x00345700 D3DDevice_SetIndices */
    CALLEE_SET_STREAM_SOURCE,    /* 0x00348f20 D3DDevice_SetStreamSource */
    CALLEE_SET_STATE,            /* 0x00346240 D3DDevice_SetRenderState_Simple */
    CALLEE_NORMALIZE_NORMALS,    /* 0x003466c0 ..._SetRenderState_NormalizeNormals */
    CALLEE_VERTEX_BLEND,         /* 0x00346a60 ..._SetRenderState_VertexBlend */
    CALLEE_COUNT
};

static struct {
    uint32_t address;
    RecompFunction target;
} render_callees[CALLEE_COUNT] = {
    {0x00286790u, NULL},
    {0x00286c00u, NULL},
    {0x00286d30u, NULL},
    {0x00287000u, NULL},
    {0x002871c0u, NULL},
    {0x00287440u, NULL},
    {0x00289500u, NULL},
    {0x00286b70u, NULL},
    {0x00288900u, NULL},
    {0x00344210u, NULL},
    {0x00344440u, NULL},
    {0x00345490u, NULL},
    {0x00345700u, NULL},
    {0x00348f20u, NULL},
    {0x00346240u, NULL},
    {0x003466c0u, NULL},
    {0x00346a60u, NULL}
};

static RecompFunction render_callee(unsigned callee)
{
    static uint32_t generation;

    if (generation != recomp_program_lookup_generation) {
        generation = recomp_program_lookup_generation;
        for (unsigned i = 0u; i < (unsigned)CALLEE_COUNT; ++i)
            render_callees[i].target = recomp_program_lookup(render_callees[i].address);
    }
    return render_callees[callee].target;
}

/* Enters a callee the way its CALL site left the stack - arguments above the
   return address - and puts ESP back afterwards, so a callee that cleans its
   own arguments and one that leaves them both work. */
static uint32_t render_invoke(unsigned callee, const uint32_t *arguments, unsigned count)
{
    const RecompFunction target = render_callee(callee);
    const uint32_t saved_esp = recomp_runtime.registers.esp;

    if (target == NULL) return recomp_runtime.registers.eax; /* no body: unreached */
    for (unsigned i = count; i > 0u; --i) {
        recomp_runtime.registers.esp -= 4u;
        *recomp_memory_u32(recomp_runtime.registers.esp) = arguments[i - 1u];
    }
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u; /* return address */
    target();
    recomp_runtime.registers.esp = saved_esp;
    return recomp_runtime.registers.eax;
}

/* 0x00286790 (fastcall): is the sphere at ECX inside the view cone. */
static uint32_t render_sphere_visible(uint32_t sphere)
{
    recomp_runtime.registers.ecx = sphere;
    return render_invoke(CALLEE_SPHERE_VISIBLE, NULL, 0u);
}

/* 0x00286c00: bind the block's material and the render states it changes. */
static void render_apply_material(uint32_t block)
{
    render_invoke(CALLEE_APPLY_MATERIAL, &block, 1u);
}

/* 0x00286d30: bind `count` material stages, the stages being at EAX - this
   routine takes its array through a register, so its caller loads one. */
static void render_bind_stages(uint32_t stages, uint32_t count)
{
    recomp_runtime.registers.eax = stages;
    render_invoke(CALLEE_BIND_STAGES, &count, 1u);
}

/* 0x00287000: bind the vertex stream and indices the record names, then draw. */
static void render_draw_record(uint32_t record)
{
    render_invoke(CALLEE_DRAW_RECORD, &record, 1u);
}

/* 0x002871c0: append the block to the depth-sorted list at `list`. It takes
   the block through EBX, like its caller here, and returns the next block. */
static uint32_t render_queue_block(uint32_t block, uint32_t flags, uint32_t list)
{
    const uint32_t arguments[2] = {flags, list};

    recomp_runtime.registers.ebx = block;
    return render_invoke(CALLEE_QUEUE_BLOCK, arguments, 2u);
}

/* 0x00287440: append the block in EBX to the deferred list, no stack arguments. */
static uint32_t render_queue_deferred(uint32_t block)
{
    recomp_runtime.registers.ebx = block;
    return render_invoke(CALLEE_QUEUE_DEFERRED, NULL, 0u);
}

static void render_sort_list(uint32_t list)
{
    render_invoke(CALLEE_SORT_LIST, &list, 1u);
}

static void render_promote_near_blocks(void)
{
    render_invoke(CALLEE_PROMOTE_NEAR, NULL, 0u);
}

/* 0x00288900: apply each queued entry's draw state and draw its block. */
static void render_submit_list(uint32_t list)
{
    render_invoke(CALLEE_SUBMIT_LIST, &list, 1u);
}

/* Same call with the list argument already pushed, as the original keeps it
   on the stack while the frame hooks between the submissions run. */
static void render_submit_pushed_list(void)
{
    render_invoke(CALLEE_SUBMIT_LIST, NULL, 0u);
}

/* The originals keep their saved registers and pending arguments on the stack;
   a callee that reads past its arguments must see the same window, so the
   replacements push the same words at the same points. */
static void render_push(uint32_t value)
{
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = value;
}

static void render_set_transform(uint32_t state, uint32_t matrix)
{
    const uint32_t arguments[2] = {state, matrix};

    render_invoke(CALLEE_SET_TRANSFORM, arguments, 2u);
}

static void render_kick(void)
{
    render_invoke(CALLEE_KICK, NULL, 0u);
}

static void render_set_texture(uint32_t stage, uint32_t texture)
{
    const uint32_t arguments[2] = {stage, texture};

    render_invoke(CALLEE_SET_TEXTURE, arguments, 2u);
}

static void render_set_indices(uint32_t buffer, uint32_t base)
{
    const uint32_t arguments[2] = {buffer, base};

    render_invoke(CALLEE_SET_INDICES, arguments, 2u);
}

static void render_set_stream_source(uint32_t stream, uint32_t buffer, uint32_t stride)
{
    const uint32_t arguments[3] = {stream, buffer, stride};

    render_invoke(CALLEE_SET_STREAM_SOURCE, arguments, 3u);
}

/* A D3D8 push-buffer render state: method in ECX, value in EDX, no arguments. */
static void render_set_state(uint32_t method, uint32_t value)
{
    recomp_runtime.registers.ecx = method;
    recomp_runtime.registers.edx = value;
    render_invoke(CALLEE_SET_STATE, NULL, 0u);
}

static void render_normalize_normals(uint32_t enable)
{
    render_invoke(CALLEE_NORMALIZE_NORMALS, &enable, 1u);
}

static void render_vertex_blend(uint32_t mode)
{
    render_invoke(CALLEE_VERTEX_BLEND, &mode, 1u);
}

/* A frame hook the game installs at 0xea6674; addressed at call time because
   the game swaps them, and cached by recomp_program_lookup. */
static void render_hook(unsigned index)
{
    const uint32_t address = *recomp_memory_u32(RENDER_HOOKS + 4u * index);
    const RecompFunction target = recomp_program_lookup(address);
    const uint32_t saved_esp = recomp_runtime.registers.esp;

    if (target == NULL) return;
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u; /* return address */
    target();
    recomp_runtime.registers.esp = saved_esp;
}

static float guest_float(uint32_t address)
{
    const uint32_t bits = *recomp_memory_u32(address);
    float value;

    memcpy(&value, &bits, sizeof value);
    return value;
}

static void set_guest_float(uint32_t address, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof bits);
    *recomp_memory_u32(address) = bits;
}

/* Draws a block now: material, then its stages, then its draw record; returns
   the next block. None of the three callees reads a register beyond its stack
   arguments, except 0x286d30's stage array, which arrives in EAX. */
static uint32_t render_draw_block(uint32_t block)
{
    render_apply_material(block);

    const uint32_t stages = *recomp_memory_u32(block + BLOCK_STAGE_COUNT);
    const uint32_t record = block + BLOCK_STAGES + stages * BLOCK_STAGE_BYTES;

    render_bind_stages(block + BLOCK_STAGES, stages);
    render_draw_record(record);
    return record + BLOCK_DRAW_RECORD_BYTES;
}

/* 0x00288a00: finish a frame's draw lists. Sorts the sorted list, promotes the
   near blocks, runs the six frame hooks around the three list submissions
   (with a fresh material-stage binding after the first), then unbinds the
   streams, indices and textures the pass left behind. */
static void render_draw_flush_lists(void)
{
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_esp = recomp_runtime.registers.esp;

    render_sort_list(RENDER_LIST1);
    if (*recomp_memory_u32(RENDER_PROMOTE_PENDING) != 0u) render_promote_near_blocks();

    render_push(saved_esi);            /* 0x288a1b: PUSH ESI */
    render_hook(0u);
    render_submit_list(RENDER_LIST0);
    render_bind_stages(0u, 0u);
    render_hook(1u);
    render_push(RENDER_LIST1);         /* 0x288a3e: the hooks below run with it */
    render_submit_pushed_list();
    render_hook(2u);
    render_hook(3u);
    render_hook(4u);
    render_push(RENDER_LIST2);
    render_submit_pushed_list();
    recomp_runtime.registers.esp += 8u; /* 0x288a64: ADD ESP,8 */
    render_hook(5u);

    render_kick();
    render_set_stream_source(0u, 0u, 0u);
    render_set_indices(0u, 0u);
    for (uint32_t stage = 0u; stage < 4u; ++stage) render_set_texture(stage, 0u);

    recomp_runtime.registers.esp = saved_esp; /* 0x288a96: POP ESI */
    recomp_runtime.registers.esi = saved_esi;
    kernel_return_caller_cleanup(recomp_runtime.registers.eax);
}

/* 0x00288aa0: queue or draw one model's block chain with a single world
   matrix and the model's material. fastcall, so ECX is the model. */
static void render_draw_model(void)
{
    const uint32_t model = recomp_runtime.registers.ecx;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_esp = recomp_runtime.registers.esp;
    uint32_t block;

    render_push(saved_esi);            /* 0x288aa0: PUSH ESI */
    if (render_sphere_visible(model + BLOCK_BOUNDS) == 0u) {
        recomp_runtime.registers.esp = saved_esp;
        recomp_runtime.registers.ebx = saved_ebx;
        recomp_runtime.registers.esi = saved_esi;
        kernel_return_caller_cleanup(recomp_runtime.registers.eax);
        return;
    }
    render_push(saved_ebx);            /* 0x288ab6: PUSH EBX */

    *recomp_memory_u32(RENDER_OBJECT) = model;
    *recomp_memory_u32(RENDER_BLOCK_BASE) = model + 0x20u;
    *recomp_memory_u32(RENDER_VERTEX_SHADER) = 0xffffffffu;
    *recomp_memory_u32(RENDER_BLEND_SET) = *recomp_memory_u32(model + 4u);
    *recomp_memory_u32(RENDER_SORT_KEY) = 0u;
    *recomp_memory_u32(RENDER_CLASS_TABLE) = RENDER_CLASS_TABLE_PASS0;
    *recomp_memory_u32(RENDER_MATRIX_SOURCE) = GUEST_MATRIX_CURRENT;
    *recomp_memory_u32(RENDER_MATRIX_COUNT) = 1u;
    render_set_transform(D3D_TRANSFORM_WORLD, GUEST_MATRIX_CURRENT);

    block = *recomp_memory_u32(RENDER_BLOCK_BASE) +
        (*recomp_memory_u32(model + 8u) << 5);
    set_guest_float(RENDER_FADE, 1.0f);
    *recomp_memory_u32(RENDER_FLAGS) = 0u;
    *recomp_memory_u32(RENDER_SKIP) = 0u;

    for (uint32_t flags = *recomp_memory_u32(block + BLOCK_FLAGS); flags != 0u;
         flags = *recomp_memory_u32(block + BLOCK_FLAGS)) {
        recomp_runtime.registers.ebx = block;
        if ((flags & BLOCK_ALWAYS_DRAW) == 0u &&
            render_sphere_visible(block + BLOCK_BOUNDS) == 0u) {
            block += *recomp_memory_u32(block + BLOCK_NEXT_OFFSET);
            continue;
        }
        /* Types 1, 3, 4 and 5 queue to a list; 0, 2, 6 and 7 draw now. */
        switch (flags & 7u) {
        case 1u: block = render_queue_block(block, flags, RENDER_LIST0); break;
        case 3u: block = render_queue_block(block, flags, RENDER_LIST1); break;
        case 4u: block = render_queue_block(block, flags, RENDER_LIST2); break;
        case 5u: block = render_queue_deferred(block); break;
        default: block = render_draw_block(block); break;
        }
    }

    recomp_runtime.registers.esp = saved_esp; /* 0x288bde: POP EBX, POP ESI */
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    kernel_return_caller_cleanup(recomp_runtime.registers.eax);
}

/* 0x00288e40: the flagged variant of 0x288aa0. fastcall with ECX the model,
   EDX the draw flags and a float fade scale as its one stack argument. Fades
   the model's material alpha, applies up to four blend matrices, turns
   blending on while the fade is under 1.0 or the block is not type 0, and runs
   the alpha test and normal normalization the flags ask for, then resets all
   of them. */
static void render_draw_model_faded(void)
{
    const uint32_t model = recomp_runtime.registers.ecx;
    const uint32_t flags = recomp_runtime.registers.edx;
    const float fade = guest_float(recomp_runtime.registers.esp + 4u);
    const uint32_t blend = flags & 1u;
    const uint32_t saved_ebx = recomp_runtime.registers.ebx;
    const uint32_t saved_esi = recomp_runtime.registers.esi;
    const uint32_t saved_edi = recomp_runtime.registers.edi;
    const uint32_t saved_ebp = recomp_runtime.registers.ebp;
    const uint32_t saved_esp = recomp_runtime.registers.esp;
    bool blending = false;
    uint32_t block;

    recomp_runtime.registers.esp -= 0x10u; /* 0x288e40: SUB ESP,0x10 */
    render_push(saved_ebx);                /* PUSH EBX */
    render_push(saved_ebp);                /* PUSH EBP */
    /* COMISS 0.0, fade: draws while 0.0 is below it, and for a NaN fade. */
    if (0.0f >= fade) goto done;
    render_push(saved_edi);                /* 0x288e5f: PUSH EDI */
    if (blend == 0u && render_sphere_visible(model + BLOCK_BOUNDS) == 0u) goto done;
    render_push(saved_esi);                /* 0x288e86: PUSH ESI */

    *recomp_memory_u32(RENDER_OBJECT) = model;
    *recomp_memory_u32(RENDER_BLOCK_BASE) = model + 0x20u;
    *recomp_memory_u32(RENDER_VERTEX_SHADER) = 0xffffffffu;
    *recomp_memory_u32(RENDER_BLEND_SET) = *recomp_memory_u32(model + 4u);
    *recomp_memory_u32(RENDER_SORT_KEY) = 0u;
    *recomp_memory_u32(RENDER_CLASS_TABLE) = RENDER_CLASS_TABLE_PASS0 +
        ((flags >> 8) & 7u) * RENDER_CLASS_TABLE_PASS_BYTES;

    if (blend != 0u) {
        const uint32_t matrices = *recomp_memory_u32(model + 4u);

        *recomp_memory_u32(RENDER_MATRIX_SOURCE) = RENDER_BLEND_MATRICES;
        *recomp_memory_u32(RENDER_MATRIX_COUNT) = matrices + 1u;
        if ((int32_t)matrices >= 0) {
            uint32_t matrix = RENDER_BLEND_MATRICES + matrices * RENDER_MATRIX_BYTES;

            for (int32_t index = (int32_t)matrices; index >= 0; --index) {
                render_set_transform(
                    *recomp_memory_u32(RENDER_BLEND_TRANSFORMS + 4u * (uint32_t)index),
                    matrix);
                matrix -= RENDER_MATRIX_BYTES;
            }
        }
        render_vertex_blend(*recomp_memory_u32(RENDER_VERTEX_BLEND_STATES + 4u * matrices));
    } else {
        *recomp_memory_u32(RENDER_MATRIX_SOURCE) = GUEST_MATRIX_CURRENT;
        *recomp_memory_u32(RENDER_MATRIX_COUNT) = 1u;
        render_set_transform(D3D_TRANSFORM_WORLD, GUEST_MATRIX_CURRENT);
    }

    if ((flags & 2u) != 0u) {
        const uint32_t alpha_ref = *recomp_memory_u32(RENDER_ALPHA_REF);

        *recomp_memory_u32(D3D_DIRTY) |= 3u;
        *recomp_memory_u32(D3D_STAGE_SHADOW + D3D_STAGE_SHADOW_ALPHA) = 4u;
        *recomp_memory_u32(D3D_STAGE_SHADOW + D3D_STAGE_SHADOW_BYTES +
                           D3D_STAGE_SHADOW_ALPHA) = 4u;
        render_set_state(D3D_METHOD_ALPHA_REF, alpha_ref);
        *recomp_memory_u32(D3D_ALPHA_REF_SHADOW) = alpha_ref;
    }
    if ((flags & 4u) != 0u) render_normalize_normals(1u);

    block = *recomp_memory_u32(RENDER_BLOCK_BASE) +
        (*recomp_memory_u32(model + 8u) << 5);
    *recomp_memory_u32(RENDER_FLAGS) = flags;
    set_guest_float(RENDER_FADE, fade);
    *recomp_memory_u32(RENDER_SKIP) = 0u;

    for (uint32_t type = *recomp_memory_u32(block + BLOCK_FLAGS); type != 0u;
         type = *recomp_memory_u32(block + BLOCK_FLAGS)) {
        recomp_runtime.registers.ebx = type;
        if (blend == 0u && (type & BLOCK_ALWAYS_DRAW) == 0u &&
            render_sphere_visible(block + BLOCK_BOUNDS) == 0u) {
            block += *recomp_memory_u32(block + BLOCK_NEXT_OFFSET);
            continue;
        }
        /* A faded block, or one that is not the plain type, blends. */
        if (!blending && ((1.0f > fade) || (type & 7u) != 0u)) {
            render_set_state(D3D_METHOD_BLEND_ENABLE, 1u);
            *recomp_memory_u32(D3D_BLEND_SHADOW) = 1u;
            blending = true;
        }
        const uint32_t class = *recomp_memory_u32(
            *recomp_memory_u32(RENDER_CLASS_TABLE) + 4u * (type & 7u));

        if (class == 1u) {
            block = render_queue_block(block, type, RENDER_LIST0);
        } else if (class == 3u) {
            block = render_queue_block(block, type, RENDER_LIST1);
        } else if (class == 4u) {
            block = render_queue_block(block, type, RENDER_LIST2);
        } else {
            block = render_draw_block(block);
        }
    }

    if (blend != 0u) render_vertex_blend(0u);
    if ((flags & 2u) != 0u) {
        uint32_t dirty = *recomp_memory_u32(D3D_DIRTY);

        *recomp_memory_u32(D3D_STAGE_SHADOW + D3D_STAGE_SHADOW_ALPHA) = 0u;
        *recomp_memory_u32(D3D_DIRTY) = dirty | 3u;
        *recomp_memory_u32(D3D_STAGE_SHADOW + D3D_STAGE_SHADOW_BYTES +
                           D3D_STAGE_SHADOW_ALPHA) = 0u;
        render_set_state(D3D_METHOD_ALPHA_REF, 1u);
        *recomp_memory_u32(D3D_ALPHA_REF_SHADOW) = 1u;
    }
    if ((flags & 4u) != 0u) render_normalize_normals(0u);
    if (blending) {
        render_set_state(D3D_METHOD_BLEND_ENABLE, 0u);
        *recomp_memory_u32(D3D_BLEND_SHADOW) = 0u;
    }

done:
    /* 0x2890db: POP ESI, POP EDI, POP EBP, POP EBX, ADD ESP,0x10 */
    recomp_runtime.registers.esp = saved_esp;
    recomp_runtime.registers.ebx = saved_ebx;
    recomp_runtime.registers.esi = saved_esi;
    recomp_runtime.registers.edi = saved_edi;
    recomp_runtime.registers.ebp = saved_ebp;
    kernel_return(1u, recomp_runtime.registers.eax);
}

RecompFunction recomp_render_draw_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x00288A00u: return render_draw_flush_lists;
    case 0x00288AA0u: return render_draw_model;
    case 0x00288E40u: return render_draw_model_faded;
    default: return NULL;
    }
}
