/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled renderer transform and bounds math, 0x283ea0-0x2847f0. Each
   replacement keeps the lifted function's guest RAM writes, register results
   and float rounding, so rollback peers and SyncTest see the same state. Two
   of them also reproduce the original's discarded stack frame: game code reads
   leftover stack bytes and a rollback comparison covers all of guest memory.

   The renderer keeps one current 4x4 transform in guest RAM (0x00ea6080,
   row-major, row 3 holding the translation) and transforms points as row
   vectors: p' = x*row0 + y*row1 + z*row2 + row3. The helpers here load, move
   and invert that matrix. */
#include "game_decomp.h"
#include "kernel_abi.h"

#include <string.h>

/* Guest 0x00ea6080: the current 4x4 transform. */
#define MATRIX_CURRENT 0x00ea6080u
/* Guest 0x00ea5f70: the working copy matrix_stack_invert permutes in place. */
#define MATRIX_SCRATCH 0x00ea5f70u
/* One past the last cell of the current transform. */
#define MATRIX_END 0x00ea60c0u
/* Addresses the original leaves in EAX; two callers take the low byte of EAX
   from those calls, so keep the register result unchanged. */
#define MATRIX_TRANSLATION_ROW 0x00ea60b0u
#define MATRIX_ROW1 0x00ea6090u

/* The renderer's absolute value: negate unless the input is above zero, which
   is what the original's COMISS/SUBSS pair does. Keeping the shape matters for
   a negative zero and a NaN. */
static float render_abs(float value)
{
    return value > 0.0f ? value : 0.0f - value;
}

/* Loads the 4x4 at guest `address` into a host copy. */
static void load_matrix(uint32_t address, float matrix[16])
{
    memcpy(matrix, recomp_memory(address, 16u * sizeof(float)),
        16u * sizeof(float));
}

/* Guest 0x00283ea0: copies the 4x4 at ECX into the current transform. */
static void matrix_stack_load(void)
{
    float matrix[16];

    load_matrix(recomp_runtime.registers.ecx, matrix);
    memcpy(recomp_memory(MATRIX_CURRENT, sizeof matrix), matrix, sizeof matrix);
    kernel_return_caller_cleanup(MATRIX_TRANSLATION_ROW);
}

/* Guest 0x00283fc0(x, y, z) on the stack: moves the current transform by a
   local-space offset, translation row += x*row0 + y*row1 + z*row2. The
   original also keeps the offset it last broadcast and the running row sum in
   a 0x20-byte frame below a 16-byte-aligned stack pointer, and game code reads
   such leftover stack, so the same bytes are left behind here. */
static void matrix_stack_translate_local(void)
{
    const uint32_t stack_pointer = recomp_runtime.registers.esp;
    float offset[3];
    float moved[4];
    float matrix[16];

    memcpy(offset, recomp_memory(stack_pointer + 4u, sizeof offset),
        sizeof offset);
    load_matrix(MATRIX_CURRENT, matrix);
    for (int lane = 0; lane < 4; ++lane) {
        float value = matrix[lane] * offset[0];

        value = value + matrix[4 + lane] * offset[1];
        moved[lane] = value + matrix[8 + lane] * offset[2];
        matrix[12 + lane] = matrix[12 + lane] + moved[lane];
    }
    memcpy(recomp_memory(MATRIX_CURRENT, sizeof matrix), matrix, sizeof matrix);

    /* Frame layout: the saved frame pointer sits just above the block, the
       broadcast offset at +0x0c and the row sum at +0x10. */
    {
        const uint32_t frame =
            ((stack_pointer - 4u) & 0xfffffff0u) - 0x20u;
        float *slots = (float *)recomp_memory(frame, 0x20u);

        *recomp_memory_u32(stack_pointer - 4u) = recomp_runtime.registers.ebp;
        slots[3] = offset[2];
        for (int lane = 0; lane < 4; ++lane) slots[4 + lane] = moved[lane];
    }
    kernel_return_caller_cleanup(MATRIX_TRANSLATION_ROW);
}

/* Transforms the xyz point read from `point_in` by the 4x4 at `matrix_address`
   and writes all four lanes to `point_out`, in the original's grouping:
   ((row0*x + row1*y) + (row2*z + row3)). Every input is read before the
   result is stored, so the point may be transformed in place. */
static void transform_point(
    uint32_t matrix_address,
    uint32_t point_in,
    uint32_t point_out)
{
    float matrix[16];
    float point[3];
    float result[4];

    load_matrix(matrix_address, matrix);
    memcpy(point, recomp_memory(point_in, sizeof point), sizeof point);
    for (int lane = 0; lane < 4; ++lane) {
        float translated = matrix[8 + lane] * point[2];
        float result_lane = matrix[lane] * point[0];

        /* The original accumulates left to right into the first product, and
           moves the translation row in before the row sums meet. */
        translated = translated + matrix[12 + lane];
        result_lane = result_lane + matrix[4 + lane] * point[1];
        result[lane] = result_lane + translated;
    }
    memcpy(recomp_memory(point_out, sizeof result), result, sizeof result);
}

/* Guest 0x002842c0(ECX = point in, EDX = point out): transforms a point by the
   current transform. */
static void matrix_stack_transform_point(void)
{
    transform_point(MATRIX_CURRENT, recomp_runtime.registers.ecx,
        recomp_runtime.registers.edx);
    kernel_return_caller_cleanup(MATRIX_ROW1);
}

/* Guest 0x00284260(ECX = point): the same transform in place. */
static void matrix_stack_transform_point_in_place(void)
{
    const uint32_t point = recomp_runtime.registers.ecx;

    transform_point(MATRIX_CURRENT, point, point);
    kernel_return_caller_cleanup(MATRIX_ROW1);
}

/* Guest 0x00284480(ECX = matrix, EDX = point): transforms the point by an
   explicit transform rather than the current one. The original touches no
   general register but the stack, so EAX keeps its incoming value. */
static void matrix_transform_point(void)
{
    const uint32_t result = recomp_runtime.registers.eax;

    transform_point(recomp_runtime.registers.ecx, recomp_runtime.registers.edx,
        recomp_runtime.registers.edx);
    kernel_return_caller_cleanup(result);
}

/* Guest 0x00284520(ECX = matrix, EDX = direction, stack = out): transforms a
   direction by an explicit transform, without the translation row, so it
   rotates and scales instead of moving the point. The original leaves the
   output pointer in EAX. */
static void matrix_transform_direction(void)
{
    const uint32_t direction_in = recomp_runtime.registers.edx;
    const float *matrix;
    float direction[3];
    float result[4];
    uint32_t direction_out;

    matrix = (const float *)recomp_memory(
        recomp_runtime.registers.ecx, 12u * sizeof(float));
    memcpy(direction, recomp_memory(direction_in, sizeof direction),
        sizeof direction);
    for (int lane = 0; lane < 4; ++lane) {
        float value = matrix[lane] * direction[0];

        value = value + matrix[4 + lane] * direction[1];
        result[lane] = value + matrix[8 + lane] * direction[2];
    }
    memcpy(&direction_out,
        recomp_memory(recomp_runtime.registers.esp + 4u, sizeof direction_out),
        sizeof direction_out);
    memcpy(recomp_memory(direction_out, sizeof result), result, sizeof result);
    kernel_return(1u, direction_out);
}

/* Guest 0x002847f0 keeps its locals in a frame carved out below a 16-byte
   aligned stack pointer: a 0x54-byte block with the three callee-saved
   registers pushed under it, which is 0x60 bytes in all. The values stay in
   guest RAM after the call returns and some game code reads such leftover
   stack, so a replacement has to leave the same image even though it computes
   in host locals. Offsets are from the block base (the post-push stack
   pointer); the words the path never reaches are simply not written. */
enum {
    kInvertFrameEdi = 0x00 / 4,           /* pushed EDI */
    kInvertFrameEsi = 0x04 / 4,           /* pushed ESI */
    kInvertFrameEbx = 0x08 / 4,           /* pushed EBX */
    kInvertFrameRowsLeft = 0x18 / 4,      /* pivots the elimination has left */
    kInvertFrameOutputBack = 0x1c / 4,    /* inverse output row, from the end */
    kInvertFrameWriteCursor = 0x20 / 4,   /* out row / candidate index */
    kInvertFrameLastColumn = 0x24 / 4,    /* permutation entry of the last pass */
    kInvertFrameColumn = 0x28 / 4,        /* pass counter */
    kInvertFramePivot = 0x2c / 4,         /* pivot column */
    kInvertFrameRowOffset = 0x30 / 4,     /* byte offset of the output column */
    kInvertFrameSpan = 0x34 / 4,          /* columns left in the pass */
    kInvertFrameRows = 0x38 / 4,          /* output rows left */
    kInvertFrameFramePointer = 0x3c / 4,  /* the block base itself */
    kInvertFramePermutation = 0x40 / 4,   /* int[4], column order */
    kInvertFrameScale = 0x50 / 4,         /* float[4], 1 / largest |column| */
    kInvertFrameWords = 0x60 / 4
};

/* How far the original's passes got, which decides what its frame holds. */
typedef enum InvertPath {
    INVERT_PATH_COLUMNS, /* stopped in the column scan: only perm/scale so far */
    INVERT_PATH_PIVOT,   /* stopped on a zero pivot inside the elimination */
    INVERT_PATH_DONE     /* every pass ran and the inverse is complete */
} InvertPath;

/* Writes the frame image the original leaves for the path just taken.
   `columns` counts the scanned columns, `step` is the elimination pass that
   stopped, `pivot_column` its selected column. */
static void write_invert_frame(
    uint32_t stack_pointer,
    InvertPath path,
    const int *perm,
    const float *scale,
    int columns,
    int step,
    uint32_t pivot_column)
{
    const uint32_t base = ((stack_pointer - 4u) & 0xfffffff0u) -
        kInvertFrameWords * 4u;
    uint32_t *words = (uint32_t *)recomp_memory(base, kInvertFrameWords * 4u);
    float *reals = (float *)words;

    words[kInvertFrameEdi] = recomp_runtime.registers.edi;
    words[kInvertFrameEsi] = recomp_runtime.registers.esi;
    words[kInvertFrameEbx] = recomp_runtime.registers.ebx;
    /* The original pushes the frame pointer just above this block and pops it
       back unchanged: 1 / largest|column 3| ends four bytes below the block
       top, so the saved pointer is never overwritten. */
    *recomp_memory_u32(stack_pointer - 4u) = recomp_runtime.registers.ebp;

    for (int column = 0; column < columns; ++column) {
        words[kInvertFramePermutation + column] = (uint32_t)perm[column];
        reals[kInvertFrameScale + column] = scale[column];
    }
    if (path == INVERT_PATH_COLUMNS) return;

    if (path == INVERT_PATH_PIVOT) {
        /* The pivot column is written before the pivot test and the pass
           counter only after it passes, so on the very first pass those two
           cells still hold the incoming bytes and stay untouched. */
        words[kInvertFrameRowsLeft] = (uint32_t)(4 - step);
        words[kInvertFramePivot] = pivot_column;
        if (step > 0) {
            words[kInvertFrameColumn] = (uint32_t)step;
            words[kInvertFrameWriteCursor] = 4u;
        }
        return;
    }

    /* The build pass walked the output rows to the end, so the cursor sits one
       past the last row and every countdown has run out. */
    words[kInvertFrameRowsLeft] = MATRIX_END;
    words[kInvertFrameOutputBack] = 0u - MATRIX_END;
    words[kInvertFrameWriteCursor] = MATRIX_END;
    words[kInvertFrameLastColumn] = (uint32_t)perm[0];
    words[kInvertFrameColumn] = 4u;
    words[kInvertFramePivot] = 4u;
    words[kInvertFrameRowOffset] = (uint32_t)-4;
    words[kInvertFrameSpan] = 0u;
    words[kInvertFrameRows] = 0u;
    words[kInvertFrameFramePointer] = base + kInvertFrameFramePointer * 4u;
}

/* Guest 0x002847f0: inverts the 4x4 at ECX into the current transform, working
   on the copy at 0x00ea5f70. The original runs Gauss-Jordan over the
   transposed matrix: a first pass records the permutation and the reciprocal
   of each column's largest absolute element, the pivoting pass then swaps the
   largest scaled pivot into place and eliminates that column below it, and the
   last pass turns the permuted, triangular matrix back into the inverse rows.
   A column of zeros, or a zero pivot, abandons the inversion after the writes
   made so far, which is why the partial identity below is written where it is.
   Reads ECX only and leaves no result. */
static void matrix_stack_invert(void)
{
    const uint32_t stack_pointer = recomp_runtime.registers.esp;
    float work[16];
    int perm[4];
    float scale[4];
    float *current;
    int columns = 0;
    int step = 0;
    uint32_t pivot_column = 0;
    bool inverted = true;

    load_matrix(recomp_runtime.registers.ecx, work);
    memcpy(recomp_memory(MATRIX_SCRATCH, sizeof work), work, sizeof work);

    for (int column = 0; column < 4; ++column) {
        float largest = 0.0f;

        for (int row = 0; row < 4; ++row) {
            const float magnitude = render_abs(work[row * 4 + column]);

            if (magnitude > largest) largest = magnitude;
        }
        if (largest == 0.0f) { /* singular: the copy is all the original writes */
            write_invert_frame(stack_pointer, INVERT_PATH_COLUMNS, perm, scale,
                columns, 0, 0u);
            kernel_return_caller_cleanup(0u);
            return;
        }
        perm[column] = column;
        scale[column] = 1.0f / largest;
        columns = column + 1;
    }

    /* The cells of the output's identity the inverse pass never revisits: the
       original stores exactly these before it starts eliminating. */
    current = (float *)recomp_memory(MATRIX_CURRENT, 16u * sizeof(float));
    current[3] = 0.0f;
    current[7] = 0.0f;
    current[11] = 0.0f;
    current[12] = 0.0f;
    current[13] = 0.0f;
    current[14] = 0.0f;
    current[15] = 1.0f;

    for (step = 0; step < 4; ++step) {
        float best_value = -1.0f;
        int best = 0;

        for (int candidate = step; candidate < 4; ++candidate) {
            const int column = perm[candidate];
            const float value =
                scale[column] * render_abs(work[step * 4 + column]);

            if (value > best_value) {
                best_value = value;
                best = candidate;
            }
        }
        pivot_column = (uint32_t)perm[best];
        const float pivot = work[step * 4 + (int)pivot_column];

        if (best != step) {
            perm[best] = perm[step];
            perm[step] = (int)pivot_column;
        }
        if (pivot == 0.0f) { /* singular: keep what the elimination reached */
            inverted = false;
            break;
        }
        if (step + 1 >= 4) break; /* last pivot: nothing left to eliminate */

        const float reciprocal = 1.0f / pivot;

        /* Scale the pivot row's remaining entries into the eliminated column. */
        for (int candidate = step + 1; candidate < 4; ++candidate) {
            const int column = perm[candidate];
            const float factor = reciprocal * work[step * 4 + column];

            work[step * 4 + column] = factor;
            for (int row = step + 1; row < 4; ++row) {
                const float scaled = work[row * 4 + (int)pivot_column] * factor;

                work[row * 4 + column] = work[row * 4 + column] - scaled;
            }
        }
    }

    if (inverted) {
        for (int row = 0; row < 4; ++row) {
            /* Forward substitution over the permuted columns. */
            for (int column = 0; column < 4; ++column) {
                const int source = perm[column];
                float value = source == row ? 1.0f : 0.0f;

                for (int i = 0; i < column; ++i) {
                    const float product = current[row * 4 + i] * work[i * 4 + source];

                    value = value - product;
                }
                current[row * 4 + column] = value;
            }
            /* Back substitution for the columns the elimination transformed. */
            for (int column = 3; column >= 0; --column) {
                const int source = perm[column];
                float value = current[row * 4 + column];

                for (int i = column + 1; i < 4; ++i) {
                    const float product = current[row * 4 + i] * work[i * 4 + source];

                    value = value - product;
                }
                current[row * 4 + column] = value / work[column * 4 + source];
            }
        }
    }

    /* The eliminated matrix stays in memory; only the host copy is new. */
    memcpy(recomp_memory(MATRIX_SCRATCH, sizeof work), work, sizeof work);
    write_invert_frame(stack_pointer,
        inverted ? INVERT_PATH_DONE : INVERT_PATH_PIVOT, perm, scale, columns,
        step, pivot_column);
    kernel_return_caller_cleanup(0u);
}

RecompFunction recomp_render_xform_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x002847f0u: return matrix_stack_invert;
    case 0x002842c0u: return matrix_stack_transform_point;
    case 0x00284480u: return matrix_transform_point;
    case 0x00283ea0u: return matrix_stack_load;
    case 0x00283fc0u: return matrix_stack_translate_local;
    case 0x00284520u: return matrix_transform_direction;
    case 0x00284260u: return matrix_stack_transform_point_in_place;
    default: return NULL;
    }
}
