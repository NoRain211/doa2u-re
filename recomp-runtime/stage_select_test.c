/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stage_select.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t guest_stack[0x200];                 /* at 0xf00, the arguments at 0x1000 */
static uint8_t variant_out;                        /* at 0x3000 */
static uint8_t pad_slots[0x20];                    /* at 0x6e2264, 8 bytes per slot */
static uint8_t stage_table[0x6e2500u - 0x6e22a0u]; /* list modes, row count and entries */
static uint8_t pad_pressed[0x60];                  /* at 0x87d268, two pads */
static uint8_t stage_tag_match;                    /* at 0x87d49d */
static uint8_t stage_cursor;                       /* at 0xdf57c4 */
static uint8_t stage_confirming[2];               /* at 0xdf57ce */
static uint8_t stage_rows[32];                     /* at 0xdf5f10 */
static uint8_t kernel_page[0x20];                  /* reserved page +0xc00 */

/* The ids one stage group (0x693370) holds; Up steps through them in order. */
static const uint8_t MIYAMA_IDS[4] = {18u, 64u, 63u, 65u};

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x0f00u && address + width <= 0x1100u) return &guest_stack[address - 0x0f00u];
    if (address >= 0x6e2264u && address + width <= 0x6e2264u + sizeof pad_slots) return &pad_slots[address - 0x6e2264u];
    if (address >= 0x6e22a0u && address + width <= 0x6e22a0u + sizeof stage_table)
        return &stage_table[address - 0x6e22a0u];
    if (address >= 0x87d268u && address + width <= 0x87d268u + sizeof pad_pressed) return &pad_pressed[address - 0x87d268u];
    if (address == 0x87d49du && width == 1u) return &stage_tag_match;
    if (address == 0xdf57c4u && width == 1u) return &stage_cursor;
    if (address >= 0xdf57ceu && address + width <= 0xdf57ceu + sizeof stage_confirming)
        return &stage_confirming[address - 0xdf57ceu];
    if (address >= 0xdf5f10u && address + width <= 0xdf5f10u + sizeof stage_rows) return &stage_rows[address - 0xdf5f10u];
    if (address >= XBOX_KERNEL_DATA_BASE + 0xc00u && address + width <= XBOX_KERNEL_DATA_BASE + 0xc00u + sizeof kernel_page)
        return &kernel_page[address - XBOX_KERNEL_DATA_BASE - 0xc00u];
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
    *recomp_memory_u32(0x1004u) = a;
    *recomp_memory_u32(0x1008u) = b;
    function();
    return recomp_runtime.registers.esp == 0x1004u ? recomp_runtime.registers.eax : 0xbeefu;
}

/* The lifted functions the confirm hook calls, as the game would run them. */
static uint32_t sfx_played, commit_side, commit_id, commit_loader, commit_slot, original_calls, original_slot;

static uint32_t arg(unsigned i) { return *recomp_memory_u32(recomp_runtime.registers.esp + 4u * i); }
static void ret(uint32_t eax)
{
    recomp_runtime.registers.eax = eax;
    recomp_runtime.registers.esp += 4u;
}

void sub_000FED70(void)
{
    sfx_played = arg(1u);
    ret(0u);
}
void sub_0012F360(void)
{
    commit_side = arg(1u);
    commit_id = arg(2u);
    commit_loader = arg(3u);
    commit_slot = arg(4u);
    ret(0u);
}
void sub_001357F0(void)
{
    ++original_calls;
    original_slot = arg(1u);
    ret(0x77u);
}

/* Stage select lists the regular stages, then Tengu's boss stage, then Random;
   the tag list is unchanged. Row lookups go both ways through the new entry,
   and Up on it steps through the five ids of its stage group. */
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

/* Up on the Miyama row is the only way to reach the other three variants: it
   steps 18 to 64, 63 and 65, then back to 18, sounds the row and commits the
   preview each time. X (27, a black void) does nothing there. Other rows and
   other presses stay the game's. */
static int test_miyama_variants(void)
{
    const char *failed = NULL;
    call_cdecl(0x001354a0u, 2u, 0u);
    stage_cursor = 21u; /* the Miyama row */
    pad_slots[3u * 8u] = 1u; /* slot 3 reads pad 1 */
    *recomp_memory_u32(0x87d268u) = 0x400u; /* pad 0 is not the one read */
    *recomp_memory_u32(0x87d298u) = 0x1u; /* pad 1's D-pad Up */
    for (unsigned i = 1u; i <= 4u; ++i) {
        const uint8_t id = MIYAMA_IDS[i % 4u];
        sfx_played = 0u;
        if (i == 4u) *recomp_memory_u32(0x87d298u) = 0x10000u; /* left-stick up */
        if (call_cdecl(0x001357f0u, 3u, 0xdeadu) != 0u || kernel_page[1] != id || kernel_page[2] != id ||
            kernel_page[3] != id || sfx_played != 0x23u || commit_side != 1u || commit_id != id ||
            commit_loader != 0x1364e0u || commit_slot != 0xffffffffu) {
            failed = "Miyama up";
            break;
        }
    }
    stage_cursor = 3u;
    original_calls = 0u;
    if (failed == NULL && (call_cdecl(0x001357f0u, 3u, 0xdeadu) != 0x77u || original_calls != 1u ||
        original_slot != 3u)) failed = "other row";
    stage_cursor = 21u;
    *recomp_memory_u32(0x87d298u) = 0x400u; /* X, the game's pick of 27 */
    if (failed == NULL && (call_cdecl(0x001357f0u, 3u, 0xdeadu) != 0u || original_calls != 1u))
        failed = "X";
    *recomp_memory_u32(0x87d298u) = 0x800u; /* Y, which belongs to the game's handler */
    if (failed == NULL && (call_cdecl(0x001357f0u, 3u, 0xdeadu) != 0x77u || original_calls != 2u))
        failed = "other press";
    *recomp_memory_u32(0x87d298u) = 0x1u;
    stage_confirming[1] = 1u; /* after a confirm, Up leaves the chosen id alone */
    if (failed == NULL && (call_cdecl(0x001357f0u, 3u, 0xdeadu) != 0x77u || original_calls != 3u ||
        kernel_page[1] != 18u)) failed = "confirmed";
    stage_confirming[1] = 0u;
    variant_out = 9u;
    if (failed == NULL && (call_cdecl(0x00135580u, 27u, 0x3000u) != 0u || variant_out != 0u ||
        kernel_page[1] != 18u)) failed = "find 27";
    for (unsigned i = 1u; i < 4u && failed == NULL; ++i) { /* find-stage restores the online scene's id */
        kernel_page[1] = kernel_page[2] = kernel_page[3] = 18u;
        variant_out = 9u;
        if (call_cdecl(0x00135580u, MIYAMA_IDS[i], 0x3000u) != 21u || variant_out != 1u ||
            kernel_page[1] != MIYAMA_IDS[i] || kernel_page[2] != MIYAMA_IDS[i] ||
            kernel_page[3] != MIYAMA_IDS[i]) failed = "find variant";
    }
    kernel_page[1] = kernel_page[2] = kernel_page[3] = 63u; /* a rebuilt list keeps the pick */
    if (failed == NULL && (call_cdecl(0x001354a0u, 2u, 0u) != 23u ||
        kernel_page[1] != 63u || kernel_page[2] != 63u || kernel_page[3] != 63u)) failed = "kept variant";
    kernel_page[1] = kernel_page[2] = kernel_page[3] = 7u;
    if (failed == NULL && (call_cdecl(0x001354a0u, 2u, 0u) != 23u ||
        kernel_page[1] != 18u || kernel_page[2] != 18u || kernel_page[3] != 18u)) failed = "reset variant";
    kernel_page[1] = kernel_page[2] = kernel_page[3] = 65u; /* Random reads the current variant */
    if (failed == NULL && call_cdecl(0x00135510u, 21u, 0u) != 65u) failed = "Random variant";
    if (failed != NULL) fprintf(stderr, "FAIL stage select: %s\n", failed);
    return failed == NULL;
}

int main(void)
{
    if (recomp_stage_select_lookup_manual(0x00135400u) != NULL) return 1;
    return test_stage_select() && test_miyama_variants() ? 0 : 1;
}
