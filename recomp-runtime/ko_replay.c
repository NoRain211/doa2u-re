/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ko_replay.h"
#include "kernel_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DOA2U's KO replay. The fight runs in a task (0x11a7d0) that, each frame,
   records the fighters into a snapshot ring (the last RING_LENGTH frames,
   0x8a7eec, written at RING_WRITE, 0x8a7ef8) and lands damage (0x119420).
   The round's end is a state machine (0xcf900) stepping through ROUND_STAGE
   (0x8a3968) by a jump table: 1 0xce5b0 sets the round up, 3 0xcbe80 is
   the fight (its stage clock runs the KO's slow motion, about two seconds,
   while the fight task records the fall), 4 0xcc650 is the replay, 5 0xce8e0
   follows it. Some stage functions copy ROUND_STAGE to STAGE (0x878394) as
   they begin, which the rest of the game reads (4: the replay is shown); in
   the fight it may still hold an earlier stage, so ROUND_STAGE is what tells
   the fight.

   The replay task (0xcc650) plays the ring back, POSITION (0x87d25c) the
   frame shown (0 oldest .. 299 newest). It first picks a mode
   (REPLAY_MODE, 0x8a38d0): 0 plays the ring once from 59.75; 1 is the
   multi-angle replay, in passes, each from a camera of its own, the pass
   step 0xc4950 (repeat in ECX) adding 1 a frame and returning 1 at the
   pass's end, the task then starting the next pass and its camera. It picks
   1 only when the winner (WINNER, 0x87d23a) is human (HUMAN, 0x87ae30 +
   0x94 per side, 1) and the KO move (KO_MOVE, 0x87d564) is on the
   attacker's list (0x692ef8 indexed by KO_CHARACTER, 0x877c95: 16-bit move
   numbers ending 0xffff), and not for some kinds of KO (0x877cb4 3). At its
   end it picks the winner's pose (0x1106c0) and starts what follows
   (0xcb2a0), and sets ROUND_STAGE to 5.

   The world update (0xe6710, its own task) ends each frame in 0xe8250:
   when STAGE turns 4 it readies the playback (0xe7a70), while it is 4 it
   shows the ring instead of running the fight, and when it leaves 4 it puts
   the live fight back (0xe7b40); otherwise it records the frame.

   Dynamic replays the KO at once, in the manner of Tekken Tag Tournament,
   then lets the fall play out live:
   - Once the fight has recorded `cut` frames past the KO, as the round's end task (its slot,
     learnt at 0xcf900) waits a frame (0xfa3d0) in the fight stage, it runs
     the replay task there itself: the round's end, and so the KO's slow
     motion, waits on it, while the world plays the ring. It sets the stage
     to 4 for it, the winner, a listed KO move and the winner human (so the
     task picks the multi-angle mode), and keeps the task's end (the pose,
     what follows) from running.
   - A KO of the stage's (KO_KIND 3: hazards, fences, falls into another
     area, which the multi-angle mode never takes and some stages never
     replay) gets no replay at all: neither ours nor the game's.
   - Its pass step times the passes as Tekken Tag Tournament's replay does
     (measured from footage at 60 frames a second; RecompKoReplayTiming): the cut comes `cut` frames past the KO; the earlier
     passes show `before` frames up to the hit and `after` past it, the last
     `last_before` up to it and on to the newest frame, each from the next
     camera.
   - Then the stages and the winner's byte go back: the world puts the
     fight back and the fall plays live as the round's end goes on. When it
     reaches its replay stage it skips the game's replay, doing only what its
     end does (0xcc650 from 0xccd28): stage 5, the winner's pose, what
     follows.
   The task starts each pass at a frame from its tables (START_TABLES): the
   passes' start is written there while ours runs, so no pass begins with a
   frame of the wrong moment. */
enum {
    RING_WRITE = 0x008a7ef8u,
    RING_LENGTH = 0x008a7eecu,
    RING_HELD = 0x008a7f00u,
    POSITION = 0x0087d25cu,
    STAGE = 0x00878394u,
    ROUND_STAGE = 0x008a3968u,
    REPLAY_MODE = 0x008a38d0u,
    WINNER = 0x0087d23au,
    HUMAN = 0x0087ae30u,
    SIDE_STRIDE = 0x94u,
    KO_MOVE = 0x0087d564u,
    KO_CHARACTER = 0x00877c95u,
    MOVE_LISTS = 0x00692ef8u,

    START_TABLES = 0x0069338cu,  /* 3 floats: start = t * 60 + 180 (KO kind not 0) */
    START_TABLES_ZERO = 0x00693398u, /* 3 floats: start = t * 60 + 180 - SINCE (kind 0) */
    SINCE = 0x0087d490u,
    KO_KIND = 0x00877cb4u,
    SHOWN = 0x0087ae2cu,
    GAME_MODE = 0x0087cfa0u,
    PASS_STEP = 0x000c4950u,
    ROUND_END = 0x000cf900u,
    WINNER_POSE = 0x001106c0u,
    WHAT_FOLLOWS = 0x000cb2a0u,
    WAIT_FRAMES = 0x000fa3d0u,
    CURRENT_TASK = 0x00a94814u,

    FIGHT_STAGE = 3u,
    REPLAY_STAGE = 4u,
    PASSES = 3u,            /* the most the multi-angle mode has cameras for */
    CHARACTERS = 25u,
};


extern void sub_000C4950(void);
extern void sub_000CC650(void);
extern void sub_000CF900(void);
extern void sub_001106C0(void);
extern void sub_000CB2A0(void);
extern void sub_000FA3D0(void);
extern void sub_000CFA20(void);
extern void sub_000D0710(void);
extern void sub_000D9D20(void);
extern void sub_000D9D80(void);

static struct {
    bool enabled, online;
    bool pending;             /* a KO this round, its replay to come */
    unsigned loser;
    uint16_t ko_write;        /* the ring's write index at the KO */
    bool nested;              /* our replay is running */
    float starts[PASSES], ends[PASSES]; /* each pass's frames, the last's end 299 */
    int last_repeat;
    bool replayed;            /* ours ran this round */
    RecompKoReplayTiming timing;
    int stage_task;           /* the round's end task's slot, or -1 */
    uint16_t last_held;
} state = {.enabled = true, .stage_task = -1, .timing = {16u, 8u, 13u, 10u, 3u, 1.0f, 0.85f}};

void recomp_ko_replay_default_timing(RecompKoReplayTiming *timing)
{
    *timing = (RecompKoReplayTiming){16u, 8u, 13u, 10u, 3u, 1.0f, 0.85f};
}

int recomp_ko_replay_listed_move(uint8_t character)
{
    if (character >= CHARACTERS) return -1;
    const uint32_t list = *recomp_memory_u32(MOVE_LISTS + 4u * character);
    if (list == 0u) return -1;
    const uint16_t move = *recomp_memory_u16(list);
    return move == 0xffffu ? -1 : (int)move;
}

static float position(void)
{
    float value;
    memcpy(&value, recomp_memory(POSITION, 4u), 4u);
    return value;
}

static void set_position(float value) { memcpy(recomp_memory(POSITION, 4u), &value, 4u); }

void recomp_ko_replay_note_ko(unsigned side)
{
    if (side > 1u || !state.enabled || state.online) return;
    if (state.replayed || state.nested) return;
    fprintf(stderr, "recomp ko replay: side %u KO'd (cut %u, shots %u: %u/%u at %.0f%%, last %u at %.0f%%)\n", side,
        state.timing.cut, state.timing.shots, state.timing.before, state.timing.after, (double)(state.timing.speed * 100.0f),
        state.timing.last_before, (double)(state.timing.last_speed * 100.0f));
    state.pending = true;
    state.loser = side;
    state.ko_write = *recomp_memory_u16(RING_WRITE);
}

static void run_replay(void)
{
    const uint16_t length = *recomp_memory_u16(RING_LENGTH);
    if (length == 0u) return;
    const unsigned winner = state.loser ^ 1u;
    const uint32_t round_stage = *recomp_memory_u32(ROUND_STAGE);
    const uint8_t stage = *recomp_memory(STAGE, 1u), old_winner = *recomp_memory(WINNER, 1u);
    uint8_t *human = recomp_memory(HUMAN + SIDE_STRIDE * winner, 1u);
    const uint8_t was_human = *human;
    const int move = recomp_ko_replay_listed_move(*recomp_memory(KO_CHARACTER, 1u));
    /* The newest frame is `cut` past the KO's. */
    const RecompKoReplayTiming *timing = &state.timing;
    const float newest = (float)(length - 1u), ko = newest - (float)timing->cut;
    for (unsigned i = 0u; i < PASSES; ++i) {
        const bool last = i == timing->shots - 1u;
        state.starts[i] = ko - (float)(last ? timing->last_before : timing->before);
        if (state.starts[i] < 0.0f) state.starts[i] = 0.0f;
        /* A pass's end frame is not shown (the task moves on to the next pass): one past. */
        state.ends[i] = last ? newest : ko + (float)timing->after + 1.0f;
        if (state.ends[i] > newest + 1.0f) state.ends[i] = newest + 1.0f;
    }
    *recomp_memory(WINNER, 1u) = (uint8_t)winner;
    *human = 1u;
    if (move >= 0) *recomp_memory_u16(KO_MOVE) = (uint16_t)move;
    *recomp_memory_u32(ROUND_STAGE) = REPLAY_STAGE;
    float tables[6];
    memcpy(tables, recomp_memory(START_TABLES, 12u), 12u);
    memcpy(tables + 3, recomp_memory(START_TABLES_ZERO, 12u), 12u);
    const float since = (float)*recomp_memory_u16(SINCE);
    for (unsigned i = 0u; i < 3u; ++i) {
        const float plain = (state.starts[i] - 180.0f) / 60.0f, zero = (state.starts[i] - 180.0f + since) / 60.0f;
        memcpy(recomp_memory(START_TABLES + 4u * i, 4u), &plain, 4u);
        memcpy(recomp_memory(START_TABLES_ZERO + 4u * i, 4u), &zero, 4u);
    }
    fprintf(stderr, "recomp ko replay: replaying side %u's KO now (move %d, KO kind %u)\n", state.loser, move,
        *recomp_memory(KO_KIND, 1u));
    const RecompRegisters registers = recomp_runtime.registers;
    state.nested = true;
    state.last_repeat = -1;
    kernel_call_lifted(sub_000CC650, NULL, 0u);
    state.nested = false;
    recomp_runtime.registers = registers;
    memcpy(recomp_memory(START_TABLES, 12u), tables, 12u);
    memcpy(recomp_memory(START_TABLES_ZERO, 12u), tables + 3, 12u);
    *human = was_human;
    *recomp_memory(WINNER, 1u) = old_winner;
    *recomp_memory_u32(ROUND_STAGE) = round_stage;
    *recomp_memory(STAGE, 1u) = stage;
    state.replayed = true;
    fprintf(stderr, "recomp ko replay: back to the fight\n");
}

/* Whether the game would replay this KO too (as 0xcc650 checks): the
   replays on (0x87ae08 or 0x87d400), a mode that has them, and not a double
   KO. */
static bool game_would_replay(void)
{
    const uint8_t mode = *recomp_memory(GAME_MODE, 1u);
    const bool replays = *recomp_memory(0x0087ae08u, 1u) != 0u || *recomp_memory(0x0087d400u, 1u) != 0u;
    const bool double_ko = *recomp_memory_u16(0x0087ae54u) == 0u && *recomp_memory_u16(0x0087ae54u + SIDE_STRIDE) == 0u;
    const bool would = replays && (mode < 0xdu || mode > 0x10u) && !double_ko;
    if (!would)
        fprintf(stderr, "recomp ko replay: the game's own (replays %u/%u, mode %u, double KO %u)\n",
            *recomp_memory(0x0087ae08u, 1u), *recomp_memory(0x0087d400u, 1u), mode, double_ko);
    return would;
}

/* Frames the fight has recorded since the KO. */
static unsigned recorded_since_ko(void)
{
    const uint16_t length = *recomp_memory_u16(RING_LENGTH);
    if (length == 0u) return 0u;
    return (unsigned)((*recomp_memory_u16(RING_WRITE) + length - state.ko_write) % length);
}

/* 0xfa3d0: the current task waits frames. The round's end task, waiting in
   the fight stage once `cut` frames past a KO are recorded, runs the replay
   first. */
static void wait_frames(void)
{
    if (state.pending && !state.nested && state.stage_task >= 0 && recorded_since_ko() >= state.timing.cut &&
        *recomp_memory_u32(CURRENT_TASK) == (uint32_t)state.stage_task) {
        state.pending = false;
        if (!state.enabled || state.online) {
        } else if (*recomp_memory_u32(ROUND_STAGE) != FIGHT_STAGE) {
            fprintf(stderr, "recomp ko replay: the game's own (round stage %u)\n", *recomp_memory_u32(ROUND_STAGE));
        } else if (*recomp_memory(KO_KIND, 1u) == 3u) {
            /* The stage's KO: no replay; the round's end skips the game's too. */
            state.replayed = true;
            fprintf(stderr, "recomp ko replay: a KO of the stage's: no replay\n");
        } else if (game_would_replay()) {
            run_replay();
        }
    }
    sub_000FA3D0();
}

void recomp_ko_replay_frame(void)
{
    const unsigned last_pass = state.timing.shots - 1u;
    if (state.nested && *recomp_memory_u32(REPLAY_MODE) == 0u && position() < state.starts[last_pass])
        set_position(state.starts[last_pass]); /* the single-pass mode: start as the last pass */
    /* A new round: the ring starts over; forget the last. */
    const uint16_t held = *recomp_memory_u16(RING_HELD);
    if (held < state.last_held && !state.nested) state.replayed = false;
    state.last_held = held;
}

/* 0xcf900, the round's end. At its replay stage after ours: what the
   replay task's end does, without the replay. */
static void round_end(void)
{
    state.stage_task = (int)*recomp_memory_u32(CURRENT_TASK);
    if (state.replayed && *recomp_memory_u32(ROUND_STAGE) == REPLAY_STAGE) {
        state.replayed = false;
        /* 0xcf900's own start, then 0xcc650's end (0xccd28). */
        kernel_call_lifted(sub_000CFA20, NULL, 0u);
        if (recomp_runtime.registers.eax != 0u) kernel_call_lifted(sub_000D0710, NULL, 0u);
        *recomp_memory(STAGE, 1u) = REPLAY_STAGE;
        *recomp_memory_u16(SHOWN) = 299u;
        set_position(299.0f);
        *recomp_memory_u32(ROUND_STAGE) = 5u;
        kernel_call_lifted(sub_001106C0, NULL, 0u);
        const uint32_t next = *recomp_memory(0x008a39c8u, 1u);
        const uint8_t winner = *recomp_memory(WINNER, 1u);
        if (*recomp_memory(GAME_MODE, 1u) == 3u && *recomp_memory(0x008783a8u, 1u) == 0u && winner < 2u &&
            *recomp_memory(HUMAN + SIDE_STRIDE * winner, 1u) == 1u &&
            *recomp_memory(0x008a3840u + 12u * *recomp_memory(0x008783ecu, 1u), 1u) == 0u) {
            /* As 0xccd8f, around a 0x40-byte local. */
            const uint32_t saved_esp = recomp_runtime.registers.esp;
            recomp_runtime.registers.esp -= 0x40u;
            const uint32_t local = recomp_runtime.registers.esp;
            kernel_call_lifted(sub_000D9D20, &local, 1u);
            kernel_call_lifted(sub_000CB2A0, &next, 1u);
            kernel_call_lifted(sub_000D9D80, &local, 1u);
            recomp_runtime.registers.esp = saved_esp;
        } else {
            kernel_call_lifted(sub_000CB2A0, &next, 1u);
        }
        fprintf(stderr, "recomp ko replay: the round moves on\n");
        recomp_runtime.registers.esp += 4u; /* 0xcf900's ret */
        return;
    }
    sub_000CF900();
}

/* 0xc4950 (repeat in ECX): one frame of a multi-angle pass; 1 when it ends. */
static void pass_step(void)
{
    if (!state.nested) {
        sub_000C4950();
        return;
    }
    const unsigned shots = state.timing.shots;
    const unsigned repeat = recomp_runtime.registers.ecx < shots ? recomp_runtime.registers.ecx : shots - 1u;
    float at = position();
    if ((int)repeat != state.last_repeat) {
        at = state.starts[repeat]; /* as written to the task's tables; ours if it read others */
        state.last_repeat = (int)repeat;
    }
    const bool last = repeat == shots - 1u;
    at += last ? state.timing.last_speed : state.timing.speed;
    uint32_t ended = 0u;
    if (at >= state.ends[repeat]) {
        at = state.ends[repeat];
        ended = !last; /* the last pass ends the replay at 299 */
    }
    set_position(at);
    recomp_runtime.registers.eax = ended;
    recomp_runtime.registers.esp += 4u;
}

/* While ours runs, the task's end does nothing: the round is not over yet. */
static void winner_pose(void)
{
    if (!state.nested) {
        sub_001106C0();
        return;
    }
    recomp_runtime.registers.esp += 4u;
}

static void what_follows(void)
{
    if (!state.nested) {
        sub_000CB2A0();
        return;
    }
    recomp_runtime.registers.esp += 4u;
}

void recomp_ko_replay_install(bool online)
{
    state.online = online;
    state.pending = state.nested = state.replayed = false;
}

void recomp_ko_replay_set_enabled(bool enabled)
{
    state.enabled = enabled;
    if (!enabled) state.pending = false;
}

RecompFunction recomp_ko_replay_lookup_manual(uint32_t guest_address)
{
    if (state.online) return NULL;
    switch (guest_address) {
    case PASS_STEP: return pass_step;
    case ROUND_END: return round_end;
    case WINNER_POSE: return winner_pose;
    case WHAT_FOLLOWS: return what_follows;
    case WAIT_FRAMES: return wait_frames;
    default: return NULL;
    }
}
