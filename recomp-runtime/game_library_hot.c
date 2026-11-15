/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hand-decompiled hot library routines at 0x30bf90, 0x30c280 and 0x2bffba.
   Both replacements keep the lifted function's guest RAM writes, register
   results and float rounding, so rollback peers and SyncTest see the same
   state. The dispatcher at 0x30c280 is bound too because its tail jumps go
   straight to the lifted bodies, so binding the decoder alone never ran. */
#include "game_decomp.h"
#include "kernel_abi.h"

#include <string.h>

/* 0x0030bc30, the mono-downmix decoder, stays lifted: the dispatcher below
   tail jumps into its body, so calling it directly has the same effect. */
extern void sub_0030BC30(void);

/* Guest table 0x00494420: the signed value of a 4-bit ADPCM nibble. */
static const int32_t adpcm_nibble[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1
};

/* Every ADX prediction saturates to a signed 16-bit sample. */
static int32_t adpcm_saturate(int32_t value)
{
    if (value > 0x7fff) return 0x7fff;
    if (value < -0x8000) return -0x8000;
    return value;
}

static uint16_t byte_swap(uint16_t value)
{
    return (uint16_t)((value >> 8) | (value << 8));
}

/* CRI ADX (Sofdec) sound decoder, 0x0030bf90. Its arguments, all on the
   stack, are: the frame data, the frame count (halved here), the left and
   right output buffers, their two 2-sample histories, the two predictor
   coefficients, a rolling decryption key and that key's linear feedback
   terms. One frame is 0x24 bytes: for each channel a 2-byte big endian
   header whose low 13 bits, after undoing the key, give the block scale, then
   16 bytes of nibble-packed samples. Each nibble is scaled and added to a
   two-tap prediction from the channel's two previous samples.
   The lifted body returns the frame-count argument (its epilogue reloads it
   after popping EDI), or twice the number of frames decoded when a header is
   the end marker, and its caller stores that word at object+0x10, so the
   return value is reproduced as it stands. */
static void adx_decode_stereo_frames(void)
{
    const uint32_t frames_address = kernel_arg(1);
    const uint32_t frame_count_argument = kernel_arg(2);
    const int32_t frame_count = (int32_t)frame_count_argument / 2;
    const uint32_t left_output_address = kernel_arg(3);
    const uint32_t left_history_address = kernel_arg(4);
    const uint32_t right_output_address = kernel_arg(5);
    const uint32_t right_history_address = kernel_arg(6);
    const int32_t coefficient1 = (int16_t)kernel_arg(7);
    const int32_t coefficient2 = (int16_t)kernel_arg(8);
    const uint32_t key_address = kernel_arg(9);
    const uint32_t key_multiplier = kernel_arg(10);
    const uint32_t key_increment = kernel_arg(11);

    int16_t *const left_history = (int16_t *)recomp_memory(left_history_address, 4u);
    int16_t *const right_history = (int16_t *)recomp_memory(right_history_address, 4u);
    uint16_t *const key = (uint16_t *)recomp_memory(key_address, 2u);
    /* history[0] is the newest sample, history[1] the one before it. */
    int32_t left_newest = left_history[0];
    int32_t left_older = left_history[1];
    int32_t right_newest = right_history[0];
    int32_t right_older = right_history[1];

    int32_t decoded_frames = 0;
    for (; decoded_frames < frame_count; ++decoded_frames) {
        const uint8_t *const frame = recomp_memory(
            frames_address + 0x24u * (uint32_t)decoded_frames, 0x24u);
        int16_t *const left_output = (int16_t *)recomp_memory(
            left_output_address + 0x40u * (uint32_t)decoded_frames, 0x40u);
        int16_t *const right_output = (int16_t *)recomp_memory(
            right_output_address + 0x40u * (uint32_t)decoded_frames, 0x40u);
        const uint16_t left_header = (uint16_t)(frame[0] | frame[1] << 8);

        if ((left_header & 0x80u) != 0u) {
            kernel_return_caller_cleanup((uint32_t)(2 * decoded_frames));
            return;
        }
        uint32_t rolling_key = *key;
        const int32_t left_scale =
            (int32_t)(((rolling_key ^ byte_swap(left_header)) & 0x1fffu) + 1u);
        rolling_key = (rolling_key * key_multiplier + key_increment) & 0x7fffu;
        *key = (uint16_t)rolling_key;

        const uint16_t right_header = (uint16_t)(frame[0x12] | frame[0x13] << 8);
        if ((right_header & 0x80u) != 0u) {
            kernel_return_caller_cleanup((uint32_t)(2 * decoded_frames));
            return;
        }
        const int32_t right_scale =
            (int32_t)(((rolling_key ^ byte_swap(right_header)) & 0x1fffu) + 1u);
        rolling_key = (rolling_key * key_multiplier + key_increment) & 0x7fffu;
        *key = (uint16_t)rolling_key;

        for (unsigned i = 0; i < 16u; ++i) {
            const int32_t left_byte = (int8_t)frame[2u + i];
            const int32_t right_byte = (int8_t)frame[0x14u + i];
            /* High nibble first: predict from the two previous samples. */
            const int32_t left_first = adpcm_saturate(
                ((coefficient1 * left_newest + coefficient2 * left_older) >> 12) +
                (left_byte >> 4) * left_scale);
            const int32_t right_first = adpcm_saturate(
                ((coefficient1 * right_newest + coefficient2 * right_older) >> 12) +
                (right_byte >> 4) * right_scale);
            left_output[2u * i] = (int16_t)left_first;
            right_output[2u * i] = (int16_t)right_first;
            /* Low nibble: predict from that sample and the previous one. */
            const int32_t left_second = adpcm_saturate(
                ((coefficient1 * left_first + coefficient2 * left_newest) >> 12) +
                adpcm_nibble[left_byte & 0xf] * left_scale);
            const int32_t right_second = adpcm_saturate(
                ((coefficient1 * right_first + coefficient2 * right_newest) >> 12) +
                adpcm_nibble[right_byte & 0xf] * right_scale);
            left_output[2u * i + 1u] = (int16_t)left_second;
            right_output[2u * i + 1u] = (int16_t)right_second;
            left_older = left_first;
            left_newest = left_second;
            right_older = right_first;
            right_newest = right_second;
        }
    }

    left_history[0] = (int16_t)left_newest;
    left_history[1] = (int16_t)left_older;
    right_history[1] = (int16_t)right_older;
    right_history[0] = (int16_t)right_newest;
    kernel_return_caller_cleanup(frame_count_argument);
}

/* ADX frame decode dispatch, 0x0030c280: one 32-bit load of the mono-downmix
   flag and a conditional tail jump. A tail jump keeps the caller's return
   address and arguments, so calling a decoder body directly is equivalent -
   it reads the same stack arguments and pops that return address itself, and
   the dispatcher then returns without touching the stack again. */
static void adx_decode_frames(void)
{
    if (*recomp_memory_u32(0x00f2bc34u) == 0u) {
        adx_decode_stereo_frames();
        return;
    }
    sub_0030BC30();
}

/* Row-major 4x4 matrix product, 0x002bffba: out = a * b, stdcall with the
   output first, the way D3DXMatrixMultiply takes them. Unlike the D3DX8 copy
   in the D3DX section, this one works on aligned 16-byte rows with SSE, has
   no aliasing guard, and sums each element left to right, so the products
   stay single-precision and in that order instead of reassociating. */
static void matrix_4x4_multiply(void)
{
    const uint32_t output_address = kernel_arg(1);
    const uint32_t left_address = kernel_arg(2);
    const uint32_t right_address = kernel_arg(3);
    float left[16];
    float right[16];
    float product[16];

    memcpy(left, recomp_memory(left_address, sizeof left), sizeof left);
    memcpy(right, recomp_memory(right_address, sizeof right), sizeof right);

    for (unsigned row = 0u; row < 4u; ++row) {
        const float a0 = left[row * 4u];
        const float a1 = left[row * 4u + 1u];
        const float a2 = left[row * 4u + 2u];
        const float a3 = left[row * 4u + 3u];
        for (unsigned column = 0u; column < 4u; ++column) {
            /* mulps then addps in this order: (((a0*b0 + a1*b1) + a2*b2) + a3*b3). */
            float sum = a0 * right[column];
            sum = sum + a1 * right[4u + column];
            sum = sum + a2 * right[8u + column];
            sum = sum + a3 * right[12u + column];
            product[row * 4u + column] = sum;
        }
    }

    memcpy(recomp_memory(output_address, sizeof product), product, sizeof product);
    kernel_return(3u, output_address);
}

/* Temporary seam: CRI ADX and the SDK's matrix multiply are library code,
   which AGENTS.md replaces wholesale rather than decompiles. No native ADX
   or D3DX replacement exists yet, and both are hot in rollback re-runs, so
   these exact copies stand in until one does; that replacement must keep
   their guest RAM writes. sub_0030BC30 is the lifted mono decoder the
   dispatcher reaches by a tail jump, so it binds to the lifter's name. */
RecompFunction recomp_library_hot_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0030c280u: return adx_decode_frames;
    case 0x0030bf90u: return adx_decode_stereo_frames;
    case 0x002bffbau: return matrix_4x4_multiply;
    default: return NULL;
    }
}
