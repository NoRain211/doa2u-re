/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cpu_costume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthetic guest memory for the Story costume write-back. The game's plain
   pick is group 0; a random pick from one unlocked group is the same value,
   so the array write is checked against a sentinel. */

enum {
    STACK = 0x00100000u,
    BATTLE_MODE = 0x0087cfa0u,
    TAG_MATCH = 0x0087d49du,
    SIDE_SETUP = 0x0087ae30u,
    SIDE_STRIDE = 0x94u,
    TAG_CHARACTERS = 0x0087ae0cu,
    TAG_COSTUMES = 0x0087d24cu,
    OPPONENT_LIST = 0x008a3a08u,
    TEAM_LIST = 0x008a3a24u,
    STORY_COSTUMES = 0x008a393cu,
    FIGHTER_COSTUME = 0x000c6b00u,
    COSTUME_ID = 0x0011e570u,
    UNLOCKED_GROUPS = 0x0011e410u,
    SAME_COSTUME = 0x000c5490u,
};

static int failures;
static void expect(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram; /* NULL: every access goes through recomp_memory */

static uint8_t stack[0x200]; /* at 0x100000 */
static uint8_t battle_mode;   /* at 0x87cfa0 */
static uint8_t tag_match;     /* at 0x87d49d */
static uint8_t side_setup[2u * SIDE_STRIDE]; /* at 0x87ae30 */
static uint8_t tag_characters[2u * 7u];      /* at 0x87ae0c */
static uint8_t tag_costumes[2u * 7u];        /* at 0x87d24c */
static uint8_t opponents[7];                 /* at 0x8a3a08 */
static uint8_t team[16];                     /* at 0x8a3a24 */
static uint8_t story[8];                     /* at 0x8a393c, one spare byte */
static uint8_t unlocked_groups;
static uint32_t base_result;     /* what battle_get_fighter_costume returns */
static uint32_t same_result;     /* what SAME_COSTUME returns */

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= STACK && address + width <= STACK + sizeof stack) return &stack[address - STACK];
    if (address == BATTLE_MODE && width == 1u) return &battle_mode;
    if (address == TAG_MATCH && width == 1u) return &tag_match;
    if (address >= SIDE_SETUP && address + width <= SIDE_SETUP + sizeof side_setup)
        return &side_setup[address - SIDE_SETUP];
    if (address >= TAG_CHARACTERS && address + width <= TAG_CHARACTERS + sizeof tag_characters)
        return &tag_characters[address - TAG_CHARACTERS];
    if (address >= TAG_COSTUMES && address + width <= TAG_COSTUMES + sizeof tag_costumes)
        return &tag_costumes[address - TAG_COSTUMES];
    if (address >= OPPONENT_LIST && address + width <= OPPONENT_LIST + sizeof opponents)
        return &opponents[address - OPPONENT_LIST];
    if (address >= TEAM_LIST && address + width <= TEAM_LIST + sizeof team)
        return &team[address - TEAM_LIST];
    if (address >= STORY_COSTUMES && address + width <= STORY_COSTUMES + sizeof story)
        return &story[address - STORY_COSTUMES];
    fprintf(stderr, "unexpected guest access %08x\n", address);
    abort();
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

static uint32_t arg8(unsigned index)
{
    return *recomp_memory(recomp_runtime.registers.esp + 4u * index, 1u);
}

/* As battle_get_fighter_costume: returns the game's pick. The hook is the
   caller; this stub pops the return address like the lifted function does. */
void sub_000C6B00(void)
{
    recomp_runtime.registers.eax = base_result;
    recomp_runtime.registers.esp += 4u;
}

/* The guest calls the hook makes: costumes from an unlocked group, the group
   count and the player-costume comparison. */
void recomp_dispatch_indirect_site(
    uint32_t guest_address,
    uint32_t saved_esp,
    const char *member,
    int line)
{
    (void)saved_esp;
    (void)member;
    (void)line;
    if (guest_address == COSTUME_ID) {
        const uint32_t character = arg8(1u), group = arg8(2u);
        recomp_runtime.registers.eax = 0x40u + 0x20u * character + group;
    } else if (guest_address == UNLOCKED_GROUPS) {
        recomp_runtime.registers.eax = unlocked_groups;
    } else if (guest_address == SAME_COSTUME) {
        recomp_runtime.registers.eax = same_result;
    } else {
        fprintf(stderr, "unexpected guest call %08x\n", guest_address);
        abort();
    }
}

static uint8_t plain(uint8_t character) { return (uint8_t)(0x40u + 0x20u * character); }

/* Each case starts from a Story bout with no write-back yet. */
static void begin(void)
{
    memset(stack, 0, sizeof stack);
    memset(side_setup, 0, sizeof side_setup);
    memset(tag_characters, 0, sizeof tag_characters);
    memset(tag_costumes, 0, sizeof tag_costumes);
    memset(team, 0, sizeof team);
    memset(story, 0xff, sizeof story);
    battle_mode = 3u;
    tag_match = 0u;
    unlocked_groups = 1u;
    base_result = 0u;
    same_result = 0u;
    for (unsigned i = 0u; i < sizeof opponents / sizeof opponents[0]; ++i) opponents[i] = (uint8_t)(i + 1u);
    side_setup[1] = 0x0fu;
    side_setup[2] = 0x0fu;
    side_setup[SIDE_STRIDE + 1u] = 0x0eu;
    side_setup[SIDE_STRIDE + 2u] = 0x0eu;
    recomp_cpu_costume_set_online(false);
    recomp_cpu_costume_set_enabled(true);
}

static uint32_t call_costume(uint8_t character, uint8_t index)
{
    recomp_runtime.registers.esp = STACK + 0x100u;
    *recomp_memory_u32(STACK + 0x100u) = 0xdeadbeefu; /* return address */
    *recomp_memory_u32(STACK + 0x104u) = character;
    *recomp_memory_u32(STACK + 0x108u) = index;
    RecompFunction function = recomp_cpu_costume_lookup_manual(FIGHTER_COSTUME);
    if (function == NULL) {
        fprintf(stderr, "no costume hook\n");
        abort();
    }
    function();
    if (recomp_runtime.registers.esp != STACK + 0x104u) {
        fprintf(stderr, "FAIL hook leaves ESP %08x\n", recomp_runtime.registers.esp);
        ++failures;
    }
    return recomp_runtime.registers.eax & 0xffu;
}

int main(void)
{
    const uint8_t character = 2u, index = 3u, plain_pick = plain(character);

    recomp_cpu_costume_install(false); /* seed once: every call shares the run */

    /* A plain Story pick is replaced and written into the opponent array. */
    begin();
    base_result = plain_pick;
    opponents[index] = character;
    expect("plain pick is written back",
        call_costume(character, index) == plain_pick && story[index] == plain_pick);

    /* The game now returns the array's value; a repeat keeps it. */
    base_result = story[index];
    expect("a repeat keeps the stored pick",
        call_costume(character, index) == plain_pick && story[index] == plain_pick);

    /* A group beyond the first two stays: Story's own pick, or a random pick
       an earlier call wrote back. The hook leaves it and a repeat matches. */
    begin();
    base_result = (uint32_t)plain(character) + 2u;
    opponents[index] = character;
    story[index] = (uint8_t)base_result;
    expect("Story's own pick stays",
        call_costume(character, index) == base_result && story[index] == (uint8_t)base_result);

    /* Modes 4, 6 and 7 have no Story array to keep in step. */
    for (unsigned i = 0u; i < 3u; ++i) {
        static const uint8_t modes[3] = {4u, 6u, 7u};
        begin();
        battle_mode = modes[i];
        base_result = plain_pick;
        opponents[index] = character;
        expect("another mode does not write",
            call_costume(character, index) == plain_pick && story[index] == 0xffu);
    }

    /* Only the seven Story bouts have an array entry. */
    begin();
    base_result = plain_pick;
    expect("an eighth bout does not write",
        call_costume(character, 7u) == plain_pick && story[7] == 0xffu);

    /* The array belongs to the opponent in that bout. */
    begin();
    base_result = plain_pick;
    opponents[index] = (uint8_t)(character + 1u);
    expect("another character's bout does not write",
        call_costume(character, index) == plain_pick && story[index] == 0xffu);

    /* Default and online keep the game's own pick. */
    begin();
    recomp_cpu_costume_set_enabled(false);
    base_result = plain_pick;
    opponents[index] = character;
    expect("the default setting does not write",
        call_costume(character, index) == plain_pick && story[index] == 0xffu);
    begin();
    recomp_cpu_costume_set_online(true);
    base_result = plain_pick;
    opponents[index] = character;
    expect("online does not write",
        call_costume(character, index) == plain_pick && story[index] == 0xffu);

    if (failures == 0) puts("cpu costume tests passed");
    return failures != 0;
}
