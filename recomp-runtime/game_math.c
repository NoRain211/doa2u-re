/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled shared vector and matrix math: the skeleton node transform
   0x63630-0x637ff, and the collision helpers 0x144940 and 0x14a6c0. Each
   replacement keeps the lifted function's guest RAM writes, register results
   and float rounding, so rollback peers and SyncTest see the same state. */
#include "game_decomp.h"
#include "kernel_abi.h"
#include "recomp_types.h"

#include <string.h>

/* recomp_types.h maps the guest register names onto the runtime and points
   memcpy/memset at the guest-memory copies the generated code needs; this file
   reads the registers through their runtime fields and copies host pointers,
   so drop both sets of macros before the first definition. */
#undef memcpy
#undef memset
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

/* Fixed guest globals these helpers share. The game reaches the 0x8xxxxxxx
   ones through the cached alias, which the runtime resolves to the same RAM as
   the physical window; guest_bytes() takes the inlined physical path first, so
   the hot loop pays no region search. */
enum {
    /* g_matrix_current: the four float4 rows composed by the matrix multiply at
       0x2bffba. Rows 0-2 are the basis, row 3 the translation that positions
       pick up and directions do not. */
    GUEST_WORLD_MATRIX = 0x00ea6080,
    /* g_node_world_position / g_node_world_direction: the two float4 results of
       0x636d0, one 32-byte scratch block; the node list copies their xyz out. */
    GUEST_NODE_POSITION = 0x00854e30,
    GUEST_NODE_DIRECTION = 0x00854e40,
    /* g_bone_basis: four 64-byte entries of three float4 rows, filled by
       0x63510 from the draw part's node palette. 0x63630 projects the frame
       source vector onto each and stores the four results as matrix rows. */
    GUEST_BONE_BASIS = 0x00854e50,
    GUEST_BONE_BASIS_ENTRY_BYTES = 0x40,
    /* g_basis_source: 16-byte stride entries, read as xyz. The node list's
       first dword is the entry index and is only honoured below 16. */
    GUEST_BASIS_SOURCE = 0x00654970,
    GUEST_BASIS_SOURCE_COUNT = 16,
    /* The per-side bone vertex tables the contact search measures against:
       { count, float3 vertices, bone records }. Only the vertex array is read
       here. */
    GUEST_BONE_TABLE_SECOND = 0x00e0d6fc,
    GUEST_BONE_TABLE_FIRST = 0x00e0d65c,
    GUEST_BONE_VERTEX_BYTES = 0x0c,
    /* The skew factor the endpoint paths apply to their projection length. */
    GUEST_CONTACT_SKEW = 0x003c2d28,
    /* Routines the replacements call back into. */
    GUEST_KERNEL_BUILD_BONE_BASIS = 0x00063510,
    GUEST_KERNEL_NODE_TRANSFORM = 0x000636d0,
    GUEST_KERNEL_BASIS_PROJECT = 0x00284520,
    GUEST_KERNEL_WORLD_MATRIX_REBUILD = 0x00063630,
    GUEST_KERNEL_NORMALIZE_VECTOR3 = 0x00283840,
    GUEST_KERNEL_ORTHOGONALIZE_VECTOR3 = 0x00283c60,
    GUEST_KERNEL_ANGLE_INDEX = 0x002836c0,
    GUEST_KERNEL_SINE = 0x002835b0,
    GUEST_KERNEL_COSINE = 0x002835c0,
    /* 0x2e1c0: the segment/triangle test 0x14a6c0's sweep runs per triangle. It
       writes a float through its first argument and returns non-zero on a hit. */
    GUEST_KERNEL_SEGMENT_TRIANGLE = 0x0002e1c0,
};

/* Host pointer to a guest RAM span. The generated code reaches RAM through a
   flat mapping; translate the two RAM windows the same way and take the
   runtime's inlined path, falling back to the checked one so write watches,
   access logs and pending device writes still see the access when they are
   on. */
static uint8_t *guest_bytes(uint32_t address, size_t width)
{
    uint32_t physical;
    uint8_t *ram;

    if (width > RECOMP_XBOX_RAM_SIZE) return recomp_memory(address, width);
    if (address < RECOMP_XBOX_RAM_SIZE) {
        physical = address;
    } else if (address >= RECOMP_XBOX_CACHED_ALIAS &&
        address - RECOMP_XBOX_CACHED_ALIAS < RECOMP_XBOX_RAM_SIZE) {
        physical = address - RECOMP_XBOX_CACHED_ALIAS;
    } else {
        return recomp_memory(address, width);
    }
    ram = recomp_memory_ram(physical, width);
    return ram != NULL ? ram : recomp_memory(address, width);
}

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof bits);
    return bits;
}

/* Pushes an x87 result the way the original's fld/fstp leaves it. */
static void push_st0(double value)
{
    const uint32_t top = (recomp_runtime.fpu_top + 7u) & 7u;

    recomp_runtime.fpu_top = top;
    recomp_runtime.fpu_stack[top] = value;
}

/* Shared float4 projection: the world position picks up the translation row
   (0x284260 and 0x636d0's position), the world direction does not. The guest
   computes ((row0*x + row1*y) + (row2*z + row3)), so the grouping is kept. */
static void matrix_project_point(
    const float rows[4][4], float x, float y, float z, float out[4])
{
    unsigned lane;

    for (lane = 0; lane < 4u; ++lane) {
        out[lane] = (rows[0][lane] * x + rows[1][lane] * y) +
            (rows[2][lane] * z + rows[3][lane]);
    }
}

static void matrix_project_direction(
    const float rows[4][4], float x, float y, float z, float out[4])
{
    unsigned lane;

    for (lane = 0; lane < 4u; ++lane) {
        out[lane] = (rows[0][lane] * x + rows[1][lane] * y) +
            rows[2][lane] * z;
    }
}

/* One entry of the node palette 0x63510 returns: the position the draw part
   wants at +0x00 and its direction at +0x10. The two spare dwords (+0x0c and
   +0x1c) are never read by 0x636d0. */
typedef struct NodeRecord {
    float position[3];
    float unused_position;
    float direction[3];
} NodeRecord;

/* Publishes both float4 results in the 0x854e30 scratch block. */
static void store_node_result(
    const float position[4], const float direction[4])
{
    uint8_t *store = guest_bytes(GUEST_NODE_POSITION, 2u * sizeof(float) * 4u);

    memcpy(store, position, sizeof(float) * 4u);
    memcpy(store + sizeof(float) * 4u, direction, sizeof(float) * 4u);
}

/* Writes the six floats the node list hands to one destination, skipping the
   two w lanes of the 0x854e30 scratch block: position xyz then direction xyz. */
static void write_node_result(uint32_t destination, const float scratch[8])
{
    const size_t component = sizeof(float) * 3u;
    uint8_t *out = guest_bytes(destination, 2u * component);

    memcpy(out, scratch, component);
    memcpy(out + component, scratch + 4u, component);
    /* The original copies six movss loads, so xmm0 is left holding the last
       value it stored (a broadcast of direction z) with the upper lanes
       zeroed; the next lifted routine may shuffle that forward. */
    recomp_runtime.xmm[0].f[0] = scratch[6];
    recomp_runtime.xmm[0].f[1] = 0.0f;
    recomp_runtime.xmm[0].f[2] = 0.0f;
    recomp_runtime.xmm[0].f[3] = 0.0f;
}

/* 0x000636d0: transform the node record at eax by the current matrix, leaving
   the world position at 0x854e30 and the world direction at 0x854e40. */
static void node_transform_apply(void)
{
    float rows[4][4];
    float position[4];
    float direction[4];
    NodeRecord node;
    unsigned lane;

    memcpy(rows, guest_bytes(GUEST_WORLD_MATRIX, sizeof rows), sizeof rows);
    memcpy(&node, guest_bytes(recomp_runtime.registers.eax, sizeof node),
        sizeof node);

    matrix_project_point(rows,
        node.position[0], node.position[1], node.position[2], position);
    matrix_project_direction(rows,
        node.direction[0], node.direction[1], node.direction[2], direction);
    store_node_result(position, direction);

    /* The lifted body ends with the packed intermediates still in xmm0..xmm6:
       the direction, the broadcast x, the scaled rows 1 and 2, row 3, the
       translated row 2 and the position. A later routine that shuffles one of
       them forward reads the same bits as the original this way. */
    for (lane = 0; lane < 4u; ++lane) {
        recomp_runtime.xmm[0].f[lane] = direction[lane];
        recomp_runtime.xmm[1].f[lane] = node.direction[0];
        recomp_runtime.xmm[2].f[lane] =
            rows[1][lane] * node.direction[1];
        recomp_runtime.xmm[3].f[lane] =
            rows[2][lane] * node.direction[2];
        recomp_runtime.xmm[4].f[lane] = rows[3][lane];
        recomp_runtime.xmm[5].f[lane] =
            rows[2][lane] * node.position[2] + rows[3][lane];
        recomp_runtime.xmm[6].f[lane] = position[lane];
    }

    /* The lifted body ends with eax holding the direction address; no caller
       reads it, but keep the register as the original left it. */
    recomp_runtime.registers.eax = GUEST_NODE_DIRECTION;
    kernel_return_caller_cleanup(recomp_runtime.registers.eax);
}

/* 0x00283840: normalize the 3-vector at address in place. */
static void normalize_vector3(uint32_t address)
{
    recomp_runtime.registers.ecx = address;
    kernel_call_guest(GUEST_KERNEL_NORMALIZE_VECTOR3, NULL, 0u);
}

/* 0x00283c60: destination = destination - axis * (destination.axis /
   axis.axis), the Gram-Schmidt step 0x63630 runs twice per row. */
static void orthogonalize_vector3(uint32_t destination, uint32_t axis)
{
    const uint32_t arguments[3] = {destination, destination, axis};

    kernel_call_guest(GUEST_KERNEL_ORTHOGONALIZE_VECTOR3, arguments, 3u);
}

/* 0x00284520: out = basis row0*v.x + row1*v.y + row2*v.z, 16 bytes. It is left
   a guest call rather than inlined: 0x283c60 below builds its b*t operand from
   three floats on its own frame and reloads 16 bytes, so the fourth lane it
   subtracts is stack leftover, and only the real call sequence leaves the
   bytes the original had. */
static void basis_entry_project(
    uint32_t out, uint32_t basis, uint32_t source)
{
    const uint32_t arguments[1] = {out};

    recomp_runtime.registers.ecx = basis;
    recomp_runtime.registers.edx = source;
    kernel_call_guest(GUEST_KERNEL_BASIS_PROJECT, arguments, 1u);
}

/* 0x00283840: normalize the 3-vector at address in place. */
static void normalize_vector3(uint32_t address);

/* The matrix rebuild is left a guest call: 0x283c60 builds its b*t operand
   from three floats on its own frame and reloads 16 bytes, so the fourth lane
   it subtracts into each row is stack leftover that only the original call
   sequence reproduces. */
static void guest_world_matrix_rebuild(uint32_t index)
{
    recomp_runtime.registers.ecx = GUEST_WORLD_MATRIX;
    recomp_runtime.registers.eax = index;
    kernel_call_guest(GUEST_KERNEL_WORLD_MATRIX_REBUILD, NULL, 0u);
}

/* 0x00063630: rebuild the current matrix from g_bone_basis and the frame
   source vector. Each of the four basis entries is projected onto the source
   vector (0x284520) to become a row; the fourth row is the translation point
   and stays as it is, while rows 2, 1 and 0 are each orthogonalized against
   the rows above them and normalized. */
static void build_world_matrix(uint32_t index)
{
    const uint32_t source_address = GUEST_BASIS_SOURCE + index * 16u;
    unsigned entry;

    for (entry = 0; entry < 4u; ++entry) {
        basis_entry_project(GUEST_WORLD_MATRIX + entry * 16u,
            GUEST_BONE_BASIS + entry * GUEST_BONE_BASIS_ENTRY_BYTES,
            source_address);
    }

    normalize_vector3(GUEST_WORLD_MATRIX + 0x20u);
    orthogonalize_vector3(GUEST_WORLD_MATRIX + 0x10u, GUEST_WORLD_MATRIX + 0x20u);
    normalize_vector3(GUEST_WORLD_MATRIX + 0x10u);
    orthogonalize_vector3(GUEST_WORLD_MATRIX, GUEST_WORLD_MATRIX + 0x20u);
    orthogonalize_vector3(GUEST_WORLD_MATRIX, GUEST_WORLD_MATRIX + 0x10u);
    /* The original ends with a tail call, so the last normalize is the
       function's result as far as the stack is concerned. */
    normalize_vector3(GUEST_WORLD_MATRIX);
}

/* 0x00063630: same body, taking its index from eax as the guest caller left
   it. The tail call leaves eax to the normalizer; nothing reads it. */
static void world_matrix_rebuild(void)
{
    build_world_matrix(recomp_runtime.registers.eax);
    kernel_return_caller_cleanup(recomp_runtime.registers.eax);
}

/* A dword stream in guest RAM with no stored length: the node list is read in
   windows so one checked translation serves a whole run of elements. */
typedef struct GuestDwordStream {
    uint32_t next_address;
    const uint8_t *window;
    uint32_t remaining;
} GuestDwordStream;

enum { GUEST_STREAM_WINDOW = 0x100u };

static void stream_open(GuestDwordStream *stream, uint32_t address)
{
    stream->next_address = address;
    stream->window = NULL;
    stream->remaining = 0u;
}

static uint32_t stream_next(GuestDwordStream *stream)
{
    uint32_t value;

    if (stream->remaining == 0u) {
        const uint32_t offset =
            stream->next_address & (RECOMP_XBOX_RAM_SIZE - 1u);
        uint32_t span = RECOMP_XBOX_RAM_SIZE - offset;
        if (span > GUEST_STREAM_WINDOW) span = GUEST_STREAM_WINDOW;
        if (span < (uint32_t)sizeof value) {
            /* The tail of RAM cannot hold a full window. */
            value = *recomp_memory_u32(stream->next_address);
            stream->next_address += (uint32_t)sizeof value;
            return value;
        }
        stream->window = guest_bytes(stream->next_address, span);
        stream->remaining = span / (uint32_t)sizeof value;
    }
    memcpy(&value, stream->window, sizeof value);
    stream->window += sizeof value;
    stream->next_address += (uint32_t)sizeof value;
    --stream->remaining;
    return value;
}

/* 0x00063780: skin one draw part. eax points at a 0x20-byte part record whose
   +0x04 field is the node list and whose +0x08 field is the palette 0x63510
   turns into g_bone_basis. The list is a dword stream:

       index, dest, ..., -1, dest, ..., -1, -1, index, ..., 0

   Each index rebuilds the world matrix, each node advances the palette by
   0x20, and every destination up to the next -1 receives the same six floats;
   a second -1 ends the index group and a zero ends the list. */
static void transform_node_list(void)
{
    const uint8_t *part =
        guest_bytes(recomp_runtime.registers.eax, 3u * sizeof(uint32_t));
    uint32_t list_address;
    uint32_t palette_argument;
    uint32_t palette;
    uint32_t index;
    GuestDwordStream stream;
    float scratch[8];

    memcpy(&list_address, part + 4, sizeof list_address);
    memcpy(&palette_argument, part + 8, sizeof palette_argument);
    kernel_call_guest(GUEST_KERNEL_BUILD_BONE_BASIS, &palette_argument, 1u);
    palette = recomp_runtime.registers.eax;

    stream_open(&stream, list_address);
    index = stream_next(&stream);
    while (index != 0u) {
        uint32_t destination;

        if (index < GUEST_BASIS_SOURCE_COUNT) guest_world_matrix_rebuild(index);
        destination = stream_next(&stream);
        for (;;) {
            /* 0x636d0 owns the transform and its two float4 scratch stores,
               which the destinations are then filled from. Calling it keeps
               this walk independent of whether 0x636d0 is itself bound. */
            recomp_runtime.registers.eax = palette;
            palette += 0x20u;
            kernel_call_guest(GUEST_KERNEL_NODE_TRANSFORM, NULL, 0u);
            memcpy(scratch, guest_bytes(GUEST_NODE_POSITION, sizeof scratch),
                sizeof scratch);

            for (;;) {
                write_node_result(destination, scratch);
                destination = stream_next(&stream);
                if (destination == UINT32_MAX) break;
            }
            destination = stream_next(&stream);
            if (destination == UINT32_MAX) break;
        }
        index = stream_next(&stream);
    }
    /* The lifted body leaves eax holding the terminator it tested. */
    kernel_return_caller_cleanup(index);
}

/* The query 0x144940 answers for and 0x145750 fills in per candidate: a point
   in the XZ plane plus the two vertex indices of the segment it is measured
   against. */
typedef struct ContactQuery {
    float x;                        /* +0x00 */
    float z;                        /* +0x04 */
    float unused[2];                /* +0x08 */
    uint32_t segment_indices;       /* +0x10: two u16 vertex indices */
} ContactQuery;

typedef struct GuestVec3 {
    float x;
    float y;
    float z;
} GuestVec3;

static GuestVec3 guest_vec3(uint32_t address)
{
    GuestVec3 vector;

    memcpy(&vector, guest_bytes(address, sizeof vector), sizeof vector);
    return vector;
}

/* 0x00144940: squared distance from the query point to one bone segment. The
   point is measured against the nearer end when it projects outside the
   segment, and against the rotated residual when it projects inside; the
   mode argument picks which of the two fighters' vertex table to read.
   Returns the double on the x87 stack, as the original's fsubp/faddp does. */
static void bone_segment_distance(void)
{
    /* kernel_arg(0) is the return address: the guest's first argument sits just
       past it. */
    const uint32_t mode = kernel_arg(1u);
    const uint32_t query_address = kernel_arg(2u);
    const uint32_t table = mode != 0u ? GUEST_BONE_TABLE_SECOND
                                      : GUEST_BONE_TABLE_FIRST;
    ContactQuery query;
    uint32_t vertices;
    uint16_t indices[2];
    GuestVec3 from;
    GuestVec3 to;
    float at_x;
    float at_z;
    float dx;
    float dz;
    float x;
    float y;
    float s;
    float skew;

    memcpy(&query, guest_bytes(query_address, sizeof query), sizeof query);
    memcpy(&vertices, guest_bytes(table + 4u, sizeof vertices), sizeof vertices);
    memcpy(indices, guest_bytes(query.segment_indices, sizeof indices),
        sizeof indices);
    from = guest_vec3(vertices + indices[0] * GUEST_BONE_VERTEX_BYTES);
    to = guest_vec3(vertices + indices[1] * GUEST_BONE_VERTEX_BYTES);
    memcpy(&skew, guest_bytes(GUEST_CONTACT_SKEW, sizeof skew), sizeof skew);

    at_x = from.x;
    at_z = from.z;
    dx = to.x - at_x;
    dz = to.z - at_z;
    x = query.x - at_x;
    y = query.z - at_z;
    /* COMISS 0, s leaves CF set for both "s above zero" and unordered, so the
       far end is measured for a NaN projection too. */
    s = dz * y + dx * x;
    if (!(0.0f < s) && !(s != s)) {
        const double length = RECOMP_FP_PC(
            RECOMP_FP_PC((double)y * (double)y) +
            RECOMP_FP_PC((double)x * (double)x));
        const double projection = RECOMP_FP_PC((double)s * (double)skew);

        push_st0(RECOMP_FP_PC(length - projection));
        kernel_return_caller_cleanup(recomp_runtime.registers.eax);
        return;
    }

    {
        const float far_x = query.x - to.x;
        const float far_y = query.z - to.z;
        const float t = dz * far_y + dx * far_x;

        /* COMISS t, 0 leaves CF set for "t below zero" and unordered. */
        if (!(t < 0.0f) && !(t != t)) {
            const double length = RECOMP_FP_PC(
                RECOMP_FP_PC((double)far_y * (double)far_y) +
                RECOMP_FP_PC((double)far_x * (double)far_x));
            const double projection = RECOMP_FP_PC((double)t * (double)skew);

            push_st0(RECOMP_FP_PC(length + projection));
            kernel_return_caller_cleanup(recomp_runtime.registers.eax);
            return;
        }

        {
            /* Inside the segment: rotate the residual into the segment's frame
               with the fixed-point angle 0x2836c0 derives from the direction,
               and the 65536-entry sine table at 0xe65f68. */
            const uint32_t angle_arguments[2] = {float_bits(dx), float_bits(dz)};
            uint32_t angle;
            double turned;
            float residual;

            kernel_call_guest(GUEST_KERNEL_ANGLE_INDEX, angle_arguments, 2u);
            angle = recomp_runtime.registers.eax;

            kernel_call_guest(GUEST_KERNEL_COSINE, &angle, 1u);
            /* FMUL then FSTP float: the product is rounded to float on the
               store, and the slot it lands in is read back as the minuend. */
            turned = RECOMP_FP_PC(recomp_runtime.fpu_stack[recomp_runtime.fpu_top] *
                (double)far_x);
            residual = (float)turned;
            recomp_runtime.fpu_top = (recomp_runtime.fpu_top + 1u) & 7u;

            kernel_call_guest(GUEST_KERNEL_SINE, &angle, 1u);
            /* FSUBR: the stored float minus the sine product. */
            turned = RECOMP_FP_PC((double)residual -
                RECOMP_FP_PC(recomp_runtime.fpu_stack[recomp_runtime.fpu_top] *
                    (double)far_y));
            /* FLD ST0 / FMULP: square it, leaving exactly one result. */
            recomp_runtime.fpu_stack[recomp_runtime.fpu_top] =
                RECOMP_FP_PC(turned * turned);
        }
    }
    kernel_return_caller_cleanup(recomp_runtime.registers.eax);
}

/* 0x0014a6c0: one triangle of the 0x14a660 sweep. The three shared vertices
   are transformed by the current matrix into a stack copy (0x284260), the
   segment/triangle test 0x2e1c0 answers through the caller's second argument
   slot, and the smallest answer so far is kept in ctx[2]. Returns 1 once ctx[2]
   is above the limit in ctx[3], which stops the sweep, else 0. */
static void sweep_triangle_callback(void)
{
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    const uint32_t context_address = entry_esp + 4u;
    const uint32_t answer_address = entry_esp + 8u;
    const uint32_t scratch = entry_esp - 0x30u;
    uint32_t context[2];
    uint32_t arguments[6];
    float rows[4][4];
    float vertices[3][4];
    unsigned index;

    memcpy(rows, guest_bytes(GUEST_WORLD_MATRIX, sizeof rows), sizeof rows);
    memcpy(context, guest_bytes(context_address, sizeof context), sizeof context);
    for (index = 0u; index < 3u; ++index) {
        uint32_t source;
        GuestVec3 vertex;

        memcpy(&source, guest_bytes(entry_esp + 8u + 4u * index, sizeof source),
            sizeof source);
        vertex = guest_vec3(source);
        matrix_project_point(rows, vertex.x, vertex.y, vertex.z, vertices[index]);
    }

    /* The original hands 0x2e1c0 its own frame as the transformed vertices; a
       host local cannot stand in for a guest address, so take the same kind of
       three-float4 scratch on the guest stack. */
    recomp_runtime.registers.esp = scratch;
    for (index = 0u; index < 3u; ++index) {
        memcpy(guest_bytes(scratch + 0x10u * index, sizeof vertices[index]),
            vertices[index], sizeof vertices[index]);
    }
    arguments[0] = answer_address;
    arguments[1] = context[0];
    arguments[2] = context[1];
    arguments[3] = scratch;
    arguments[4] = scratch + 0x10u;
    arguments[5] = scratch + 0x20u;
    kernel_call_guest(GUEST_KERNEL_SEGMENT_TRIANGLE, arguments, 6u);
    recomp_runtime.registers.esp = entry_esp;

    if (recomp_runtime.registers.eax != 0u) {
        float answer;
        float closest;

        memcpy(&answer, guest_bytes(answer_address, sizeof answer), sizeof answer);
        memcpy(&closest, guest_bytes(context_address + 8u, sizeof closest),
            sizeof closest);
        /* COMISS keeps the old value when the compare is unordered. */
        if (closest > answer) {
            memcpy(guest_bytes(context_address + 8u, sizeof answer), &answer,
                sizeof answer);
        }
    }

    {
        float closest;
        float limit;

        memcpy(&closest, guest_bytes(context_address + 8u, sizeof closest),
            sizeof closest);
        memcpy(&limit, guest_bytes(context_address + 0x0cu, sizeof limit),
            sizeof limit);
        kernel_return_caller_cleanup(closest > limit ? 1u : 0u);
    }
}

RecompFunction recomp_game_math_lookup_manual(uint32_t address)
{
    (void)world_matrix_rebuild;
    (void)transform_node_list;
    (void)bone_segment_distance;
    (void)sweep_triangle_callback;
    switch (address) {
    case 0x000636d0u: return node_transform_apply;
    default: return NULL;
    }
}
