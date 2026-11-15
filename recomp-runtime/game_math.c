/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled skeleton math: the node transform 0x636d0. It keeps the
   lifted function's guest RAM writes, register results and float rounding,
   so rollback peers and SyncTest see the same state. Its callers 0x63630 and
   0x63780 stay lifted: 0x283c60's fourth lane is leftover stack
   (docs/bring-up.md), so they only match with the original call chain. */
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

enum {
    /* g_matrix_current: the four float4 rows composed by the matrix multiply at
       0x2bffba. Rows 0-2 are the basis, row 3 the translation that positions
       pick up and directions do not. */
    GUEST_WORLD_MATRIX = 0x00ea6080,
    /* g_node_world_position / g_node_world_direction: the two float4 results of
       0x636d0, one 32-byte scratch block; the node list copies their xyz out. */
    GUEST_NODE_POSITION = 0x00854e30,
    GUEST_NODE_DIRECTION = 0x00854e40,
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

RecompFunction recomp_game_math_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x000636d0u: return node_transform_apply;
    default: return NULL;
    }
}
