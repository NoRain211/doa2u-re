/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_guest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SLOT = 0x007cb660u + 0x12140u, /* g_profile_slots[1] */
    SLOT_SIZE = 0x12140u,
    PROFILE_DATA = 0x0087cfa8u,
    DATA = 0x5000u,
    STACK = 0x1000u,
};

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t slot[SLOT_SIZE];
static uint8_t data[0x50];
static uint32_t data_pointer = DATA;
static uint32_t stack[128];
static unsigned clears, reads;
static uint32_t missing_read = UINT32_MAX; /* this read returns not found */
static uint16_t read_names[2][16];

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= SLOT && address + width <= SLOT + SLOT_SIZE) return &slot[address - SLOT];
    if (address >= DATA && address + width <= DATA + sizeof data) return &data[address - DATA];
    if (address >= STACK && address + width <= STACK + sizeof stack) return (uint8_t *)stack + (address - STACK);
    if (address == PROFILE_DATA && width == 4u) return (uint8_t *)&data_pointer;
    abort();
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

void sub_000244F0(void) { ++clears; } /* profile_main_clear */

/* profile_read_into_struct (0x24010) overwrites the slot and leaves the
   profile's controller 1 settings at +0xf200 +0x1e. */
void recomp_dispatch_indirect_site(uint32_t guest_address, uint32_t saved_esp, const char *member, int line)
{
    (void)saved_esp; (void)member; (void)line;
    if (guest_address != 0x00024010u) return;
    const uint32_t esp = recomp_runtime.registers.esp;
    const uint32_t name = *recomp_memory_u32(esp + 4u), profile = *recomp_memory_u32(esp + 8u);
    if (reads < 2u && profile == SLOT) memcpy(read_names[reads], recomp_memory(name, 32u), 32u);
    memset(slot, 0xee, sizeof slot);
    memset(slot + 0xf200u + 0x1eu, (int)(0x10u + reads), 0x14u);
    recomp_runtime.registers.eax = reads == missing_read ? 1u : 0u;
    ++reads;
}

static int failures;
static void check(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static int all(const uint8_t *bytes, size_t size, uint8_t value)
{
    for (size_t i = 0u; i < size; ++i) if (bytes[i] != value) return 0;
    return 1;
}

static void clear_profile(const uint16_t *side0, const uint16_t *side1)
{
    recomp_netplay_guest_set_side_profiles(side0, side1);
    memset(slot, 0x5a, sizeof slot);
    memset(data, 0, sizeof data);
    memset(read_names, 0, sizeof read_names);
    clears = reads = 0u;
    recomp_runtime.registers.esp = STACK + 0x100u;
    RecompFunction function = recomp_netplay_guest_lookup_manual(0x000244f0u);
    if (function != NULL) function();
}

int main(void)
{
    static const uint16_t ann[] = {'A', 'n', 'n', 0u}, bob[] = {'B', 'o', 'b', 0u};
    check("hooks are off by default", recomp_netplay_guest_lookup_manual(0x0012b460u) == NULL &&
        recomp_netplay_guest_lookup_manual(0x000244f0u) == NULL);
    recomp_netplay_guest_set_fight_names("A", "", 0u);
    check("fight names hook the name draw", recomp_netplay_guest_lookup_manual(0x0012b460u) != NULL);
    recomp_netplay_guest_set_fight_names(NULL, NULL, 0u);
    check("no fight names unhook it", recomp_netplay_guest_lookup_manual(0x0012b460u) == NULL);

    clear_profile(ann, bob);
    check("each side's profile is read by name", clears == 1u && reads == 2u &&
        memcmp(read_names[0], ann, sizeof ann) == 0 && memcmp(read_names[1], bob, sizeof bob) == 0);
    check("the borrowed slot is put back", all(slot, sizeof slot, 0x5au));
    check("each port takes its side's layout", all(data + 0x1eu, 0x14u, 0x10u) && all(data + 0x32u, 0x14u, 0x11u));
    check("the rest of the profile keeps its defaults", all(data, 0x1eu, 0u) && all(data + 0x46u, 0xau, 0u));

    missing_read = 1u;
    clear_profile(ann, bob);
    missing_read = UINT32_MAX;
    check("a missing profile keeps the default layout",
        all(slot, sizeof slot, 0x5au) && all(data + 0x1eu, 0x14u, 0x10u) && all(data + 0x32u, 0x14u, 0u));

    clear_profile(NULL, bob);
    check("a side without a profile is not read", reads == 1u && memcmp(read_names[0], bob, sizeof bob) == 0 &&
        all(data + 0x1eu, 0x14u, 0u) && all(data + 0x32u, 0x14u, 0x10u));

    uint16_t split[17];
    for (unsigned i = 0u; i < 14u; ++i) split[i] = 'a';
    split[14] = 0xd83du; split[15] = 0xde00u; split[16] = 0u; /* a pair across the 15-unit limit */
    clear_profile(split, NULL);
    check("a long name stops before a split pair", reads == 1u && read_names[0][13] == 'a' && read_names[0][14] == 0u);

    clear_profile(NULL, NULL);
    check("no profiles unhook profile_main_clear", recomp_netplay_guest_lookup_manual(0x000244f0u) == NULL);
    return failures == 0 ? 0 : 1;
}
