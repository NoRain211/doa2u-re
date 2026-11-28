/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vs_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic guest RAM: the stand-ins for the game's functions record the
   modes they ran under, as the game's own code would read them. */
enum {
    STACK = 0x00f00000u,
    VERSUS_SLOT = 0x00df5d84u,
    SELECTED_MODE = 0x00df57b4u,
    TAG_CHOICE = 0x00df57bfu,
    GAME_MODE = 0x00878392u,
    BATTLE_MODE = 0x0087cfa0u,
    HANDOFF_PENDING = 0x00877ea8u,
    PLAYERS = 0x008783a8u,
    CPU_LEVEL = 0x0087d0d4u,
    HUMAN = 0x0087ae30u,
    PROFILE = 0x0087cfa8u,
    OPTIONS = 0x00100000u,
    STORY_LEVELS = 0x00692438u,
};

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address + width > RECOMP_XBOX_RAM_SIZE) abort();
    return recomp_fast_ram + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

static uint8_t *b(uint32_t address) { return recomp_memory(address, 1u); }
static uint32_t *u(uint32_t address) { return recomp_memory_u32_checked(address); }

static unsigned pads = 1u, slot_calls, setup_calls;
static uint32_t seen_mode, seen_game;

void sub_00127CE0(void) { recomp_runtime.registers.eax = pads; }
void sub_00134570(void) { ++slot_calls; *u(VERSUS_SLOT) = 0u; }
/* The Normal/Tag confirm: Versus refuses one pad, Sparring takes it. */
void sub_001389F0(void)
{
    seen_mode = *u(SELECTED_MODE);
    if (seen_mode == 5u && pads < 2u) {
        recomp_runtime.registers.eax = 0xffffffffu;
        return;
    }
    if (seen_mode == 6u) *b(GAME_MODE) = 9u;
    recomp_runtime.registers.eax = 3u;
}
void sub_00138E10(void) { seen_mode = *u(SELECTED_MODE); seen_game = *b(GAME_MODE); }
void sub_000A3EE0(void) { seen_mode = *u(SELECTED_MODE); }
/* The hand-off: one human makes the player flag 0. */
void sub_000D87C0(void)
{
    seen_game = *b(GAME_MODE);
    if (*b(HANDOFF_PENDING) != 1u) return;
    *b(BATTLE_MODE) = seen_game;
    *b(PLAYERS) = 0u;
}
void sub_000C6690(void) { ++setup_calls; }

static int failures;
static void expect(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static void call(uint32_t address)
{
    RecompFunction function = recomp_vs_cpu_lookup_manual(address);
    if (function == NULL) abort();
    function();
}

static void pick(uint32_t side, uint8_t character, uint8_t costume)
{
    *b(HUMAN + side * 0x94u + 1u) = character;
    *b(HUMAN + side * 0x94u + 2u) = costume;
}

int main(void)
{
    recomp_fast_ram = calloc(1u, RECOMP_XBOX_RAM_SIZE);
    if (recomp_fast_ram == NULL) return 1;
    recomp_runtime.registers.esp = STACK;
    *u(PROFILE) = OPTIONS;
    for (uint32_t i = 0u; i < 44u; ++i) *b(STORY_LEVELS + i) = (uint8_t)(10u + i);

    expect("other addresses", recomp_vs_cpu_lookup_manual(0x00134571u) == NULL);
    call(0x00134570u);
    expect("versus open", slot_calls == 1u && *u(VERSUS_SLOT) == 2u);

    /* Tag with one pad stays refused, as Versus did. */
    *u(SELECTED_MODE) = 5u;
    *b(TAG_CHOICE) = 1u;
    call(0x001389f0u);
    expect("tag refused", seen_mode == 5u && *u(SELECTED_MODE) == 5u);

    /* Normal with one pad confirms as Sparring. */
    *b(TAG_CHOICE) = 0u;
    call(0x001389f0u);
    expect("sparring select", seen_mode == 6u && *u(SELECTED_MODE) == 6u && *b(GAME_MODE) == 9u);
    call(0x000a3ee0u);
    expect("versus banner", seen_mode == 5u && *u(SELECTED_MODE) == 6u);

    /* The hand-off puts Versus back, with its player flag and a CPU level. */
    pick(0u, 3u, 1u);
    pick(1u, 7u, 2u);
    *b(OPTIONS) = 2u;
    *b(HANDOFF_PENDING) = 1u;
    call(0x000d87c0u);
    expect("versus fight", seen_game == 5u && *b(GAME_MODE) == 5u && *u(SELECTED_MODE) == 5u &&
        *b(PLAYERS) == 1u && *b(CPU_LEVEL) == 10u + 2u * 11u + 4u);

    /* A difficulty out of range leaves the CPU level as it was. */
    *b(OPTIONS) = 4u;
    *b(CPU_LEVEL) = 3u;
    *u(SELECTED_MODE) = 6u;
    *b(GAME_MODE) = 9u;
    call(0x000d87c0u);
    expect("bad difficulty", *b(PLAYERS) == 1u && *b(CPU_LEVEL) == 3u);
    *b(HANDOFF_PENDING) = 0u;

    /* AGAIN: the flag is 0 and the CPU side has another fighter. */
    *b(PLAYERS) = 0u;
    pick(1u, 20u, 0u);
    call(0x000c6690u);
    expect("again", setup_calls == 0u && *b(PLAYERS) == 1u && *b(HUMAN + 0x94u + 1u) == 7u &&
        *b(HUMAN + 0x94u + 2u) == 2u && *b(HUMAN + 1u) == 3u);

    /* CHARACTER: the select runs as Sparring again. */
    call(0x00138e10u);
    expect("character", seen_game == 9u && seen_mode == 6u);

    /* Back to the Normal/Tag screen with two pads: plain Versus. */
    pads = 2u;
    call(0x001389f0u);
    expect("two pads", seen_mode == 5u && *u(SELECTED_MODE) == 5u);
    call(0x000c6690u);
    expect("setup untouched", setup_calls == 1u);

    /* Real Sparring is left alone. */
    *u(SELECTED_MODE) = 6u;
    call(0x000a3ee0u);
    expect("sparring banner", seen_mode == 6u);

    free(recomp_fast_ram);
    return failures == 0 ? 0 : 1;
}
