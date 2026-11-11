/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu_costume.h"
#include "kernel_abi.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* battle_get_fighter_costume (0xc6b00, cdecl character, opponent index)
   gives each CPU opponent its costume in Story (mode 3), Time Attack (4),
   Survival (6) and Team (7). Other callers ask again during a run, so a
   random pick is keyed by the run's opponent lists and stays the same. Only
   the game's plain picks change: costume 1, or costume 2 when the player
   wears costume 1. Story's own picks (a costume about to unlock, Kasumi's
   clone) stay. */
enum {
    FIGHTER_COSTUME = 0x000c6b00u,
    COSTUME_ID = 0x0011e570u,        /* cdecl (character, group, column, side) */
    UNLOCKED_GROUPS = 0x0011e410u,   /* cdecl (character, side) */
    SAME_COSTUME = 0x000c5490u,      /* cdecl (character, costume, character, costume) */
    BATTLE_MODE = 0x0087cfa0u,
    TAG_MATCH = 0x0087d49du,
    SIDE_SETUP = 0x0087ae30u,        /* +1 character, +2 costume; sides 0x94 apart */
    TAG_CHARACTERS = 0x0087ae0cu,    /* two per side, seven apart */
    TAG_COSTUMES = 0x0087d24cu,
    OPPONENT_LIST = 0x008a3a08u,     /* seven opponents */
    TEAM_LIST = 0x008a3a24u,         /* opponent pairs */
};

extern void sub_000C6B00(void);

static bool enabled, online_session;
static uint32_t seed;

void recomp_cpu_costume_install(bool online)
{
    const char *value = getenv("RECOMP_RANDOM_CPU_COSTUMES");
    online_session = online;
    enabled = !online && value != NULL && strcmp(value, "1") == 0;
    struct timespec now;
    timespec_get(&now, TIME_UTC); /* nanoseconds, so quick reboots still differ */
    seed = ((uint32_t)now.tv_sec * 2654435761u ^ (uint32_t)now.tv_nsec) | 1u;
}

static uint32_t hash_bytes(uint32_t hash, const uint8_t *bytes, uint32_t size)
{
    for (uint32_t i = 0u; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u; /* FNV-1a */
    return hash;
}

/* The same run, character and index give the same group, so every lookup in a
   run agrees. */
static uint8_t run_group(uint8_t character, uint8_t index, uint8_t groups)
{
    uint32_t hash = 2166136261u ^ seed;
    hash = hash_bytes(hash, recomp_memory(OPPONENT_LIST, 7u), 7u);
    hash = hash_bytes(hash, recomp_memory(TEAM_LIST, 16u), 16u);
    const uint8_t key[2] = {character, index};
    hash = hash_bytes(hash, key, 2u);
    return groups == 0u ? 0u : (uint8_t)(hash % groups);
}

static uint8_t call_byte(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d, unsigned count)
{
    const uint32_t arguments[4] = {a, b, c, d};
    const uint32_t eax = recomp_runtime.registers.eax; /* the game's pick, which every return keeps */
    kernel_call_guest(function, arguments, count);
    const uint8_t value = (uint8_t)recomp_runtime.registers.eax;
    recomp_runtime.registers.eax = eax;
    return value;
}

static bool wears_player_costume(uint8_t character, uint8_t costume)
{
    const uint32_t side = *recomp_memory(SIDE_SETUP, 1u) != 1u;
    if (*recomp_memory(TAG_MATCH, 1u) == 0u) {
        return call_byte(SAME_COSTUME, character, costume, *recomp_memory(SIDE_SETUP + side * 0x94u + 1u, 1u),
            *recomp_memory(SIDE_SETUP + side * 0x94u + 2u, 1u), 4u) != 0u;
    }
    for (uint32_t i = 0u; i < 2u; ++i) {
        if (call_byte(SAME_COSTUME, character, costume, *recomp_memory(TAG_CHARACTERS + side * 7u + i, 1u),
                *recomp_memory(TAG_COSTUMES + side * 7u + i, 1u), 4u) != 0u) return true;
    }
    return false;
}

void recomp_cpu_costume_set_enabled(bool on)
{
    enabled = !online_session && on;
}

bool recomp_cpu_costume_enabled(void)
{
    return enabled;
}

static void fighter_costume(void)
{
    if (!enabled) {
        sub_000C6B00();
        return;
    }
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint8_t character = *recomp_memory(entry + 4u, 1u), index = *recomp_memory(entry + 8u, 1u);
    sub_000C6B00();
    const uint32_t result = recomp_runtime.registers.eax;
    const uint8_t mode = *recomp_memory(BATTLE_MODE, 1u);
    if (mode != 3u && mode != 4u && mode != 6u && mode != 7u) return;
    const uint8_t picked = (uint8_t)result;
    if (picked != call_byte(COSTUME_ID, character, 0u, 0u, 0u, 4u) &&
        picked != call_byte(COSTUME_ID, character, 1u, 0u, 0u, 4u)) return;
    const uint8_t groups = call_byte(UNLOCKED_GROUPS, character, 0u, 0u, 0u, 2u);
    const uint8_t first = run_group(character, index, groups);
    for (uint8_t step = 0u; step < groups; ++step) {
        const uint8_t costume = call_byte(COSTUME_ID, character, (uint8_t)((first + step) % groups), 0u, 0u, 4u);
        if (!wears_player_costume(character, costume)) {
            recomp_runtime.registers.eax = (result & ~0xffu) | costume;
            return;
        }
    }
}

RecompFunction recomp_cpu_costume_lookup_manual(uint32_t guest_address)
{
    /* Routed whenever it may be wanted, so a cached lookup still sees a later switch. */
    return !online_session && guest_address == FIGHTER_COSTUME ? fighter_costume : NULL;
}
