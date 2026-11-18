/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled fighter strand skinning and vertex-chain work,
   0x4fd00-0x591a0. Each replacement keeps the lifted function's guest RAM
   writes, register results and float rounding, so rollback peers and SyncTest
   see the same state.

   Two passes are bound here. 0x4fd00 (fighter_vertex_chain_constrain) builds
   a mesh part's per-bone rest and current positions and writes three sets of
   constrained vertices through two locked ranges of its vertex buffer. 0x50510
   (fighter_strand_skin_chain) skins one strand: it builds a per-segment 4x4
   basis into g_matrix_current and writes the skinned vertices of each segment.

   Both are compiler output over overlapping stack temporaries, and a caller or
   a later pass reads the leftover stack, so the replacements rebuild the same
   frame at the same guest addresses instead of using host locals: the guest
   stack pointer is put at the depth the original's pushes reach before each
   call, and its callees spill where they always did.

   0x591a0 (fighter_strand_update) walks one fighter's 0x68-byte strand chains,
   its hair and cloth, from the per-fighter record at 0x80b7b8 + slot * 0xf994
   (count byte at record + 2, set up by fighter_render_task 0x5a740). Per
   chain it:

     - skips it when its segment count (node+0x00) is 0, or when it indexes
       bone matrix 0x80 while g_fighter_alt_matrix_draw (0x8503e4) is clear;
     - publishes the bone matrix (g_bone_matrices_low_inverse slot +0x80 or
       g_bone_matrices_direct_inverse for indices from 0x80 up) through
       0x283ea0 into g_matrix_current (0xea6080);
     - when the chain has a root vertex (node+0x44), locks the fighter's
       vertex buffer (0x346190) at the 0x80 bias and copies the root position
       into node+0x14..0x1c;
     - transforms that root into the world position it keeps in its frame at
       +0x4c (0x2842c0), asks 0x26bb80 for this frame's world offset (a
       64-frame position history ring), scales it by node+0x3c into
       g_fighter_vertex_offset (0x8504f4..fc), rotates the chain axis with
       0x284370 into frame+0x5c and looks the floor height up with 0x146920,
       adding the guest's own 0.003 bias;
     - walks the 0x40-byte segments at node+0x48: each one is pulled towards
       its rest position plus the world offset, damped by node+0x10, minus
       last frame's move, gravity (node+0x08) takes 0.5 * it off the vertical
       step, node+0x0c bends it towards the rest pose, and the axis term
       (node+0x34 shrink per segment, node+0x38 softness) steers it; the step
       is capped at node+0x40, added to the segment direction, scaled to
       node+0x04 by 0x283b80, then 0x4bfe0 walks the attachment chain at
       node+0x4c and 0x4bde0 keeps the segment off the floor;
     - finally draws or skins the chain through 0x50510 when its leading id
       (at node+0x50) is -1 or -2, else through 0x50100, and repeats that for
       the alternate id set at node+0x64 with node+0x50..0x60 swapped in.

   0x591a0 is left lifted, as is 0x51f20 (fighter_cloth_strand_update, 5763
   bytes): each needs its own full pass over a much larger body than the two
   above. 0x591a0 has one confirmed blocker: at 0x594fd the original reads the
   chain axis with `movups xmm1,[eax]`, and EAX is only ever loaded with
   frame+0x5c once, before the segment loop. Every callee clobbers it, so from
   the second segment on that read uses whatever the last call left (the
   lifted body therefore reads recomp_runtime.registers.eax). A replacement has
   to reproduce that register read bit for bit; reading the frame slot instead
   matches on the first segment and diverges on every later one for chains
   whose softness (node+0x38) is non-zero. */
#include "game_decomp.h"
#include "kernel_abi.h"

#include <string.h>

/* Guest addresses this group works with. */
enum {
    /* g_matrix_current: the 4x4 float matrix the 0x282e/0x284xx helpers
       multiply by. 0x283ea0 copies a bone matrix into it. */
    GUEST_MATRIX_CURRENT = 0x00ea6080u,
    /* Bone matrices, 0x40 bytes each. The segment axes are rotated by the
       forward pair (g_bone_matrices_low and, from index 0x80 up, the shared
       g_bone_matrices_direct); the root publish uses the inverse pair. */
    GUEST_FIGHTER_MATRICES = 0x00850504u,
    GUEST_FIGHTER_AXIS_MATRICES = 0x0080b7a0u,
    GUEST_SHARED_AXIS_MATRICES = 0x00849e40u,
    GUEST_SHARED_MATRICES = 0x0084a0c0u,
    /* Pointer to the active fighter's D3D object table; a vertex buffer
       handle sits at object+0x24. */
    GUEST_FIGHTER_OBJECTS = 0x0080b7a4u,
    /* Stage ground-height query table. */
    GUEST_GROUND_TABLE = 0x00e0d690u,
    /* Three floats added to every simulated strand vertex this frame. */
    GUEST_VERTEX_OFFSET = 0x008504f4u,
    /* g_fighter_alt_matrix_draw. */
    GUEST_ALT_MATRIX_DRAW = 0x008503e4u,
    /* g_game_tasks_suspended: non-zero skips the strand simulation. */
    GUEST_TASKS_SUSPENDED = 0x008783c9u,
    /* g_fighter_render_options, set by fighter_render_task 0x5a740. Bit 15
       set makes the strand skinning passes return immediately. */
    GUEST_FIGHTER_RENDER_OPTIONS = 0x008504ecu,
    /* Pointer to the per-fighter skin matrix stack, reset per mesh part. */
    GUEST_SKIN_MATRIX_STACK = 0x00850350u,
    /* Where that pointer is set to: the matrix stack's base. */
    GUEST_SKIN_MATRIX_BASE = 0x00850348u,
    /* Per-bone-slot current positions, 16 bytes per slot. */
    GUEST_SKIN_POSITIONS = 0x0084a348u,
    /* Per-bone-slot rest positions, 16 bytes per slot. */
    GUEST_SKIN_REST = 0x0084d348u
};

/* Guest float constants these routines read from the folded .rdata pool: the
   1.0 in the last matrix lane at 0x3c1a44 and the 999.0 that ends a strand's
   position list at 0x3c2e54. They are constant data, so the values below are
   a cache of them. */
#define GUEST_F32_ONE 1.0f
#define GUEST_F32_STRAND_END 999.0f

/* One 0x68-byte strand of a fighter's render list. Only the fields these
   routines touch are named; a slot neither reads keeps its offset. */
typedef struct FighterStrand {
    uint8_t segment_count;    /* +0x00 segments in the chain, 0 skips the strand */
    uint8_t matrix_index;     /* +0x01 bone matrix; 0x80 selects the shared table */
    uint8_t kind;             /* +0x02 how the drawing pass poses the segments */
    uint8_t reserved_03;      /* +0x03 */
    float segment_length;     /* +0x04 length each segment is scaled to */
    float gravity;            /* +0x08 taken off the vertical displacement */
    float bend;               /* +0x0c how far the rest pose bends the chain */
    float chain_scale;        /* +0x10 scale of the running displacement */
    float root[3];            /* +0x14 root position, also a matrix translation */
    float reserved_20;        /* +0x20 */
    float axis[3];            /* +0x24 rest direction of the chain */
    float reserved_30;        /* +0x30 */
    float axis_scale;         /* +0x34 shrink applied to the axis per segment */
    float tip_softness;       /* +0x38 pull of a segment towards the axis */
    float offset_scale;       /* +0x3c scale of this frame's world offset */
    float max_segment;        /* +0x40 longest a segment may get */
    uint32_t root_vertex;     /* +0x44 byte offset of the root vertex, 0 = none */
    uint32_t segments;        /* +0x48 0x40-byte segment records */
    uint32_t constraints;     /* +0x4c attachment chain 0x4bfe0 walks */
    uint32_t id_list;         /* +0x50 {int16 root id; int16 object slot; ...} */
    uint32_t positions;       /* +0x54 one vec4 per segment, 999.0 ends it */
    uint32_t indices;         /* +0x58 vertex indices the skinning pass walks */
    uint32_t reserved_5c;     /* +0x5c */
    uint32_t chain;           /* +0x60 chain descriptor for 0x4fd00, 0 = none */
    uint32_t alternate_ids;   /* +0x64 second id set, drawn right after the first */
} FighterStrand;

/* One 0x40-byte segment: the rest position it is pulled towards, the direction
   the drawing pass reads, and the displacement and previous position the
   simulation carries between frames. */
typedef struct FighterStrandSegment {
    float source[4];      /* +0x00 rest position the segment is pulled from */
    float direction[4];   /* +0x10 direction the drawing pass reads */
    float offset[3];      /* +0x20 displacement added to the running position */
    float reserved_2c;    /* +0x2c */
    float previous[3];    /* +0x30 position the segment had last frame */
    float reserved_3c;    /* +0x3c */
} FighterStrandSegment;

/* The chain descriptor 0x4fd00 walks: a bone-slot count, then one pointer per
   pass. Each pointer holds the pass's records: the first is the slot's vertex
   index, and the rest are runs of vertex indices for the segment. */
typedef struct FighterMeshChain {
    uint16_t slot_count;   /* +0x00 bone slots; 16 bytes each in both tables */
    uint16_t reserved_02;  /* +0x02 */
    uint32_t index_list;   /* +0x04 u32 vertex indices of the position pass */
    uint32_t rest_list;    /* +0x08 {int16 slot; float xyz;} 0x14 each, -1 ends */
    uint32_t run_list;     /* +0x0c {(u32 count, u32 slot, u32 a, u32 b,
                                     u32 items[count]) ...} 0 ends */
    uint32_t group_list;   /* +0x10 {u32 count; u32 indices[count];}... */
    uint32_t normal_list;  /* +0x14 {u32 count; u32 indices[count];}... */
} FighterMeshChain;

static uint32_t guest_u32(uint32_t address)
{
    return *recomp_memory_u32(address);
}

static uint16_t guest_u16(uint32_t address)
{
    return *(const uint16_t *)(const void *)recomp_memory(
        address, sizeof(uint16_t));
}

static uint8_t guest_u8(uint32_t address)
{
    return *recomp_memory(address, sizeof(uint8_t));
}

static float guest_f32(uint32_t address)
{
    return *(const float *)(const void *)recomp_memory(address, sizeof(float));
}

/* A slot of a rebuilt guest frame. Every access has the width the original
   used, so whatever reads the leftover frame sees the same bytes. */
static uint32_t frame_u32(const uint8_t *frame, uint32_t offset)
{
    uint32_t value;

    memcpy(&value, frame + offset, sizeof value);
    return value;
}

static void frame_set_u32(uint8_t *frame, uint32_t offset, uint32_t value)
{
    memcpy(frame + offset, &value, sizeof value);
}

static void frame_set_f32(uint8_t *frame, uint32_t offset, float value)
{
    memcpy(frame + offset, &value, sizeof value);
}

static float frame_f32(const uint8_t *frame, uint32_t offset)
{
    float value;

    memcpy(&value, frame + offset, sizeof value);
    return value;
}

static void store_vec3(uint32_t address, const void *source)
{
    memcpy(recomp_memory(address, 3u * sizeof(float)), source,
        3u * sizeof(float));
}

/* The original moves whole 16-byte vectors, and at an address it computes from
   a list element, so the destination is not always inside its own frame. */
static void store_vec4(uint32_t address, const float value[4])
{
    memcpy(recomp_memory(address, 4u * sizeof(float)), value,
        4u * sizeof(float));
}

static void load_vec4(uint32_t address, float value[4])
{
    memcpy(value, recomp_memory(address, 4u * sizeof(float)),
        4u * sizeof(float));
}

/* Calls a guest routine with the guest stack pointer at the depth the
   original's pushes put it, then restores the caller's pointer. The callee
   spills below that pointer and game code reads such leftover bytes, so the
   depth, and the original CALL's return address it pushes, are part of the
   behaviour. */
static uint32_t call_guest_from(uint32_t return_address, uint32_t stack,
    uint32_t address, uint32_t ecx, uint32_t edx, const uint32_t *arguments,
    unsigned count)
{
    const uint32_t saved = recomp_runtime.registers.esp;
    uint32_t result;

    recomp_runtime.registers.esp = stack;
    recomp_runtime.registers.ecx = ecx;
    recomp_runtime.registers.edx = edx;
    kernel_call_guest_at(return_address, address, arguments, count);
    result = recomp_runtime.registers.eax;
    recomp_runtime.registers.esp = saved;
    return result;
}

/* The bone matrix a strand names for its segment axes: its index selects the
   palette 0x80b7a0 points at, or the table shared from 0x80 up. */
static uint32_t strand_axis_matrix(uint32_t strand)
{
    const uint32_t index = guest_u8(strand + 1u);

    if (index < 0x80u) {
        return guest_u32(GUEST_FIGHTER_AXIS_MATRICES) + (index << 6);
    }
    return ((index - 0x80u) << 6) + GUEST_SHARED_AXIS_MATRICES;
}

/* The bone matrix a strand names for its root: its index selects either the
   fighter's own inverse palette or, from 0x80 up, the shared inverse table. */
static uint32_t strand_bone_matrix(uint32_t strand)
{
    const uint32_t index = guest_u8(strand + 1u);

    if (index < 0x80u) {
        return guest_u32(GUEST_FIGHTER_MATRICES) + (index << 6);
    }
    return ((index - 0x80u) << 6) + GUEST_SHARED_MATRICES;
}

/* The D3D object a strand's id list names: 0x80b7a4 points at the fighter's
   object table, and each object's vertex buffer handle sits at +0x24. */
static uint32_t strand_object(uint32_t slot)
{
    return guest_u32(guest_u32(GUEST_FIGHTER_OBJECTS) + (slot << 2));
}

/* Frame slots of 0x4fd00's 0x54-byte frame. */
enum {
    CHAIN_INDEX_LIST = 0x00,      /* u32 cursor into the position index list */
    CHAIN_RING = 0x04,            /* ring index of the two pointer tables */
    CHAIN_VERTEX_BUFFER = 0x08,   /* u32 handle of the part's vertex buffer */
    CHAIN_REST_TABLE = 0x0c,      /* u32[3] rest-position pointers */
    CHAIN_POSITION_TABLE = 0x18,  /* u32[3] position pointers */
    CHAIN_CROSS = 0x24,           /* vec3 accumulated twist correction */
    CHAIN_DELTA = 0x34            /* vec4[2] segment directions */
};

/* Frame slots of 0x50510's 0x15c-byte frame. */
enum {
    STRAND_OBJECT_SLOT = 0x00,   /* u32 object slot, from the id list */
    STRAND_COUNT = 0x04,         /* s32 segments left to skin */
    STRAND_POSITION = 0x08,      /* vec3 running position, matrix row 3 */
    STRAND_DIRECTION = 0x18,     /* vec4 segment direction, matrix row 1 */
    STRAND_AXES = 0x28,          /* u32 cursor into the rotated axis array */
    STRAND_SIDE = 0x2c,          /* vec4 segment side vector, matrix row 2 */
    STRAND_ROW0 = 0x3c,          /* vec4 matrix row 0 */
    STRAND_POINT = 0x4c,         /* vec4 skinned point of one segment */
    STRAND_ROTATED_AXES = 0x5c   /* vec4 per segment, 0x10 bytes each */
};

/* 0x0004fd00: cdecl (object slot address, FighterMeshChain *). Builds the
   part's per-bone rest and current positions, then writes three sets of
   vertices through two locked ranges of its vertex buffer. */
static void fighter_vertex_chain_constrain(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t object_slot = entry + 4u;
    const uint32_t chain = guest_u32(entry + 8u);
    const uint32_t frame_address = ((entry - 4u) & 0xfffffff0u) - 0x54u;
    uint8_t *frame = recomp_memory(frame_address, 0x54u);
    uint32_t count;
    uint32_t vertex_buffer;

    /* The original's prologue saves, at the addresses it leaves them. */
    *recomp_memory_u32(entry - 4u) = recomp_runtime.registers.ebp;
    *recomp_memory_u32(frame_address - 4u) = recomp_runtime.registers.ebx;
    *recomp_memory_u32(frame_address - 8u) = recomp_runtime.registers.esi;
    *recomp_memory_u32(frame_address - 0x0cu) = recomp_runtime.registers.edi;
    *recomp_memory_u32(GUEST_SKIN_MATRIX_STACK) = GUEST_SKIN_MATRIX_BASE;

    /* Bit 15 of the fighter's render options turns the whole pass off; the
       matrix stack pointer above is reset either way. */
    if ((guest_u32(GUEST_FIGHTER_RENDER_OPTIONS) & 0x8000u) != 0u ||
        chain == 0u) {
        kernel_return_caller_cleanup(0u);
        return;
    }
    frame_set_u32(frame, CHAIN_RING, 0u);
    count = guest_u16(chain);
    if (count == 0u) {
        kernel_return_caller_cleanup(0u);
        return;
    }

    /* Clear one 16-byte slot per bone, then read the listed rest positions:
       each record is a slot word and a float triple, 0x14 bytes, ending at
       slot 0xffff. */
    {
        uint32_t rest = guest_u32(chain + 8u);

        memset(recomp_memory(GUEST_SKIN_REST, (size_t)count << 4), 0,
            (size_t)count << 4);
        while (rest != 0u && guest_u16(rest) != 0xffffu) {
            store_vec3(GUEST_SKIN_REST + ((uint32_t)guest_u16(rest) << 4),
                recomp_memory(rest + 4u, 12u));
            rest += 0x14u;
        }
    }

    /* Lock the part's vertex buffer, then read the current position of every
       listed slot back out of it. */
    {
        const uint32_t arguments[2] = {
            guest_u32(guest_u32(object_slot) + 0x24u), 0x80u
        };

        frame_set_u32(frame, CHAIN_VERTEX_BUFFER, arguments[0]);
        frame_set_u32(frame, CHAIN_INDEX_LIST, guest_u32(chain + 4u));
        vertex_buffer = call_guest_from(0x0004fdd2u, frame_address - 0x0cu, 0x00346190u,
            0u, 0u, arguments, 2u);
    }
    /* The original reuses its own argument slot for that pointer. */
    *recomp_memory_u32(object_slot) = vertex_buffer;
    if ((int32_t)count > 0) {
        for (uint32_t slot = 0; slot < count; ++slot) {
            const uint32_t cursor = frame_u32(frame, CHAIN_INDEX_LIST);
            const uint32_t index = guest_u32(cursor);

            if (index != 0u) {
                store_vec3(GUEST_SKIN_POSITIONS + (slot << 4),
                    recomp_memory(vertex_buffer + index, 12u));
            }
            frame_set_u32(frame, CHAIN_INDEX_LIST, cursor + 4u);
        }
    }

    /* Walk the display list. A run starts with its element count. The first
       element of a run moves the ring onto two bone positions and its
       difference is the first segment direction; every further element is
       another segment whose direction is the difference against the ring's
       previous entry. A -1 element repeats the ring's newest entry. */
    {
        uint32_t cursor = guest_u32(chain + 0x0cu);
        uint32_t run = guest_u32(cursor);
        uint32_t toggle = 0u;
        uint32_t ring = 1u;

        cursor += 4u;
        frame_set_u32(frame, CHAIN_INDEX_LIST, run);
        while (run != 0u) {
            float current[4];
            float previous[4];
            uint32_t store_offset;

            if (guest_u32(cursor) == 0xffffffffu) {
                const uint32_t index = frame_u32(frame, CHAIN_RING);
                const uint32_t positions =
                    frame_u32(frame, CHAIN_POSITION_TABLE + (index << 2));

                cursor += 4u;
                ring = (ring + 1u) % 3u;
                load_vec4(positions, current);
                frame_set_u32(frame, CHAIN_REST_TABLE + (ring << 2),
                    frame_u32(frame, CHAIN_REST_TABLE + (index << 2)));
                frame_set_u32(frame, CHAIN_POSITION_TABLE + (ring << 2),
                    positions);
                /* Read back after the copy, as the original does. */
                load_vec4(frame_u32(frame,
                    CHAIN_POSITION_TABLE + (index << 2)), previous);
                store_offset = toggle << 4;
                toggle ^= 1u;
            } else {
                const uint32_t slot = guest_u32(cursor);
                const uint32_t start = guest_u32(cursor + 4u);
                const uint32_t end = guest_u32(cursor + 8u);
                const uint32_t start_address = GUEST_SKIN_POSITIONS +
                    (start << 4);
                const uint32_t end_address = GUEST_SKIN_POSITIONS +
                    (end << 4);

                /* slot, then the two bones this run interpolates between. */
                cursor += 12u;
                load_vec4(start_address, previous);
                frame_set_u32(frame, CHAIN_POSITION_TABLE, start_address);
                frame_set_u32(frame, CHAIN_REST_TABLE,
                    GUEST_SKIN_REST + (start << 4));
                load_vec4(end_address, current);
                frame_set_u32(frame, CHAIN_POSITION_TABLE + 4u, end_address);
                frame_set_u32(frame, CHAIN_REST_TABLE + 4u,
                    GUEST_SKIN_REST + (end << 4));
                store_offset = slot << 4;
                toggle = slot ^ 1u;
                ring = 1u;
            }
            run = frame_u32(frame, CHAIN_INDEX_LIST);
            for (int lane = 0; lane < 4; ++lane) {
                current[lane] = current[lane] - previous[lane];
            }
            store_vec4(frame_address + CHAIN_DELTA + store_offset, current);
            if ((int32_t)run <= 0) {
                goto next_run;
            }

            frame_set_u32(frame, CHAIN_INDEX_LIST, run);
            for (;;) {
                const uint32_t index = guest_u32(cursor);
                const uint32_t positions =
                    GUEST_SKIN_POSITIONS + (index << 4);
                const uint32_t old_ring = ring;
                uint32_t arguments[3];
                uint32_t left;

                cursor += 4u;
                ring = (ring + 1u) % 3u;
                load_vec4(positions, current);
                frame_set_u32(frame, CHAIN_RING, old_ring);
                frame_set_u32(frame, CHAIN_POSITION_TABLE + (ring << 2),
                    positions);
                frame_set_u32(frame, CHAIN_REST_TABLE + (ring << 2),
                    GUEST_SKIN_REST + (index << 4));
                load_vec4(frame_u32(frame,
                    CHAIN_POSITION_TABLE + (old_ring << 2)), previous);
                for (int lane = 0; lane < 4; ++lane) {
                    current[lane] = current[lane] - previous[lane];
                }
                store_vec4(frame_address + CHAIN_DELTA + (toggle << 4),
                    current);
                toggle ^= 1u;

                /* The twist correction is the cross product of the two ring
                   directions, spread over the three probed rest positions. */
                arguments[0] = frame_address + CHAIN_DELTA;
                arguments[1] = frame_address + CHAIN_DELTA + 0x10u;
                arguments[2] = frame_address + CHAIN_CROSS;
                (void)call_guest_from(0x0004ff44u, frame_address - 0x0cu, 0x00283ac0u,
                    0u, 0u, arguments, 3u);
                (void)call_guest_from(0x0004ff50u, frame_address - 0x0cu, 0x00283840u,
                    frame_address + CHAIN_CROSS, 0u, NULL, 0u);
                for (uint32_t slot = 0; slot < 3u; ++slot) {
                    float *target = (float *)(void *)recomp_memory(
                        frame_u32(frame, CHAIN_REST_TABLE + (slot << 2)), 12u);

                    target[0] = target[0] + frame_f32(frame, CHAIN_CROSS);
                    target[1] = target[1] +
                        frame_f32(frame, CHAIN_CROSS + 4u);
                    target[2] = target[2] +
                        frame_f32(frame, CHAIN_CROSS + 8u);
                }
                left = frame_u32(frame, CHAIN_INDEX_LIST) - 1u;
                frame_set_u32(frame, CHAIN_INDEX_LIST, left);
                if (left == 0u) {
                    break;
                }
            }

        next_run:
            run = guest_u32(cursor);
            cursor += 4u;
            frame_set_u32(frame, CHAIN_INDEX_LIST, run);
        }
    }

    /* Second lock: normalise the rest positions in place and write them per
       listed slot, once for every vertex of the segment. */
    {
        const uint32_t arguments[2] = {
            frame_u32(frame, CHAIN_VERTEX_BUFFER), 0x20u
        };

        vertex_buffer = call_guest_from(0x0005000du, frame_address - 0x0cu, 0x00346190u,
            0u, 0u, arguments, 2u);
    }
    *recomp_memory_u32(object_slot) = vertex_buffer;
    if ((int32_t)count > 0) {
        uint32_t groups = guest_u32(chain + 0x10u);
        uint32_t slot_address = GUEST_SKIN_REST;
        uint32_t left = count;

        for (;;) {
            const uint32_t repeats = guest_u32(groups);

            groups += 4u;
            if (repeats != 0u) {
                (void)call_guest_from(0x00050036u, frame_address - 0x0cu, 0x00283840u,
                    slot_address, 0u, NULL, 0u);
                for (int32_t repeat = 0; repeat < (int32_t)repeats; ++repeat) {
                    store_vec3(vertex_buffer + guest_u32(groups),
                        recomp_memory(slot_address, 12u));
                    groups += 4u;
                }
            }
            slot_address += 0x10u;
            left = left - 1u;
            frame_set_u32(frame, CHAIN_RING, left);
            if (left == 0u) {
                break;
            }
        }
        frame_set_u32(frame, CHAIN_RING, 0u);
    }

    /* Last pass: the same rest positions, negated, written per listed slot. */
    {
        uint32_t groups = guest_u32(chain + 0x14u);
        uint32_t left = count;
        uint32_t slot_address = GUEST_SKIN_REST;

        if (groups == 0u || (int32_t)count <= 0) {
            kernel_return_caller_cleanup(0u);
            return;
        }
        frame_set_u32(frame, CHAIN_INDEX_LIST, left);
        while (left != 0u) {
            const uint32_t repeats = guest_u32(groups);
            float *source = (float *)(void *)recomp_memory(slot_address, 12u);

            groups += 4u;
            if (repeats != 0u) {
                source[0] = 0.0f - source[0];
                source[1] = 0.0f - source[1];
                source[2] = 0.0f - source[2];
                for (int32_t repeat = 0; repeat < (int32_t)repeats; ++repeat) {
                    store_vec3(vertex_buffer + guest_u32(groups),
                        recomp_memory(slot_address, 12u));
                    groups += 4u;
                }
            }
            slot_address += 0x10u;
            left = left - 1u;
            frame_set_u32(frame, CHAIN_INDEX_LIST, left);
        }
    }
    kernel_return_caller_cleanup(0u);
}

/* 0x00050510: cdecl (FighterStrand *). Skins one strand node from the bone
   palette: builds a per-segment 4x4 basis into g_matrix_current, then writes
   the skinned vertices of each segment into the locked vertex-buffer range. */
static void fighter_strand_skin_chain(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t strand = guest_u32(entry + 4u);
    const uint32_t frame_address = entry - 0x15cu;
    const uint32_t stack = frame_address - 0x10u;
    uint8_t *frame = recomp_memory(frame_address, 0x15cu);
    uint32_t id_list;
    uint32_t object;
    uint32_t vertex_buffer;
    int32_t segments;

    /* Bit 15 of the fighter's render options returns before the original
       pushes anything. */
    if ((guest_u32(GUEST_FIGHTER_RENDER_OPTIONS) & 0x8000u) != 0u) {
        kernel_return_caller_cleanup(0u);
        return;
    }
    *recomp_memory_u32(frame_address - 0x04u) = recomp_runtime.registers.ebx;
    *recomp_memory_u32(frame_address - 0x08u) = recomp_runtime.registers.esi;

    /* {int16 root id; int16 object slot; ...}: the slot picks the fighter's
       D3D object, whose +0x24 is the vertex buffer of this part. */
    id_list = guest_u32(strand + 0x50u);
    frame_set_u32(frame, STRAND_OBJECT_SLOT, guest_u16(id_list + 2u));

    object = strand_object(frame_u32(frame, STRAND_OBJECT_SLOT));
    vertex_buffer = guest_u32(object + 0x24u);
    segments = guest_u8(strand);

    if (guest_u8(GUEST_TASKS_SUSPENDED) == 0u) {
        /* Publish the strand's bone matrix and rotate every segment's rest
           axis by it, 16 bytes per rotated axis. */
        uint32_t source;
        uint32_t target;

        *recomp_memory_u32(frame_address - 0x0cu) = recomp_runtime.registers.ebp;
        *recomp_memory_u32(frame_address - 0x10u) = recomp_runtime.registers.edi;
        (void)call_guest_from(0x00050575u, stack, 0x00283ea0u, strand_axis_matrix(strand),
            0u, NULL, 0u);
        source = guest_u32(strand + 0x48u) + 0x10u;
        target = frame_address + STRAND_ROTATED_AXES;
        for (int32_t segment = segments; segment > 0; --segment) {
            (void)call_guest_from(0x00050599u, stack, 0x00284370u, source, target, NULL, 0u);
            target += 0x10u;
            source += 0x40u;
        }

        /* The leading id -2 starts the chain at the root position; any other
           id starts it at the origin. */
        if (guest_u16(id_list) == 0xfffeu) {
            frame_set_f32(frame, STRAND_POSITION, guest_f32(strand + 0x14u));
            frame_set_f32(frame, STRAND_POSITION + 4u,
                guest_f32(strand + 0x18u));
            frame_set_f32(frame, STRAND_POSITION + 8u,
                guest_f32(strand + 0x1cu));
        } else {
            frame_set_f32(frame, STRAND_POSITION, 0.0f);
            frame_set_f32(frame, STRAND_POSITION + 4u, 0.0f);
            frame_set_f32(frame, STRAND_POSITION + 8u, 0.0f);
        }

        /* Lock 0x20 bytes of the vertex buffer for the skinned vertices. */
        {
            const uint32_t arguments[2] = { vertex_buffer, 0x20u };

            frame_set_u32(frame, STRAND_AXES,
                frame_address + STRAND_ROTATED_AXES);
            vertex_buffer = call_guest_from(0x00050603u, stack, 0x00346190u, 0u, 0u,
                arguments, 2u);
        }
        frame_set_u32(frame, STRAND_COUNT, (uint32_t)segments);
        if (segments > 0) {
            uint32_t positions = guest_u32(strand + 0x54u);
            uint32_t indices = guest_u32(strand + 0x58u);
            int32_t remaining = segments;

            frame_set_f32(frame, STRAND_ROW0 + 0x0cu, 0.0f);
            while (remaining > 0) {
                const uint32_t axis_address = frame_u32(frame, STRAND_AXES);
                float direction[4];
                uint32_t arguments[2];

                /* The running position takes this segment's rest axis. */
                frame_set_f32(frame, STRAND_POSITION,
                    guest_f32(axis_address) +
                        frame_f32(frame, STRAND_POSITION));
                frame_set_f32(frame, STRAND_POSITION + 4u,
                    guest_f32(axis_address + 4u) +
                        frame_f32(frame, STRAND_POSITION + 4u));
                frame_set_f32(frame, STRAND_POSITION + 8u,
                    guest_f32(axis_address + 8u) +
                        frame_f32(frame, STRAND_POSITION + 8u));

                /* The segment direction is the next rest axis minus this one,
                   except on the last segment, which uses its own axis. */
                direction[0] = guest_f32(axis_address);
                if (remaining == 1) {
                    direction[1] = guest_f32(axis_address + 4u);
                    direction[2] = guest_f32(axis_address + 8u);
                } else {
                    direction[0] = direction[0] +
                        guest_f32(axis_address + 0x10u);
                    direction[1] = guest_f32(axis_address + 4u) +
                        guest_f32(axis_address + 0x14u);
                    direction[2] = guest_f32(axis_address + 8u) +
                        guest_f32(axis_address + 0x18u);
                    frame_set_u32(frame, STRAND_AXES, axis_address + 0x10u);
                }
                direction[3] = 0.0f;
                memcpy(frame + STRAND_DIRECTION, direction,
                    sizeof direction);

                /* Scale it to unit length: 0x283b80 is given the -1.0 the
                   original pushes, so the direction points back along the
                   chain. */
                arguments[0] = frame_address + STRAND_DIRECTION;
                arguments[1] = 0xbf800000u;
                (void)call_guest_from(0x000506b8u, stack, 0x00283b80u, 0u, 0u, arguments,
                    2u);

                /* The side vector of the segment is (0, -z, y) of the
                   direction, normalised; the cross product of the two is the
                   first row, so the three make up a basis. Each lane keeps the
                   original's arithmetic, zero products included, because the
                   sign of a zero result is visible in the matrix. */
                frame_set_f32(frame, STRAND_SIDE,
                    frame_f32(frame, STRAND_DIRECTION + 8u) * 0.0f -
                        frame_f32(frame, STRAND_DIRECTION + 4u) * 0.0f);
                frame_set_f32(frame, STRAND_SIDE + 4u,
                    0.0f - frame_f32(frame, STRAND_DIRECTION + 8u));
                frame_set_f32(frame, STRAND_SIDE + 8u,
                    frame_f32(frame, STRAND_DIRECTION + 4u) -
                        frame_f32(frame, STRAND_DIRECTION) * 0.0f);
                (void)call_guest_from(0x0005070cu, stack, 0x00283840u,
                    frame_address + STRAND_SIDE, 0u, NULL, 0u);
                frame_set_f32(frame, STRAND_SIDE + 0x0cu, 0.0f);
                {
                    const float side_x = frame_f32(frame, STRAND_SIDE);
                    const float side_y = frame_f32(frame, STRAND_SIDE + 4u);
                    const float side_z = frame_f32(frame, STRAND_SIDE + 8u);
                    const float axis_x = frame_f32(frame, STRAND_DIRECTION);
                    const float axis_y = frame_f32(frame,
                        STRAND_DIRECTION + 4u);
                    const float axis_z = frame_f32(frame,
                        STRAND_DIRECTION + 8u);

                    frame_set_f32(frame, STRAND_ROW0,
                        side_z * axis_y - side_y * axis_z);
                    frame_set_f32(frame, STRAND_ROW0 + 4u,
                        side_x * axis_z - side_z * axis_x);
                    frame_set_f32(frame, STRAND_ROW0 + 8u,
                        side_y * axis_x - side_x * axis_y);
                }
                frame_set_f32(frame, STRAND_ROW0 + 0x0cu, 0.0f);

                /* Publish the basis; row 3 is the running position with a
                   constant 1 in the last lane. */
                {
                    float *matrix = (float *)(void *)recomp_memory(
                        GUEST_MATRIX_CURRENT, 16u * sizeof(float));

                    memcpy(matrix, frame + STRAND_ROW0, 16u);
                    memcpy(matrix + 4, frame + STRAND_DIRECTION, 16u);
                    memcpy(matrix + 8, frame + STRAND_SIDE, 16u);
                    for (int lane = 0; lane < 3; ++lane) {
                        matrix[12 + lane] = frame_f32(frame, STRAND_POSITION +
                            (uint32_t)lane * 4u);
                    }
                    matrix[15] = GUEST_F32_ONE;
                }

                /* Skin this segment's vertices until the position list
                   reaches its 999.0 terminator. An index with bit 0 set is
                   written at index & ~1 and the run continues; a -1 index
                   writes nothing. */
                if (guest_f32(positions) != GUEST_F32_STRAND_END) {
                    for (;;) {
                        uint32_t index = guest_u32(indices);

                        indices += 4u;
                        if (index != 0xffffffffu) {
                            (void)call_guest_from(0x0005084au, stack, 0x002842c0u,
                                positions, frame_address + STRAND_POINT,
                                NULL, 0u);
                            if ((index & 1u) != 0u) {
                                do {
                                    store_vec3(vertex_buffer + (index & ~1u),
                                        frame + STRAND_POINT);
                                    index = guest_u32(indices);
                                    indices += 4u;
                                } while ((index & 1u) != 0u);
                            }
                            store_vec3(vertex_buffer + index,
                                frame + STRAND_POINT);
                        }
                        positions += 0x10u;
                        if (guest_f32(positions) == GUEST_F32_STRAND_END) {
                            break;
                        }
                    }
                }
                positions += 0x10u;
                --remaining;
                frame_set_u32(frame, STRAND_COUNT, (uint32_t)remaining);
            }
        }
    }

    /* A secondary chain of the strand, constrained into the same buffer. */
    {
        const uint32_t chain = guest_u32(strand + 0x60u);

        if (chain != 0u) {
            const uint32_t arguments[2] = { object, chain };

            (void)call_guest_from(0x000508d8u, frame_address - 0x08u, 0x0004fd00u, 0u, 0u,
                arguments, 2u);
        }
    }

    /* A leading id of -1 publishes the root matrix, moves it by the root
       position and lets the object update itself. */
    if (guest_u16(guest_u32(strand + 0x50u)) == 0xffffu) {
        (void)call_guest_from(0x0005091bu, frame_address - 0x08u, 0x00283ea0u,
            strand_bone_matrix(strand), 0u, NULL, 0u);
        (void)call_guest_from(0x00050923u, frame_address - 0x08u, 0x00284070u,
            strand + 0x14u, 0u, NULL, 0u);
        (void)call_guest_from(0x0005092au, frame_address - 0x08u, 0x00288aa0u, object, 0u,
            NULL, 0u);
    }
    kernel_return_caller_cleanup(0u);
}

RecompFunction recomp_fighter_mesh_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0004fd00u: return fighter_vertex_chain_constrain;
    case 0x00050510u: return fighter_strand_skin_chain;
    default: return NULL;
    }
}
