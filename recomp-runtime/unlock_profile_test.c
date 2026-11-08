/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "unlock_profile.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint32_t guest_stack[4];          /* at 0x1000 */
static uint32_t game_data;               /* 0x87cfa8 */
static uint8_t order[15];                /* 0x6959dc */
static uint8_t slot_of[0x20];            /* 0x6959c0 */
static uint8_t counts[0x20];             /* 0x6e0f08 */
static uint8_t slot[0x12118];            /* a save slot at 0x100000 */
static uint8_t records[0x100];           /* reserved page +0xd00 */

enum { SLOT = 0x00100000u, DATA = SLOT + 0xf200u };

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address == 0x0087cfa8u && width <= 4u) return (uint8_t *)&game_data;
    if (address >= 0x006959dcu && address + width <= 0x006959dcu + sizeof order) return &order[address - 0x006959dcu];
    if (address >= 0x006959c0u && address + width <= 0x006959c0u + sizeof slot_of) return &slot_of[address - 0x006959c0u];
    if (address >= 0x006e0f08u && address + width <= 0x006e0f08u + sizeof counts) return &counts[address - 0x006e0f08u];
    if (address >= SLOT && address + width <= SLOT + sizeof slot) return &slot[address - SLOT];
    if (address >= XBOX_KERNEL_DATA_BASE + 0xd00u && address + width <= XBOX_KERNEL_DATA_BASE + 0xd00u + sizeof records)
        return &records[address - XBOX_KERNEL_DATA_BASE - 0xd00u];
    if (address >= 0x1000u && address + width <= 0x1000u + sizeof guest_stack)
        return (uint8_t *)guest_stack + (address - 0x1000u);
    abort();
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

/* Character i's costume byte and the three flags, as the game lays them out. */
static uint8_t *costume(unsigned i) { return &slot[0xf200u + 0x2ed8u + slot_of[order[i]]]; }
static uint8_t *flag(unsigned i) { return &slot[0xf200u + 0x2ed2u + i]; }

static uint8_t saved_costume0, saved_flag0;
static unsigned saves, loads;

void sub_00021B50(void) { ++saves; saved_costume0 = *costume(0); saved_flag0 = *flag(0); }
void sub_00021BB0(void) { sub_00021B50(); }
void sub_00021D90(void)
{
    ++loads;
    for (unsigned i = 0u; i < 15u; ++i) *costume(i) = 3u; /* another profile, three costumes each */
    memset(flag(0), 0, 3u);
}
void sub_00021DE0(void) { sub_00021D90(); }
/* An award: one more costume for the first character and its hidden flag. */
void sub_000DC840(void)
{
    if (*costume(0) < counts[order[0]]) ++*costume(0);
    *flag(0) = 1u;
}

static void call(uint32_t address)
{
    recomp_runtime.registers.esp = 0x1000u;
    guest_stack[1] = 0u;
    guest_stack[2] = SLOT;
    recomp_unlock_profile_lookup_manual(address)();
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL unlock profile: %s\n", what);
    return 0;
}

static int test_unlock_profile(void)
{
    for (unsigned i = 0u; i < 15u; ++i) {
        order[i] = (uint8_t)(14u - i);
        slot_of[order[i]] = (uint8_t)i;
        counts[order[i]] = (uint8_t)(4u + i % 3u);
        *costume(i) = 2u; /* earned: two each */
    }
    memset(flag(0), 0, 3u);
    game_data = DATA;
    recomp_unlock_profile_reset();

    recomp_unlock_profile_frame(true);
    if (*costume(0) != counts[order[0]] || *costume(14) != counts[order[14]] || *flag(2) != 1u)
        return fail("unlocks in memory");
    call(0x00021b50u);
    if (saves != 1u || saved_costume0 != 2u || saved_flag0 != 0u) return fail("save holds the earned values");
    if (*costume(0) != counts[order[0]] || *flag(0) != 1u) return fail("unlocked again after the save");

    call(0x000dc840u);
    if (*costume(0) != counts[order[0]]) return fail("unlocked again after the award");
    call(0x00021bb0u);
    if (saved_costume0 != 3u || saved_flag0 != 1u) return fail("the award is earned and saved");
    recomp_unlock_profile_frame(true);
    call(0x00021b50u);
    if (saved_costume0 != 3u) return fail("a later frame keeps the award");

    recomp_unlock_profile_frame(false);
    if (*costume(0) != 3u || *costume(1) != 2u || *flag(0) != 1u || *flag(1) != 0u)
        return fail("off gives back the earned values");
    recomp_unlock_profile_frame(false);
    if (*costume(0) != 3u) return fail("off twice is still earned");

    /* A load replaces the profile; its values are the ones kept. */
    recomp_unlock_profile_frame(true);
    call(0x00021d90u);
    if (loads != 1u || *costume(0) != 3u) return fail("load");
    recomp_unlock_profile_frame(true);
    call(0x00021b50u);
    if (saved_costume0 != 3u || saved_flag0 != 0u) return fail("the loaded profile's values are saved");
    /* A profile loaded fully unlocked after the record went is left as it is. */
    for (unsigned i = 0u; i < 15u; ++i) *costume(i) = counts[order[i]];
    memset(flag(0), 1, 3u);
    call(0x00021de0u);
    for (unsigned i = 0u; i < 15u; ++i) *costume(i) = counts[order[i]];
    memset(flag(0), 1, 3u);
    recomp_unlock_profile_frame(false);
    if (*costume(0) != counts[order[0]] || *flag(0) != 1u) return fail("a loaded unlocked profile is not reverted");

    /* Without the setting the hooks change nothing. */
    *costume(0) = 2u;
    call(0x00021b50u);
    call(0x000dc840u);
    if (saved_costume0 != 2u || *costume(0) != 3u) return fail("off, saves and awards are the game's");
    if (recomp_unlock_profile_lookup_manual(0x00021c50u) != NULL) return fail("other functions unhooked");
    return 1;
}

int main(void)
{
    return test_unlock_profile() ? 0 : 1;
}
