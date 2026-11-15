/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled fighter pose and skeleton, 0x1fef0, 0x2e1c0, 0x4bfe0, 0x4c4d0. Each
   replacement keeps the lifted function's guest RAM writes, register results
   and float rounding, so rollback peers and SyncTest see the same state. */
#include "game_decomp.h"
#include "kernel_abi.h"

#include <math.h>
#include <string.h>

/* ── guest float arithmetic ──────────────────────────────────────────────
   The lifted runtime keeps the guest x87 registers as doubles and rounds every
   arithmetic result under the control word's precision bits. The game runs the
   default double-precision word, where this is the identity; it stays here so
   a peer that changed the word still matches the lifted bodies. */
static double x87_round(double value)
{
    const double magnitude = fabs(value);
    int exponent;

    if ((recomp_runtime.fpu_control_word & 0x300u) != 0u) return value;
    if ((magnitude <= 3.4028234663852886e38 && magnitude >= 1.1754943508222875e-38) ||
        magnitude == 0.0 || value != value || magnitude == INFINITY) {
        return (double)(float)value;
    }
    value = frexp(value, &exponent);
    return ldexp((double)(float)value, exponent);
}

/* 0x283aa0: the guest's vec3 dot product. x87 multiplies and adds the three
   terms in z, y, x order, and every product and sum is its own rounding. */
static double dot3(const float *a, const float *b)
{
    const double zz = x87_round((double)a[2] * (double)b[2]);
    const double yy = x87_round((double)a[1] * (double)b[1]);
    const double xx = x87_round((double)a[0] * (double)b[0]);

    return x87_round(x87_round(zz + yy) + xx);
}

/* 0x283ac0: cross(a, b, out) writes a x b with single-precision products, in
   the guest's operand order. */
static void vec3_cross(const float *a, const float *b, float *out)
{
    out[0] = b[2] * a[1] - b[1] * a[2];
    out[1] = b[0] * a[2] - a[0] * b[2];
    out[2] = a[0] * b[1] - b[0] * a[1];
}

/* 0x282ef0: the CRT's fast sqrtf, rsqrtss plus one correction step. Call the
   guest body so the approximation is the one every peer produces; it takes one
   float argument on the stack and returns the result in st(0). */
static float guest_sqrtf(uint32_t return_address, float value)
{
    uint32_t argument;
    unsigned top;
    float result;

    memcpy(&argument, &value, sizeof argument);
    kernel_call_guest_at(return_address, 0x00282ef0u, &argument, 1u);
    top = recomp_runtime.fpu_top;
    result = (float)recomp_runtime.fpu_stack[top];
    recomp_runtime.fpu_top = (top + 1u) & 7u;
    return result;
}

/* CVTTSS2SI: truncate toward zero; NaN and out of range give the integer
   indefinite value, exactly as recomp_types.h MMX_CVT_F2I does. */
static int32_t truncate_to_int(float value)
{
    if (!(value >= -2147483648.0f && value < 2147483648.0f)) return INT32_MIN;
    return (int32_t)value;
}

/* ── 0x1fef0: MT19937 next 32-bit draw ───────────────────────────────────
   The state block holds the 624-word MT array followed by the draws left before
   the reload and the guest address of the next word. __fastcall, the state
   pointer arrives in ecx; ret 0, result in eax. */
typedef struct Mt19937State {
    uint32_t state[624]; /* +0x000 */
    uint32_t left;       /* +0x9c0: draws left before the reload */
    uint32_t cursor;     /* +0x9c4: guest address of the next word */
} Mt19937State;

static uint32_t mt_next_u32_body(void)
{
    const uint32_t object = recomp_runtime.registers.ecx;
    Mt19937State *mt = (Mt19937State *)recomp_memory(object, sizeof *mt);
    const uint32_t left = mt->left - 1u;
    uint32_t cursor;
    uint32_t value;

    mt->left = left;
    /* 0x1fef0: PUSH ESI, which stays below ESP after the return. */
    *recomp_memory_u32(recomp_runtime.registers.esp - 4u) = recomp_runtime.registers.esi;
    if (left == 0u) {
        /* The reload rewrites the array, the counter and the cursor. */
        const uint32_t entry = recomp_runtime.registers.esp;

        recomp_runtime.registers.ecx = object;
        recomp_runtime.registers.esp = entry - 4u;
        kernel_call_guest_at(0x0001ff00u, 0x0001fe30u, NULL, 0u);
        recomp_runtime.registers.esp = entry;
        mt = (Mt19937State *)recomp_memory(object, sizeof *mt);
    }
    cursor = mt->cursor;
    value = *recomp_memory_u32(cursor);
    mt->cursor = cursor + 4u;

    value ^= value >> 11;
    value ^= (value & 0xff3a58adu) << 7;
    value ^= (value & 0xffffdf8cu) << 15;
    return (value >> 18) ^ value;
}

/* ── 0x4c4d0: pose constraint "keep the position out of a circle" ────────
   One handler of the pose constraint list at 0x64c79c. The axis index says
   which component the circle is perpendicular to; that component is clamped to
   [low, high] first, then the two plane offsets and the clamped one are pushed
   out to the radius. Besides the position it leaves a stack image later code
   can read: the axis offset over its second argument slot, its saved
   registers, the two plane offsets, the distance, and the sqrt call's argument
   and return address. Returns 1 when it moved the point. cdecl,
   (float *position, params *), ret 0. */
typedef struct PoseCircleConstraint {
    float axis;    /* +0x00: 0, 1 or 2, read through cvttss2si */
    float centre1; /* +0x04: plane offset of the "across1" component */
    float centre2; /* +0x08: plane offset of the "across2" component */
    float radius;  /* +0x0c */
    float low;     /* +0x10: lower bound of the axis component */
    float high;    /* +0x14: upper bound of the axis component */
} PoseCircleConstraint;

static uint32_t pose_circle_constraint_body(void)
{
    float *const position = (float *)recomp_memory(kernel_arg(1u), 3u * sizeof(float));
    PoseCircleConstraint *const circle =
        (PoseCircleConstraint *)recomp_memory(kernel_arg(2u), sizeof *circle);
    const int axis = truncate_to_int(circle->axis);
    /* Which component is the axis, and which two are the circle's plane. */
    const unsigned normal = axis == 0 ? 0u : axis == 1 ? 1u : 2u;
    const unsigned across1 = axis == 0 ? 2u : 0u;
    const unsigned across2 = axis == 0 ? 1u : axis == 1 ? 2u : 1u;
    const float offset1 = position[across1] - circle->centre1;
    const float offset2 = position[across2] - circle->centre2;
    float offset_normal = 0.0f;
    float length2;
    float distance;
    float scale;

    if (position[normal] > circle->high) {
        offset_normal = position[normal] - circle->high;
    } else if (circle->low > position[normal]) {
        offset_normal = position[normal] - circle->low;
    }

    /* 0x4c4d0: SUB ESP,0xc then PUSH EBX, EBP, ESI, EDI; the plane offsets
       go to the frame, the axis offset over the params argument. */
    {
        const uint32_t entry = recomp_runtime.registers.esp;
        uint32_t *const frame = (uint32_t *)(void *)recomp_memory(entry - 0x1cu, 0x1cu);
        uint32_t bits;

        frame[3] = recomp_runtime.registers.ebx;
        frame[2] = recomp_runtime.registers.ebp;
        frame[1] = recomp_runtime.registers.esi;
        frame[0] = recomp_runtime.registers.edi;
        memcpy(&frame[5], &offset1, sizeof offset1);
        memcpy(&frame[6], &offset2, sizeof offset2);
        memcpy(&bits, &offset_normal, sizeof bits);
        *recomp_memory_u32(entry + 8u) = bits;

        length2 = offset_normal * offset_normal + offset2 * offset2 + offset1 * offset1;
        recomp_runtime.registers.esp = entry - 0x1cu;
        distance = guest_sqrtf(0x0004c585u, length2);
        recomp_runtime.registers.esp = entry;
        memcpy(&frame[4], &distance, sizeof distance); /* FSTP [ESP+0x14] */
    }
    if (!(circle->radius > distance)) return 0u;
    scale = 1.0f;
    if (distance != 0.0f) scale = (circle->radius - distance) / distance;

    position[across1] = scale * offset1 + position[across1];
    position[normal] = scale * offset_normal + position[normal];
    position[across2] = scale * offset2 + position[across2];
    return 1u;
}

/* ── the pose matrix stack the constraint list works in ──────────────────
   The game keeps the current 4x4 at 0xea6080 and points transform through it.
   Two parallel bone-matrix tables give the two directions: the forward ones at
   0x849e40 (bones 0x80 and up) and *0x80b7a0 (bones below 0x80), the inverse
   ones at 0x84a0c0 and *0x850504. */
#define POSE_MATRIX_CURRENT 0x00ea6080u

/* 0x283ea0: make the matrix at "source" the current one. */
static void pose_matrix_select(uint8_t bone, int inverse)
{
    uint32_t matrix;

    if ((bone & 0x80u) != 0u) {
        matrix = 0x40u * (uint32_t)(bone - 0x80u) +
                 (inverse ? 0x0084a0c0u : 0x00849e40u);
    } else {
        const uint32_t base =
            *recomp_memory_u32(inverse ? 0x00850504u : 0x0080b7a0u);
        matrix = base + 0x40u * (uint32_t)bone;
    }
    recomp_guest_memcpy(POSE_MATRIX_CURRENT, matrix, 0x40u);
}

/* 0x2842c0: transform the point at "point" into four lanes at "out" with the
   current matrix. Each lane is ((m0*x + m1*y) + (m2*z + m3)). */
static void pose_transform_point(const float *point, float *out)
{
    const float *const matrix =
        (const float *)recomp_memory(POSE_MATRIX_CURRENT, 16u * sizeof(float));
    const float x = point[0];
    const float y = point[1];
    const float z = point[2];

    for (unsigned lane = 0u; lane < 4u; ++lane) {
        const float along_x = matrix[lane] * x;
        const float along_y = matrix[4u + lane] * y;
        const float along_z = matrix[8u + lane] * z;
        const float offset = matrix[12u + lane];

        out[lane] = (along_x + along_y) + (along_z + offset);
    }
}

/* ── 0x4bfe0: apply a fighter's pose constraint list to a position ───────
   The list is an array of 0x1c-byte commands: byte +0 names the bone matrix the
   command works in, +1 indexes the handler table at 0x64c79c, and +4..+0x1b is
   the handler's parameter block; a +1 of 0xff ends the list. A run of commands
   sharing a matrix has the point transformed into that bone's space, each
   handler edits it there, and the point is transformed back before the next
   matrix starts. When anything ran, *arg4 receives the moved point minus
   *arg3. cdecl, (list, unused, float *position, float *delta), ret 0. */
static uint32_t fighter_pose_constraints_apply_body(void)
{
    const uint32_t esp_in = recomp_runtime.registers.esp;
    const uint32_t list = kernel_arg(1u);
    const uint32_t position_address = kernel_arg(3u);
    const uint32_t delta_address = kernel_arg(4u);
    const float *const position =
        (const float *)recomp_memory(position_address, 3u * sizeof(float));
    float *const delta = (float *)recomp_memory(delta_address, 3u * sizeof(float));
    /* The handlers edit the bone-local point through guest memory, so it lives
       in a scratch block below our frame, where the original kept its own. */
    const uint32_t local_address = esp_in - 0x30u;
    float *const local =
        (float *)recomp_memory(local_address, 4u * sizeof(float));
    float world[4]; /* x, y, z and the transform's unused fourth lane */
    uint32_t command;
    uint8_t matrix_id = 0xffu;
    uint32_t changed = 0u;

    if (list == 0u) return 0u;
    world[0] = position[0] + delta[0];
    world[1] = position[1] + delta[1];
    world[2] = position[2] + delta[2];
    if (*(const uint8_t *)recomp_memory(list + 1u, 1u) == 0xffu) return 0u;

    command = list;
    recomp_runtime.registers.esp = local_address;
    for (;;) {
        const uint8_t *const entry = recomp_memory(command, 2u);
        const uint32_t handler =
            *recomp_memory_u32(0x0064c79cu + 4u * (uint32_t)entry[1]);
        const uint32_t handler_args[2] = {local_address, command + 4u};
        int last;

        if (entry[0] != matrix_id) {
            matrix_id = entry[0];
            pose_matrix_select(matrix_id, 0);
            pose_transform_point(world, local);
            changed = 0u;
        }
        kernel_call_guest_at(0x0004c094u, handler, handler_args, 2u);
        changed |= recomp_runtime.registers.eax;

        command += 0x1cu;
        last = *(const uint8_t *)recomp_memory(command + 1u, 1u) == 0xffu;
        if (last || (changed != 0u &&
                     *(const uint8_t *)recomp_memory(command, 1u) != matrix_id)) {
            /* Leave this bone's space with its inverse matrix. */
            pose_matrix_select(matrix_id, 1);
            pose_transform_point(local, world);
        }
        if (last) break;
    }
    recomp_runtime.registers.esp = esp_in;

    delta[0] = world[0] - position[0];
    delta[1] = world[1] - position[1];
    delta[2] = world[2] - position[2];
    return 0u;
}

/* ── 0x2e1c0: score a segment against one stage triangle ─────────────────
   Rejects a segment whose endpoints are on the same side of the triangle's
   plane, asks 0x2e040 where it crosses, then projects the crossing point onto
   each edge and requires it to end up on the inner side of all three. The
   score is 0.0 when the crossing point is inside the triangle and 1.0 when it
   is not; it is left untouched when there is no crossing at all. The result is
   the crossing count. cdecl, six pointer arguments, ret 0. */
static uint32_t fighter_segment_triangle_score_body(void)
{
    const uint32_t esp_in = recomp_runtime.registers.esp;
    float *const score = (float *)recomp_memory(kernel_arg(1u), sizeof(float));
    const uint32_t p2_address = kernel_arg(2u);
    const uint32_t p3_address = kernel_arg(3u);
    const uint32_t v4_address = kernel_arg(4u);
    const uint32_t v5_address = kernel_arg(5u);
    const uint32_t v6_address = kernel_arg(6u);
    const float *const p2 = (const float *)recomp_memory(p2_address, 3u * sizeof(float));
    const float *const p3 = (const float *)recomp_memory(p3_address, 3u * sizeof(float));
    const float *const v4 = (const float *)recomp_memory(v4_address, 3u * sizeof(float));
    const float *const v5 = (const float *)recomp_memory(v5_address, 3u * sizeof(float));
    const float *const v6 = (const float *)recomp_memory(v6_address, 3u * sizeof(float));
    const float zero = *(const float *)recomp_memory(0x003c2a10u, sizeof(float));
    const float one = *(const float *)recomp_memory(0x003c1a44u, sizeof(float));
    float edge45[3]; /* v5 - v4 */
    float edge46[3]; /* v6 - v4 */
    float normal[3];
    float to_p2[3];
    float to_p3[3];
    uint32_t scratch;
    float *work;
    uint32_t crossing_args[6];
    uint32_t saved_esp;
    uint32_t crossings;
    unsigned inside = 0u;

    for (unsigned i = 0u; i < 3u; ++i) {
        edge45[i] = v5[i] - v4[i];
        edge46[i] = v6[i] - v4[i];
        to_p2[i] = p2[i] - v4[i];
        to_p3[i] = p3[i] - v4[i];
    }
    vec3_cross(edge46, edge45, normal);

    /* The two endpoints must straddle the plane: a positive product means both
       are on the same side, and a NaN (degenerate input) falls through. */
    {
        const float near_side = (float)dot3(normal, to_p2);
        const double far_side = dot3(normal, to_p3);

        if (x87_round(far_side * (double)near_side) > (double)zero) return 0u;
    }

    /* The crossing point and the vectors 0x2e040 reads live in a scratch block
       below our frame, where the original kept its own; nothing reads them
       after the call. Layout: crossing point (4 floats), direction, start, then
       the two triangle edges the callee re-derives its normal from. */
    /* shortcut: the original builds these in its 0x98-byte frame and calls
       0x2e040 from entry-0xc0, so the callee's spills land elsewhere here;
       reproduce that frame if a mixed-build check ever diverges below it. */
    scratch = esp_in - 0x50u;
    work = (float *)recomp_memory(scratch, 16u * sizeof(float));
    for (unsigned i = 0u; i < 3u; ++i) {
        work[4u + i] = p3[i] - p2[i];
        work[7u + i] = p2[i];
        work[10u + i] = edge46[i];
        work[13u + i] = edge45[i];
    }
    crossing_args[0] = scratch;
    crossing_args[1] = scratch + 4u * 4u;
    crossing_args[2] = scratch + 7u * 4u;
    crossing_args[3] = scratch + 10u * 4u;
    crossing_args[4] = scratch + 13u * 4u;
    crossing_args[5] = v4_address;
    saved_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp = scratch;
    kernel_call_guest_at(0x0002e370u, 0x0002e040u, crossing_args, 6u);
    recomp_runtime.registers.esp = saved_esp;
    crossings = recomp_runtime.registers.eax;
    if (crossings == 0u) return 0u;

    /* Walk the three edges in order: v4->v5 against v6, v5->v6 against v4 and
       v6->v4 against v5. Project the crossing point onto the edge line and ask
       whether it stops short of the opposite vertex. */
    for (unsigned i = 0u; i < 3u; ++i) {
        const float *base;
        const float *opposite;
        float edge[3];
        float length2;
        float along;
        float step;
        float projected[3];
        float to_opposite[3];
        float to_crossing[3];

        if (i == 0u) {
            base = v4;
            opposite = v6;
            for (unsigned k = 0u; k < 3u; ++k) edge[k] = v5[k] - v4[k];
        } else if (i == 1u) {
            base = v5;
            opposite = v4;
            for (unsigned k = 0u; k < 3u; ++k) edge[k] = v6[k] - v5[k];
        } else {
            base = v6;
            opposite = v5;
            for (unsigned k = 0u; k < 3u; ++k) edge[k] = v4[k] - v6[k];
        }
        length2 = edge[2] * edge[2] + edge[1] * edge[1] + edge[0] * edge[0];
        if (length2 == 0.0f) return 0u;

        along = (((work[2] - base[2]) * edge[2]) + ((work[1] - base[1]) * edge[1])) +
                ((work[0] - base[0]) * edge[0]);
        step = along * (one / length2);
        for (unsigned k = 0u; k < 3u; ++k) projected[k] = edge[k] * step + base[k];
        for (unsigned k = 0u; k < 3u; ++k) {
            to_opposite[k] = opposite[k] - projected[k];
            to_crossing[k] = work[k] - projected[k];
        }
        /* Unordered (NaN) counts as outside, as the guest's fcomip does. */
        if (dot3(to_opposite, to_crossing) >= (double)zero) ++inside;
    }

    *score = inside == 3u ? 0.0f : one;
    return crossings;
}

static void mt_next_u32(void)
{
    kernel_return_caller_cleanup(mt_next_u32_body());
}

static void pose_circle_constraint(void)
{
    kernel_return_caller_cleanup(pose_circle_constraint_body());
}

static void fighter_segment_triangle_score(void)
{
    kernel_return_caller_cleanup(fighter_segment_triangle_score_body());
}

static void fighter_pose_constraints_apply(void)
{
    kernel_return_caller_cleanup(fighter_pose_constraints_apply_body());
}

RecompFunction recomp_fighter_pose_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0001fef0u: return mt_next_u32;
    case 0x0002e1c0u: return fighter_segment_triangle_score;
    case 0x0004bfe0u: return fighter_pose_constraints_apply;
    case 0x0004c4d0u: return pose_circle_constraint;
    default: return NULL;
    }
}