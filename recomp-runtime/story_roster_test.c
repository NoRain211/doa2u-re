/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "story_roster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t pool[0x14];       /* 0x692b84, up to the next list */
static uint32_t game_data;       /* 0x87cfa8 */
static uint8_t flags[2];         /* game data +0x2ed3 (Bayman), +0x2ed4 (Hitomi) */
static unsigned setups;

enum { DATA = 0x00100000u };

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x00692b84u && address + width <= 0x00692b84u + sizeof pool) return &pool[address - 0x00692b84u];
    if (address == 0x0087cfa8u && width <= 4u) return (uint8_t *)&game_data;
    if (address >= DATA + 0x2ed3u && address + width <= DATA + 0x2ed3u + sizeof flags)
        return &flags[address - DATA - 0x2ed3u];
    abort();
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void sub_000C6D40(void) { ++setups; }

static int fails;
static void expect(const char *name, bool ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++fails;
    }
}

/* A stand-in pool: twelve ids starting with 0, the marker and its padding,
   then the start of the next list. */
static uint8_t start[0x14];

static bool setup_ends(bool hitomi, bool bayman, const uint8_t want[4])
{
    flags[1] = hitomi ? 1u : 0u;
    flags[0] = bayman ? 1u : 0u;
    const unsigned before = setups;
    recomp_story_roster_lookup_manual(0x000c6d40u)();
    return setups == before + 1u && memcmp(&pool[12], want, 4u) == 0 && memcmp(pool, start, 12u) == 0
        && memcmp(&pool[16], &start[16], 4u) == 0;
}

int main(void)
{
    expect("other addresses", recomp_story_roster_lookup_manual(0x000c6d41u) == NULL);
    for (unsigned i = 1u; i < 12u; ++i) start[i] = (uint8_t)(0x30u + i);
    start[12] = 0xffu;
    for (unsigned i = 16u; i < sizeof start; ++i) start[i] = (uint8_t)(0x40u + i);
    memcpy(pool, start, sizeof pool);
    game_data = DATA;
    expect("both locked", setup_ends(false, false, (const uint8_t[4]){0xff, 0, 0, 0}));
    expect("Hitomi", setup_ends(true, false, (const uint8_t[4]){0x15, 0xff, 0, 0}));
    expect("both", setup_ends(true, true, (const uint8_t[4]){0x15, 0x18, 0xff, 0}));
    expect("Bayman", setup_ends(false, true, (const uint8_t[4]){0x18, 0xff, 0, 0}));
    expect("locked again", setup_ends(false, false, (const uint8_t[4]){0xff, 0, 0, 0}));
    flags[0] = flags[1] = 1u;
    game_data = 0u;
    expect("no game data", setup_ends(true, true, (const uint8_t[4]){0xff, 0, 0, 0}));
    game_data = DATA;
    pool[13] = 0x07u; /* not a layout the disc or this hook wrote */
    expect("foreign layout kept", setup_ends(true, true, (const uint8_t[4]){0xff, 0x07, 0, 0}));
    return fails == 0 ? 0 : 1;
}
