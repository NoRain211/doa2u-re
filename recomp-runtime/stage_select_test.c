/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stage_select.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint32_t guest_stack[4];                    /* at 0x1000 */
static uint8_t variant_out;                        /* at 0x3000 */
static uint8_t stage_table[0x6e2500u - 0x6e22a0u]; /* list modes, row count and entries */
static uint8_t stage_rows[32];                     /* at 0xdf5f10 */
static uint8_t stage_tag_match;                    /* at 0x87d49d */
static uint8_t kernel_page[0x20];                  /* reserved page +0xc00 */

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x6e22a0u && address + width <= 0x6e22a0u + sizeof stage_table)
        return &stage_table[address - 0x6e22a0u];
    if (address >= 0xdf5f10u && address + width <= 0xdf5f10u + sizeof stage_rows) return &stage_rows[address - 0xdf5f10u];
    if (address == 0x87d49du && width == 1u) return &stage_tag_match;
    if (address >= XBOX_KERNEL_DATA_BASE + 0xc00u && address + width <= XBOX_KERNEL_DATA_BASE + 0xc00u + sizeof kernel_page)
        return &kernel_page[address - XBOX_KERNEL_DATA_BASE - 0xc00u];
    if (address >= 0x1000u && address + width <= 0x1000u + sizeof guest_stack)
        return (uint8_t *)guest_stack + (address - 0x1000u);
    if (address == 0x3000u && width == 1u) return &variant_out;
    abort();
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

static uint32_t call_cdecl(uint32_t address, uint32_t a, uint32_t b)
{
    RecompFunction function = recomp_stage_select_lookup_manual(address);
    if (function == NULL) return 0xdeadu;
    recomp_runtime.registers.esp = 0x1000u;
    guest_stack[1] = a;
    guest_stack[2] = b;
    function();
    return recomp_runtime.registers.esp == 0x1004u ? recomp_runtime.registers.eax : 0xbeefu;
}

/* Stage select lists the regular stages, then Tengu's boss stage, then Random;
   the tag list is unchanged. Row lookups go both ways through the new entry. */
static int test_stage_select(void)
{
    const char *failed = NULL;
    uint8_t *modes = &stage_table[4], *entries = &stage_table[0x10];
    modes[2] = 0x10u; modes[3] = 5u; modes[4] = 0u; modes[5] = 0x15u; /* tag: 16-20, regular: 0-20 */
    for (unsigned i = 0u; i < 0x15u; ++i) memset(entries + i * 0x18u + 1u, (int)(i + 30u), 3u);
    entries[0x15u * 0x18u] = 1u;
    memset(entries + 0x15u * 0x18u + 1u, 0xff, 3u);
    if (call_cdecl(0x001354a0u, 2u, 0u) != 23u || stage_table[0xa] != 23u ||
        stage_rows[20] != 20u || stage_rows[21] != 0x16u || stage_rows[22] != 0x15u) failed = "regular rows";
    else if (call_cdecl(0x00135510u, 21u, 0u) != 18u || call_cdecl(0x00135510u, 3u, 1u) != 33u ||
        call_cdecl(0x00135510u, 40u, 0u) != 0xffu) failed = "row stage"; /* past the end clamps to Random */
    else if (call_cdecl(0x00135410u, 21u, 0u) != XBOX_KERNEL_DATA_BASE + 0xc00u ||
        kernel_page[4] != 61u || kernel_page[5] != 0u || kernel_page[8] != 14u || kernel_page[9] != 0u ||
        kernel_page[12] != 36u || kernel_page[13] != 0u) failed = "Miyama entry";
    else if (call_cdecl(0x00135410u, 22u, 0u) != 0x6e22b0u + 0x15u * 0x18u) failed = "Random entry";
    if (failed != NULL) {
        fprintf(stderr, "FAIL stage select: %s\n", failed);
        return 0;
    }
    variant_out = 9;
    if (call_cdecl(0x00135580u, 18u, 0x3000u) != 21u || variant_out != 1) failed = "find Miyama";
    else if (call_cdecl(0x00135580u, 0xffu, 0x3000u) != 0u || variant_out != 0) failed = "find missing";
    else if (call_cdecl(0x001354a0u, 1u, 0u) != 6u || stage_rows[4] != 20u || stage_rows[5] != 0x15u)
        failed = "short list";
    stage_tag_match = 1u;
    if (failed == NULL && call_cdecl(0x001354a0u, 2u, 0u) != 6u) failed = "tag list";
    stage_tag_match = 0u;
    if (failed != NULL) fprintf(stderr, "FAIL stage select: %s\n", failed);
    return failed == NULL;
}

int main(void)
{
    if (recomp_stage_select_lookup_manual(0x00135400u) != NULL) return 1;
    return test_stage_select() ? 0 : 1;
}
