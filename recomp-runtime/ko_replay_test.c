/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ko_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void expect(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
enum { LOW = 0x00690000u, HIGH = 0x00a95000u, STACK = 0x00700000u };
static uint8_t memory[HIGH - LOW];
uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address < LOW || address + width > HIGH) {
        fprintf(stderr, "unexpected read %08x\n", address);
        abort();
    }
    return memory + (address - LOW);
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)(void *)recomp_memory(address, 4u); }

static void put8(uint32_t at, unsigned v) { *recomp_memory(at, 1u) = (uint8_t)v; }
static void put16(uint32_t at, unsigned v) { uint16_t x = (uint16_t)v; memcpy(recomp_memory(at, 2u), &x, 2u); }
static void put32(uint32_t at, uint32_t v) { memcpy(recomp_memory(at, 4u), &v, 4u); }
static unsigned get8(uint32_t at) { return *recomp_memory(at, 1u); }
static unsigned get16(uint32_t at) { uint16_t x; memcpy(&x, recomp_memory(at, 2u), 2u); return x; }
static uint32_t get32(uint32_t at) { uint32_t x; memcpy(&x, recomp_memory(at, 4u), 4u); return x; }
static void putf(uint32_t at, float v) { memcpy(recomp_memory(at, 4u), &v, 4u); }
static float getf(uint32_t at) { float v; memcpy(&v, recomp_memory(at, 4u), 4u); return v; }

/* The game's functions, as far as the replay needs them. */
static unsigned poses, follows, follows_arg, steps_original, frames, round_end_original;
void sub_000C4950(void) { ++steps_original; recomp_runtime.registers.eax = 1u; recomp_runtime.registers.esp += 4u; }
void sub_001106C0(void) { ++poses; recomp_runtime.registers.esp += 4u; }
void sub_000CB2A0(void) { ++follows; follows_arg = get32(recomp_runtime.registers.esp + 4u); recomp_runtime.registers.esp += 4u; }
void sub_000CFA20(void) { recomp_runtime.registers.eax = 0u; recomp_runtime.registers.esp += 4u; }
void sub_000D0710(void) { recomp_runtime.registers.esp += 4u; }
void sub_000D9D20(void) { recomp_runtime.registers.esp += 4u; }
void sub_000D9D80(void) { recomp_runtime.registers.esp += 4u; }
void sub_000CF900(void) { ++round_end_original; recomp_runtime.registers.esp += 4u; }
/* A frame passes: the runner's swap. */
void sub_000FA3D0(void) { ++frames; recomp_ko_replay_frame(); recomp_runtime.registers.esp += 4u; }

/* As 0xcc650 in mode 1: what it sees, then its passes through the pass step,
   each starting from its tables, then its end (pose, what follows). */
static unsigned task_kind, task_runs, task_mode, task_stage, task_human, task_winner, task_move, passes_seen, shown_at_ko;
static float pass_starts[4], pass_end_max;
void sub_000CC650(void)
{
    ++task_runs;
    const uint8_t winner = get8(0x0087d23au);
    task_winner = winner;
    task_human = get8(0x0087ae30u + 0x94u * winner);
    task_move = get16(0x0087d564u);
    task_stage = get32(0x008a3968u);
    put8(0x00878394u, (uint8_t)task_stage);
    task_kind = get8(0x00877cb4u);
    task_mode = task_human == 1u && task_move == 270u && task_kind != 3u ? 1u : 0u;
    put32(0x008a38d0u, task_mode);
    unsigned repeat = 0u;
    putf(0x0087d25cu, getf(0x0069338cu) * 60.0f + 180.0f); /* KO kind 1 */
    pass_starts[0] = getf(0x0087d25cu);
    for (unsigned frame = 0u; frame < 600u; ++frame) {
        recomp_runtime.registers.ecx = repeat;
        recomp_runtime.registers.esp -= 4u;
        recomp_ko_replay_lookup_manual(0x000c4950u)();
        if (recomp_runtime.registers.eax == 1u) {
            ++repeat;
            putf(0x0087d25cu, getf(0x0069338cu + 4u * (repeat < 3u ? repeat : 2u)) * 60.0f + 180.0f);
            if (repeat < 4u) pass_starts[repeat] = getf(0x0087d25cu);
        }
        if ((unsigned)getf(0x0087d25cu) == 299u) ++shown_at_ko;
        if (repeat < 2u && getf(0x0087d25cu) > pass_end_max) pass_end_max = getf(0x0087d25cu);
        ++frames;
        recomp_ko_replay_frame();
        if (getf(0x0087d25cu) >= 299.0f && recomp_runtime.registers.eax == 0u) break;
    }
    passes_seen = repeat + 1u;
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x001106c0u)();
    const uint32_t arg = 7u;
    recomp_runtime.registers.esp -= 4u;
    memcpy(recomp_memory(recomp_runtime.registers.esp, 4u), &arg, 4u);
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000cb2a0u)();
    recomp_runtime.registers.esp += 4u;
    put32(0x008a3968u, 5u);
    recomp_runtime.registers.esp += 4u;
}

int main(void)
{
    put32(0x00692ef8u + 4u * 5u, 0x00692e04u);
    put16(0x00692e04u, 270u);
    put16(0x00692e06u, 0xffffu);
    put32(0x00692ef8u + 4u * 10u, 0x00692d80u);
    put16(0x00692d80u, 0xffffu);
    expect("listed move", recomp_ko_replay_listed_move(5u) == 270 && recomp_ko_replay_listed_move(10u) == -1 &&
        recomp_ko_replay_listed_move(200u) == -1);

    /* Tables as the game has them. */
    putf(0x0069338cu, -0.5f); putf(0x00693390u, -0.4f); putf(0x00693394u, -0.3f);
    putf(0x00693398u, -0.1f); putf(0x0069339cu, 0.0f); putf(0x006933a0u, 0.0f);

    /* The timing file. */
    RecompKoReplayTiming timing;
    recomp_ko_replay_default_timing(&timing);
    expect("defaults", timing.cut == 16u && timing.before == 8u && timing.after == 13u && timing.last_before == 10u &&
        timing.shots == 3u && timing.speed == 1.0f && timing.last_speed == 0.85f);
    recomp_ko_replay_install(false);
    recomp_runtime.registers.esp = STACK;
    recomp_runtime.registers.ebx = 0x1234u;
    put16(0x008a7eecu, 300u);
    put8(0x00878394u, 1u); /* the stage copy lags: the fight is ROUND_STAGE 3 */
    put32(0x008a3968u, 1u);
    /* The round's end task (slot 1) starts the round: its slot is learnt. */
    put32(0x00a94814u, 1u);
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000cf900u)();
    expect("the round's end otherwise the game's", round_end_original == 1u && recomp_runtime.registers.esp == STACK);
    put32(0x008a3968u, 3u);
    /* The CPU (side 1, Kasumi) KOs side 0 with an unlisted move. */
    put8(0x00877c95u, 5u);
    put16(0x0087d564u, 999u);
    put8(0x0087d23au, 2u);
    put8(0x0087ae30u + 0x94u, 0u);
    put8(0x00877cb4u, 1u);
    put8(0x0087ae08u, 1u);           /* replays on */
    put16(0x0087ae54u + 0x94u, 50u); /* the winner standing */
    put16(0x008a7ef8u, 298u);
    recomp_ko_replay_note_ko(0u);
    const uint32_t one = 1u;
    for (unsigned i = 0u; i < 15u; ++i) { /* fifteen frames recorded past the KO */
        put16(0x008a7ef8u, (299u + i) % 300u);
        recomp_runtime.registers.esp -= 4u;
        memcpy(recomp_memory(recomp_runtime.registers.esp, 4u), &one, 4u);
        recomp_runtime.registers.esp -= 4u;
        recomp_ko_replay_lookup_manual(0x000fa3d0u)();
        recomp_runtime.registers.esp += 4u;
    }
    expect("not yet", task_runs == 0u);
    put16(0x008a7ef8u, 14u); /* the sixteenth, the ring wrapping */
    /* Another task's wait: not there. */
    put32(0x00a94814u, 2u);
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000fa3d0u)();
    expect("only in the round's end task", task_runs == 0u);
    put32(0x00a94814u, 1u);
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000fa3d0u)();
    expect("the replay runs as the round's end waits", task_runs == 1u && recomp_runtime.registers.esp == STACK);
    expect("the task saw stage 4, a human winner and a listed move",
        task_stage == 4u && task_winner == 1u && task_human == 1u && task_move == 270u && task_mode == 1u);
    expect("three passes, each from its lead to the hit",
        passes_seen == 3u && pass_starts[0] == 275.0f && pass_starts[1] == 275.0f && pass_starts[2] == 273.0f);
    expect("the first passes end 13 past the hit", pass_end_max == 296.0f);
    expect("the last pass slowed", shown_at_ko >= 1u && steps_original == 0u);
    expect("the task's end kept back", poses == 0u && follows == 0u);
    expect("back to the fight", get32(0x008a3968u) == 3u && get8(0x00878394u) == 1u &&
        get8(0x0087ae30u + 0x94u) == 0u && get8(0x0087d23au) == 2u && getf(0x0069338cu) == -0.5f &&
        getf(0x00693398u) == -0.1f && recomp_runtime.registers.ebx == 0x1234u);

    /* The round's end at its replay stage: no second replay, the task's end done. */
    put8(0x0087d23au, 1u);
    put32(0x008a3968u, 4u);
    const unsigned runs_before = task_runs;
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000cf900u)();
    expect("no second replay, the task's end done", task_runs == runs_before && poses == 1u && follows == 1u &&
        follows_arg == 0u && get32(0x008a3968u) == 5u && round_end_original == 1u && recomp_runtime.registers.esp == STACK);

    /* The next round's end runs as the game's. */
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000cf900u)();
    expect("then the game's again", round_end_original == 2u);

    unsigned ring_round = 10u; /* each case below starts its KO further on in the ring */
    /* Through a fence (KO kind 3): no replay at all, the round moves on. */
    put32(0x008a3968u, 3u);
    put8(0x00877cb4u, 3u);
    put16(0x008a7ef8u, 3u + ring_round);
    recomp_ko_replay_note_ko(0u);
    for (unsigned i = 1u; i <= 17u; ++i) {
        put16(0x008a7ef8u, (3u + i) % 300u + ring_round);
        recomp_runtime.registers.esp -= 4u;
        recomp_ko_replay_lookup_manual(0x000fa3d0u)();
    }
    ring_round += 10u;
    expect("a stage KO not replayed by us", task_runs == runs_before);
    put32(0x008a3968u, 4u);
    recomp_runtime.registers.esp -= 4u;
    recomp_ko_replay_lookup_manual(0x000cf900u)();
    expect("nor by the game", task_runs == runs_before && poses == 2u && round_end_original == 2u &&
        get32(0x008a3968u) == 5u && recomp_runtime.registers.esp == STACK);
    put8(0x00877cb4u, 1u);

    const unsigned runs_after = task_runs;

    /* A double KO: no replay, as the game. */
    put32(0x008a3968u, 3u);
    put16(0x0087ae54u + 0x94u, 0u);
    put16(0x008a7ef8u, 3u + ring_round);
    recomp_ko_replay_note_ko(0u);
    for (unsigned i = 1u; i <= 17u; ++i) {
        put16(0x008a7ef8u, (3u + i) % 300u + ring_round);
        recomp_runtime.registers.esp -= 4u;
        recomp_ko_replay_lookup_manual(0x000fa3d0u)();
    }
    ring_round += 10u;
    expect("double KO not replayed", task_runs == runs_after);
    put16(0x0087ae54u + 0x94u, 50u);

    /* Original: the game's own replay. */
    recomp_ko_replay_set_enabled(false);
    put16(0x008a7ef8u, 3u + ring_round);
    recomp_ko_replay_note_ko(0u);
    for (unsigned i = 1u; i <= 17u; ++i) {
        put16(0x008a7ef8u, (3u + i) % 300u + ring_round);
        recomp_runtime.registers.esp -= 4u;
        recomp_ko_replay_lookup_manual(0x000fa3d0u)();
    }
    ring_round += 10u;
    expect("original left alone", task_runs == runs_after);

    if (failures == 0) puts("ko replay tests passed");
    return failures != 0;
}
