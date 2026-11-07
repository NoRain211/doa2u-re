/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stage_select.h"
#include "kernel_abi.h"
#include "xbox_memory_layout.h"

#include <string.h>

/* Stage Select (issue #117). The game keeps 24-byte stage entries at 0x6e22b0:
   +0 Random flag, +1..3 the stage id per variant, +4 the name label, +8 and
   +0xc the 4:3 and wide thumbnails. 0x1354a0 lists a mode's entry numbers as
   byte rows at 0xdf5f10 and appends Random (entry 0x15); the extra row ends
   at 0xdf5f26, short of the next data the game uses (0xdf5f30). Rows are
   read only through 0x135410, 0x135510 and 0x135580. These replace all four
   to add Tengu's boss stage (id 18, the Miyama his story fights use) before
   Random in the regular list. Other data follows the table, so its entry
   lives in the reserved kernel page. Labels 61, 14 and 36 are the game's
   unused Miyama name and thumbnails. */
enum {
    STAGE_ENTRIES = 0x006e22b0u,
    STAGE_ENTRY_SIZE = 0x18u,
    STAGE_MODES = 0x006e22a4u, /* per list: first entry, entry count */
    STAGE_ROW_COUNT = 0x006e22aau,
    STAGE_ROWS = 0x00df5f10u,
    STAGE_TAG_MATCH = 0x0087d49du,
    STAGE_REGULAR_LIST = 2u,
    STAGE_TAG_LIST = 1u,
    STAGE_RANDOM_ENTRY = 0x15u,
    MIYAMA_ENTRY = 0x16u,
    MIYAMA_STAGE = 18u,
};
#define MIYAMA_ENTRY_ADDRESS (XBOX_KERNEL_DATA_BASE + 0xc00u)

static uint8_t guest_byte(uint32_t address) { return *recomp_memory(address, 1u); }

static uint32_t stage_entry(uint8_t entry)
{
    return entry == MIYAMA_ENTRY ? MIYAMA_ENTRY_ADDRESS : STAGE_ENTRIES + entry * STAGE_ENTRY_SIZE;
}

static uint8_t stage_row_entry(uint32_t row)
{
    const uint32_t count = guest_byte(STAGE_ROW_COUNT);
    return guest_byte(STAGE_ROWS + (row < count ? row : count - 1u));
}

static void stage_select_build_rows(void)
{
    uint32_t list = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    if (guest_byte(STAGE_TAG_MATCH) != 0u) list = STAGE_TAG_LIST;
    const uint8_t first = guest_byte(STAGE_MODES + list * 2u);
    const uint8_t count = guest_byte(STAGE_MODES + list * 2u + 1u);
    uint8_t *rows = recomp_memory(STAGE_ROWS, count + 2u);
    uint8_t row = 0u;
    for (; row < count; ++row) rows[row] = (uint8_t)(first + row);
    if (list == STAGE_REGULAR_LIST) {
        uint8_t *miyama = recomp_memory(MIYAMA_ENTRY_ADDRESS, STAGE_ENTRY_SIZE);
        memcpy(miyama, recomp_memory(STAGE_ENTRIES, STAGE_ENTRY_SIZE), STAGE_ENTRY_SIZE);
        miyama[1] = miyama[2] = miyama[3] = MIYAMA_STAGE;
        const uint16_t labels[3] = {61u, 14u, 36u};
        for (unsigned i = 0u; i < 3u; ++i) memcpy(miyama + 4u + i * 4u, &labels[i], 2u);
        rows[row++] = MIYAMA_ENTRY;
    }
    rows[row] = STAGE_RANDOM_ENTRY;
    *recomp_memory(STAGE_ROW_COUNT, 1u) = (uint8_t)(row + 1u);
    kernel_return(0u, row + 1u);
}

static void stage_select_row_entry(void)
{
    const uint8_t row = *recomp_memory(recomp_runtime.registers.esp + 4u, 1u);
    kernel_return(0u, stage_entry(stage_row_entry(row)));
}

static void stage_select_row_stage(void)
{
    const int8_t row = (int8_t)*recomp_memory(recomp_runtime.registers.esp + 4u, 1u);
    const uint8_t variant = *recomp_memory(recomp_runtime.registers.esp + 8u, 1u);
    kernel_return(0u, guest_byte(stage_entry(stage_row_entry(row < 0 ? 0u : (uint32_t)row)) + 1u + variant));
}

static void stage_select_find_stage(void)
{
    const uint8_t stage = *recomp_memory(recomp_runtime.registers.esp + 4u, 1u);
    const uint32_t variant_out = *recomp_memory_u32(recomp_runtime.registers.esp + 8u);
    const uint32_t count = guest_byte(STAGE_ROW_COUNT);
    for (uint32_t row = 0u; row < count; ++row) {
        const uint32_t entry = stage_entry(guest_byte(STAGE_ROWS + row));
        for (uint32_t step = 1u; step <= 3u; ++step) {
            const uint8_t variant = (uint8_t)(step % 3u); /* the game tries 1, 2, then 0 */
            const uint8_t id = guest_byte(entry + 1u + variant);
            if (id < 0x80u && id == stage) { /* Random's 0xff never matches */
                *recomp_memory(variant_out, 1u) = variant;
                kernel_return(0u, row);
                return;
            }
        }
    }
    *recomp_memory(variant_out, 1u) = 0u;
    kernel_return(0u, 0u);
}

RecompFunction recomp_stage_select_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case 0x00135410u: return stage_select_row_entry;
    case 0x001354a0u: return stage_select_build_rows;
    case 0x00135510u: return stage_select_row_stage;
    case 0x00135580u: return stage_select_find_stage;
    default: return NULL;
    }
}
