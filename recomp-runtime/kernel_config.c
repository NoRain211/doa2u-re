#include "kernel_abi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    REG_BINARY = 3u,
    REG_DWORD = 4u,
};

int recomp_kernel_widescreen = -1;
int recomp_kernel_surround = -1;

static int setting(int chosen, const char *name)
{
    const char *value = getenv(name);

    return chosen >= 0 ? chosen : value != NULL && strcmp(value, "0") != 0;
}

/* DOA2U selects its native 16:9 camera and UI from XC_VIDEO's widescreen bit;
   the presenter fits the picture to the window either way. */
uint32_t recomp_kernel_video_flags(void)
{
    return setting(recomp_kernel_widescreen, "RECOMP_D3D_WIDESCREEN")
        ? 0x004f0000u : 0x004e0000u;
}

/* XC_AUDIO: surround with AC3 (discrete 5.1) sends DOA2 and DSOUND down
   their 5.1 paths; the output folds that mix to the local speakers. */
uint32_t recomp_kernel_audio_flags(void)
{
    return setting(recomp_kernel_surround, "RECOMP_AUDIO_SURROUND")
        ? 0x00010002u : 0u;
}

uint32_t recomp_kernel_query_nonvolatile_setting(
    uint32_t value_index,
    uint32_t type,
    uint32_t value,
    uint32_t value_length,
    uint32_t result_length)
{
    uint32_t dword_value = value_index == 0x08u ? recomp_kernel_video_flags()
        : value_index == 0x09u ? recomp_kernel_audio_flags() : 0u;
    int dword_backed = value_length == sizeof dword_value;

    if (type != 0u) {
        *recomp_memory_u32(type) = dword_backed ? REG_DWORD : REG_BINARY;
    }
    if (result_length != 0u) {
        *recomp_memory_u32(result_length) =
            dword_backed ? (uint32_t)sizeof dword_value : value_length;
    }
    if (value != 0u && value_length > 0u) {
        recomp_guest_memset(value, 0, value_length);
        if (dword_backed) {
            *recomp_memory_u32(value) = dword_value;
        } else if (value_index == 0xffffu && value_length >= 0x46u) {
            for (uint32_t i = 0u; i < 0x10u; ++i) {
                *recomp_memory_i8(value + 0x36u + i) =
                    (int8_t)(0x11u + i);
            }
        }
    }
    return 0u;
}

static void bridge_ex_query_nonvolatile_setting(void)
{
    uint32_t value_index = kernel_arg(1u);
    uint32_t type = kernel_arg(2u);
    uint32_t value = kernel_arg(3u);
    uint32_t value_length = kernel_arg(4u);
    uint32_t result_length = kernel_arg(5u);

    kernel_return(
        5u,
        recomp_kernel_query_nonvolatile_setting(
            value_index, type, value, value_length, result_length));
}

RecompFunction recomp_kernel_config(uint32_t ordinal)
{
    switch (ordinal) {
    case 24u: return bridge_ex_query_nonvolatile_setting;
    default: return NULL;
    }
}
