/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled renderer mesh setup, 0x282ef0-0x283c60. Each
   replacement keeps the lifted function's guest RAM writes, register results
   and float rounding, so rollback peers and SyncTest see the same state.

   These are the small vector helpers the renderer walks its mesh and light
   lists with: the reciprocal square root and its square root companion, the
   in-place normalise, the dot and cross products, an in-place "scale to
   length" and the component of a homogeneous vector perpendicular to an axis.
   The x87 routines return their result on the x87 stack, so every replacement
   pushes it there too. The guest keeps 64-bit x87 precision (control word
   0x037f), but a routine may select 24-bit precision, so intermediate x87
   results go through RECOMP_FP_PC exactly as the lifted bodies do.

   Two guest-visible details of these tiny routines are easy to miss. Several
   spill their result, or a saved register, into the frame below the caller's
   stack pointer, and a caller can read that scrap back through a wide copy,
   so the same slots get the same values here. And 0x283c60 moves a whole
   16-byte slot, whose fourth lane it neither writes nor initialises. */
#include "game_decomp.h"
#include "kernel_abi.h"
#include "recomp_types.h"

/* recomp_types.h maps the guest register names onto the runtime fields; this
   file reads the registers by their runtime fields like the rest of the
   runtime sources, so drop the macros before the first definition. */
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esi
#undef edi
#undef esp
#undef ebp
#undef g_esp
#undef g_ebp
#undef g_seh_ebp

/* Guest float constants these routines read from the folded .rdata pools:
   0.0f at 0x3c2a10 (f32_zero), 0.5f at 0x444d20 (f32_half) and 1.5f at
   0x444d40 (f32_three_halves). They are constant data, so the values below are
   a cache of them. */
#define GUEST_F32_ZERO 0.0f
#define GUEST_F32_HALF 0.5f
#define GUEST_F32_ONE_HALF 1.5f

/* Guest vector layouts: the mesh and light data are plain float triples
   passed by pointer, and 0x283c60 moves a whole 16-byte slot. */
typedef struct RenderVec3 {
    float x;
    float y;
    float z;
} RenderVec3;

typedef struct RenderVec4 {
    float x;
    float y;
    float z;
    float w;
} RenderVec4;

static RenderVec3 *guest_vec3(uint32_t address)
{
    return (RenderVec3 *)(void *)recomp_memory(address, sizeof(RenderVec3));
}

static RenderVec4 *guest_vec4(uint32_t address)
{
    return (RenderVec4 *)(void *)recomp_memory(address, sizeof(RenderVec4));
}

static float guest_f32(uint32_t address)
{
    return *(const float *)(void *)recomp_memory(address, sizeof(float));
}

static uint32_t guest_u32(uint32_t address)
{
    return *recomp_memory_u32(address);
}

/* Writes a value the lifted body spills into its own frame, just below the
   caller's stack pointer. */
static void spill_f32(uint32_t address, float value)
{
    *(float *)(void *)recomp_memory(address, sizeof value) = value;
}

/* Pushes a value as the x87 ST(0) the original's `fld float` leaves; the
   guest's own stack pointer is untouched. */
static void push_st0(double value)
{
    const uint32_t top = (recomp_runtime.fpu_top + 7u) & 7u;

    recomp_runtime.fpu_top = top;
    recomp_runtime.fpu_stack[top] = value;
}

/* 0x282f50's arithmetic: one Newton step on the RSQRTSS estimate,
   1.5 * e - 0.5 * x * e * e * e, in the original's SSE operation order. */
static float rsqrt_refined(float value)
{
    const float estimate = recomp_rsqrtss(value);
    const float cube = value * estimate * estimate * estimate;
    const float correction = cube * GUEST_F32_HALF;
    const float scaled = estimate * GUEST_F32_ONE_HALF;

    return scaled - correction;
}

static void render_fast_rsqrt(void)
{
    /* 0x282f50: float in, reciprocal square root out on the x87 stack. A zero
       input returns positive zero instead of dividing by it. The original
       keeps its result in its frame, over the ECX it saved, and a caller can
       read that scrap back. */
    const uint32_t stack = recomp_runtime.registers.esp;
    const float value = guest_f32(stack + 4u);
    const float result = value == GUEST_F32_ZERO ? GUEST_F32_ZERO
        : rsqrt_refined(value);

    *recomp_memory_u32(stack - 4u) = recomp_runtime.registers.ebp;
    spill_f32(stack - 8u, result);
    push_st0((double)result);
    kernel_return_caller_cleanup(0u);
}

static void render_fast_sqrt(void)
{
    /* 0x282ef0: the same estimate times its input. The original spills the
       float into its argument slot and reloads it for the x87 result, so the
       slot is written here too. */
    const uint32_t argument = recomp_runtime.registers.esp + 4u;
    const float value = guest_f32(argument);
    const float rooted = value == GUEST_F32_ZERO ? GUEST_F32_ZERO
        : rsqrt_refined(value) * value;

    /* PUSH EBP of the original's frame. */
    *recomp_memory_u32(argument - 8u) = recomp_runtime.registers.ebp;
    if (value != GUEST_F32_ZERO) spill_f32(argument, rooted);
    push_st0((double)rooted);
    kernel_return_caller_cleanup(0u);
}

static void render_vec3_normalize(void)
{
    /* 0x283840: normalises the vector ECX points at, in place. A vector of
       zero (or NaN) squared length is left alone, as the original's
       comiss/jbe is, but its squared length is still spilled into the frame
       first. */
    const uint32_t stack = recomp_runtime.registers.esp;
    RenderVec3 *vector = guest_vec3(recomp_runtime.registers.ecx);
    const float xx = vector->x * vector->x;
    const float yy = vector->y * vector->y;
    const float zz = vector->z * vector->z;
    const float length_squared = (xx + yy) + zz;

    /* PUSH ECX, PUSH ESI, then the length spill over the saved ECX. */
    spill_f32(stack - 4u, length_squared);
    *recomp_memory_u32(stack - 8u) = recomp_runtime.registers.esi;
    if (length_squared > GUEST_F32_ZERO) {
        const double scale = (double)rsqrt_refined(length_squared);
        const float x = (float)RECOMP_FP_PC(scale * (double)vector->x);
        const float y = (float)RECOMP_FP_PC(scale * (double)vector->y);
        const float z = (float)RECOMP_FP_PC(scale * (double)vector->z);

        /* The inlined call's frame: PUSH EAX, the return address, then the
           callee's PUSH EBP and its result spill. */
        spill_f32(stack - 12u, length_squared);
        *recomp_memory_u32(stack - 16u) = 0x00283885u;
        *recomp_memory_u32(stack - 20u) = recomp_runtime.registers.ebp;
        spill_f32(stack - 24u, rsqrt_refined(length_squared));
        vector->x = x;
        vector->y = y;
        vector->z = z;
    }
    kernel_return_caller_cleanup(0u);
}

static void render_vec3_dot(void)
{
    /* 0x283aa0: dot product of the vectors in ECX and EDX, accumulated z, y, x
       and left on the x87 stack. */
    const RenderVec3 *left = guest_vec3(recomp_runtime.registers.ecx);
    const RenderVec3 *right = guest_vec3(recomp_runtime.registers.edx);
    const double z = RECOMP_FP_PC((double)left->z * (double)right->z);
    const double y = RECOMP_FP_PC((double)left->y * (double)right->y);
    const double zy = RECOMP_FP_PC(z + y);
    const double x = RECOMP_FP_PC((double)left->x * (double)right->x);

    push_st0(RECOMP_FP_PC(zy + x));
    kernel_return_caller_cleanup(0u);
}

static void render_vec3_cross(void)
{
    /* 0x283ac0: cdecl (const vec3 *left, const vec3 *right, vec3 *out);
       out = left x right. Each lane is computed from memory, so an out vector
       that aliases an input behaves as the original does. */
    const uint32_t stack = recomp_runtime.registers.esp;
    const RenderVec3 *left = guest_vec3(guest_u32(stack + 4u));
    const RenderVec3 *right = guest_vec3(guest_u32(stack + 8u));
    RenderVec3 *out = guest_vec3(guest_u32(stack + 12u));

    out->x = right->z * left->y - right->y * left->z;
    out->y = right->x * left->z - left->x * right->z;
    out->z = left->x * right->y - right->x * left->y;
    kernel_return_caller_cleanup(0u);
}

static void render_vec3_set_length(void)
{
    /* 0x283b80: cdecl (vec3 *vector, float length); scales the vector in
       place to the given length. A zero or NaN squared length leaves it
       alone. The original spills that length over its first argument slot. */
    const uint32_t stack = recomp_runtime.registers.esp;
    RenderVec3 *vector = guest_vec3(guest_u32(stack + 4u));
    const float length = guest_f32(stack + 8u);
    const float xx = vector->x * vector->x;
    const float yy = vector->y * vector->y;
    const float zz = vector->z * vector->z;
    const float length_squared = (xx + yy) + zz;

    spill_f32(stack + 4u, length_squared);
    *recomp_memory_u32(stack - 4u) = recomp_runtime.registers.esi;
    if (length_squared > GUEST_F32_ZERO) {
        const double scale = RECOMP_FP_PC(
            (double)rsqrt_refined(length_squared) * (double)length);
        const float x = (float)RECOMP_FP_PC(scale * (double)vector->x);
        const float y = (float)RECOMP_FP_PC(scale * (double)vector->y);
        const float z = (float)RECOMP_FP_PC(scale * (double)vector->z);

        /* The inlined call's frame: PUSH EAX, the return address, then the
           callee's PUSH EBP and its result spill. */
        spill_f32(stack - 8u, length_squared);
        *recomp_memory_u32(stack - 12u) = 0x00283bc6u;
        *recomp_memory_u32(stack - 16u) = recomp_runtime.registers.ebp;
        spill_f32(stack - 20u, rsqrt_refined(length_squared));
        vector->x = x;
        vector->y = y;
        vector->z = z;
    }
    kernel_return_caller_cleanup(0u);
}

static void render_vec4_remove_axis(void)
{
    /* 0x283c60: cdecl (vec4 *out, const vec4 *point, const vec4 *axis);
       removes the axis component of point: out = point - axis * (point . axis
       / axis . axis). A zero dot product or axis copies point. The original
       spells the scaled axis into a 16-byte aligned slot in its own frame and
       subtracts the whole slot, so out->w is point->w minus whatever that
       slot's fourth float held; the slot is rebuilt at the same guest address
       so the guest sees the same stack contents and the same out->w. */
    const uint32_t stack = recomp_runtime.registers.esp;
    const uint32_t out_address = guest_u32(stack + 4u);
    const uint32_t point_address = guest_u32(stack + 8u);
    const uint32_t axis_address = guest_u32(stack + 12u);
    const RenderVec4 point = *guest_vec4(point_address);
    const RenderVec4 axis = *guest_vec4(axis_address);
    const float axis_xx = axis.x * axis.x;
    const float axis_yy = axis.y * axis.y;
    const float axis_zz = axis.z * axis.z;
    const float axis_squared = (axis_xx + axis_yy) + axis_zz;
    const float dot = ((point.z * axis.z) + (point.y * axis.y)) +
        (point.x * axis.x);

    /* PUSH EBP of the original's frame, in both paths. */
    *recomp_memory_u32(stack - 4u) = recomp_runtime.registers.ebp;
    if (dot != GUEST_F32_ZERO && axis_squared != GUEST_F32_ZERO) {
        const float ratio = dot / axis_squared;
        /* MOV EBP,ESP; AND ESP,-16; SUB ESP,16: the slot the original
           subtracts from point. */
        const uint32_t slot_address = ((stack - 4u) & 0xfffffff0u) - 16u;
        float *slot = (float *)(void *)recomp_memory(slot_address, 16u);
        RenderVec4 *out = guest_vec4(out_address);
        const float stale = slot[3];

        slot[0] = axis.x * ratio;
        slot[1] = axis.y * ratio;
        slot[2] = axis.z * ratio;
        out->x = point.x - slot[0];
        out->y = point.y - slot[1];
        out->z = point.z - slot[2];
        out->w = point.w - stale;
    } else if (point_address != out_address) {
        *guest_vec4(out_address) = point;
    }
    kernel_return_caller_cleanup(0u);
}

RecompFunction recomp_render_mesh_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x00282ef0u: return render_fast_sqrt;
    case 0x00282f50u: return render_fast_rsqrt;
    case 0x00283840u: return render_vec3_normalize;
    case 0x00283aa0u: return render_vec3_dot;
    case 0x00283ac0u: return render_vec3_cross;
    case 0x00283b80u: return render_vec3_set_length;
    case 0x00283c60u: return render_vec4_remove_axis;
    default: return NULL;
    }
}
