/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The parts of a Live fight's rollback session that need no game: the stop
   bit, the fight-only hash ranges, the player index, and the fight hooks over
   two fake guest pages. netplay_session.cpp and the guest's own bodies are
   stubbed here (live_fight_test_stubs, below); everything else is the real
   code the runner links. */
#include "game_state.h"
#include "kernel_abi.h"
#include "live_fight.h"
#include "live_fight_rules.h"
#include "live_session.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "live_fight_test: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

/* --- fake guest pages --------------------------------------------------- */

/* 0xa94a70, g_pad_state: six 0x30-byte pads. */
static uint8_t pad_ram[0x10000];
/* 0xf0a540..0xf1cd00: the lockstep flags, the session player records and the
   logged-on users. */
static uint8_t session_ram[0x20000];
/* The guest stack kernel_call_guest pushes a return address onto. */
static uint8_t stack_ram[0x1000];
/* 0x875c20..0x877c20: g_fighter_bone_matrices, and a byte on each side. */
static uint8_t bone_ram[0x2002];

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x00a90000u && address + width <= 0x00aa0000u) return pad_ram + (address - 0x00a90000u);
    if (address >= 0x00f00000u && address + width <= 0x00f20000u) return session_ram + (address - 0x00f00000u);
    if (address >= 0x00001000u && address + width <= 0x00002000u) return stack_ram + (address - 0x1000u);
    if (address >= 0x00875c1fu && address + width <= 0x00877c21u) return bone_ram + (address - 0x00875c1fu);
    fprintf(stderr, "live_fight_test: guest address %08x+%zu is not in the test's pages\n", address, width);
    abort();
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, sizeof(uint32_t));
}

/* --- netplay_session.cpp and the guest's own bodies --------------------- */

static bool live_on;
static bool session_active, session_hidden;
static unsigned session_player;
static uint32_t session_frame;
static unsigned round_starts, stop_requests;
static uint32_t started_opponent;
static unsigned original_lockstep_frames, original_round_starts, original_stop_calls;
static uint32_t round_start_eax;
static unsigned lockstep_resets;

bool recomp_netplay_session_live(void) { return live_on; }
bool recomp_live_rollback_active(void) { return session_active; }
bool recomp_live_rollback_hidden(void) { return session_hidden; }
uint32_t recomp_live_rollback_frame(void) { return session_frame; }
unsigned recomp_live_rollback_player(void) { return session_player; }
void recomp_live_round_started(uint32_t opponent_ina)
{
    ++round_starts;
    started_opponent = opponent_ina;
}
void recomp_live_request_stop(void) { ++stop_requests; }

void sub_00298930(void) { ++original_lockstep_frames; }
void sub_000D1000(void)
{
    ++original_round_starts;
    recomp_runtime.registers.eax = round_start_eax;
}
void sub_00293980(void) { ++original_stop_calls; }

void recomp_dispatch_indirect_site(uint32_t address, uint32_t saved_esp, const char *member, int line)
{
    (void)saved_esp;
    (void)member;
    (void)line;
    if (address == 0x002936a0u) ++lockstep_resets; /* online_lockstep_reset */
}

/* --- the rules --------------------------------------------------------- */

static void test_stop_bit(void)
{
    CHECK(RECOMP_LIVE_STOP_BUTTON == 0x8000u);
    CHECK(recomp_live_pad_add_stop(0x0001u) == 0x8001u);
    CHECK(recomp_live_pad_add_stop(0x8001u) == 0x8001u);
    CHECK(recomp_live_pad_mask_stop(0x8001u) == 0x0001u);
    CHECK(recomp_live_pad_mask_stop(0x7fffu) == 0x7fffu);
    /* Every other button and trigger bit survives both ways. */
    const uint16_t all = 0x7fffu;
    CHECK(recomp_live_pad_mask_stop(recomp_live_pad_add_stop(all)) == all);
    CHECK(recomp_live_pads_carry_stop(0x8000u, 0u));
    CHECK(recomp_live_pads_carry_stop(0u, 0x8000u));
    CHECK(recomp_live_pads_carry_stop(0x8000u, 0x8000u));
    CHECK(!recomp_live_pads_carry_stop(0x7fffu, 0x7fffu));
    CHECK(!recomp_live_pads_carry_stop(0u, 0u));
}

/* Virtual addresses are 10.x.y.z in network order, as guest memory holds them:
   the four octets in order. The comparison must order them as the octets
   spell, not as the host reads the word. */
static uint32_t ina(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t bytes[4] = {a, b, c, d};
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static void test_player_index(void)
{
    const uint32_t ten_0_0_1 = ina(10u, 0u, 0u, 1u), ten_0_0_2 = ina(10u, 0u, 0u, 2u);
    CHECK(recomp_live_player_index(ten_0_0_1, ten_0_0_2) == 0u);
    CHECK(recomp_live_player_index(ten_0_0_2, ten_0_0_1) == 1u);
    /* The octet that decides is the third one here, which a host-order compare
       of the stored word would read before the first: 10.0.0.2 < 10.0.1.1. */
    const uint32_t ten_0_1_1 = ina(10u, 0u, 1u, 1u);
    CHECK(recomp_live_player_index(ten_0_0_2, ten_0_1_1) == 0u);
    CHECK(recomp_live_player_index(ten_0_1_1, ten_0_0_2) == 1u);
    /* Both sides of one fight pick different indices. */
    const uint32_t a = ina(10u, 1u, 2u, 3u), b = ina(10u, 1u, 2u, 4u);
    CHECK(recomp_live_player_index(a, b) != recomp_live_player_index(b, a));
}

static void test_hash_ranges(void)
{
    RecompRamRange ignored[RECOMP_SNAPSHOT_HASH_RANGES];
    const size_t count = recomp_live_hash_ignored(ignored, RECOMP_SNAPSHOT_HASH_RANGES);
    CHECK(count > 5u);
    const RecompRamRange kept[] = {
        {0x00863a8cu, 0x0087d56cu},
        {0x0087d570u, 0x0087f53cu},
        {0x008a3968u, 0x008a396cu},
        {0x008a5768u, 0x008a612cu},
    };
    const size_t kept_count = sizeof kept / sizeof *kept;
    size_t kept_bytes = 0u, outside = 0u;
    for (size_t i = 0u; i < kept_count; ++i) kept_bytes += kept[i].stop - kept[i].start;
    /* Sorted and disjoint; every byte outside the fight is ignored. */
    for (size_t i = 0u; i < count; ++i) {
        CHECK(ignored[i].stop > ignored[i].start);
        if (i > 0u) CHECK(ignored[i].start >= ignored[i - 1u].stop);
        bool inside = false;
        for (size_t k = 0u; k < kept_count; ++k)
            inside = inside || (ignored[i].start >= kept[k].start && ignored[i].stop <= kept[k].stop);
        if (!inside) outside += ignored[i].stop - ignored[i].start;
    }
    CHECK(outside + kept_bytes == RECOMP_XBOX_RAM_SIZE);
    /* Frames since boot, what a player's widescreen and 5.1 choices change and
       what the menu demo scenes leave behind stay out; the fighters, their
       bones, the round timer, the match's life and tag settings, the bytes the
       fight reads beside the demo's state, and the random numbers stay in. */
    static const struct { uint32_t address; bool skipped; } probes[] = {
        {0x0087d56cu, true}, {0x00877c6cu, true}, {0x00879bf8u, true}, {0x00879c10u, true},
        {0x0087a93cu, true}, {0x0087d360u, true}, {0x0087d39cu, true}, {0x0087f538u, true},
        {0x00863a8cu, false}, {0x0087f539u, false}, {0x008a3968u, false}, {0x008a5768u, false},
        {0x00876050u, false}, {0x00877c9cu, false}, {0x0087d498u, false}, {0x0087d49du, false},
        {0x0087d5a4u, false}, {0x0087d63cu, false}, {0x0087f51du, false},
        {0x008785f0u, true}, {0x00877c78u, true}, {0x00877cd8u, true}, {0x0087d128u, true}, {0x0087d23au, true},
        {0x0087d580u, true}, {0x0087f2b0u, true}, {0x0087f50cu, true}, {0x0087bfa0u, true},
    };
    for (size_t p = 0u; p < sizeof probes / sizeof *probes; ++p) {
        bool skipped = false;
        for (size_t i = 0u; i < count; ++i)
            skipped = skipped || (ignored[i].start <= probes[p].address && ignored[i].stop > probes[p].address);
        CHECK(skipped == probes[p].skipped);
    }
    /* A buffer that cannot hold the ranges says so instead of overflowing. */
    CHECK(recomp_live_hash_ignored(ignored, count - 1u) == 0u);
    CHECK(recomp_live_hash_ignored(NULL, RECOMP_SNAPSHOT_HASH_RANGES) == 0u);
}

static void test_window(void)
{
    CHECK(recomp_live_window(true, 0u) == 0u);
    CHECK(recomp_live_window(true, 9u) == 0u);
    CHECK(recomp_live_window(true, 10u) == RECOMP_ROLLBACK_WINDOW);
    CHECK(recomp_live_window(true, 11u) == RECOMP_ROLLBACK_WINDOW);
    CHECK(recomp_live_window(false, 10u) == 0u);
    CHECK(recomp_live_window(false, 600u) == 0u);
}

/* --- the session's ends ------------------------------------------------ */

/* A desync, the other player's timeout, a failed rewind or their goodbye: a Live
   fight hands the fight back to the game and keeps the runner alive, while every
   other session stops it, as before. */
static unsigned told_opponent, stopped, handed_back;
static char ended_stop[48];

static void end_tell(void *context)
{
    (void)context;
    ++told_opponent;
}

static void end_stop(void *context, const char *stop_id)
{
    (void)context;
    ++stopped;
    snprintf(ended_stop, sizeof ended_stop, "%s", stop_id);
}

static void end_hand_back(void *context, const char *stop_id)
{
    (void)context;
    ++handed_back;
    snprintf(ended_stop, sizeof ended_stop, "%s", stop_id);
}

static void reset_end(void)
{
    told_opponent = stopped = handed_back = 0u;
    ended_stop[0] = '\0';
}

static void test_session_ends(void)
{
    const RecompLiveEndOps ops = {NULL, end_tell, end_stop, end_hand_back};

    /* The stop ids stay netplay's own, so a failure is named the same either way. */
    CHECK(strcmp(recomp_live_end_stop_id(RECOMP_LIVE_END_DESYNC), "rollback:desync") == 0);
    CHECK(strcmp(recomp_live_end_stop_id(RECOMP_LIVE_END_TIMEOUT), "rollback:peer-timeout") == 0);
    CHECK(strcmp(recomp_live_end_stop_id(RECOMP_LIVE_END_REWIND), "rollback:unrestorable") == 0);
    CHECK(strcmp(recomp_live_end_stop_id(RECOMP_LIVE_END_PEER_LEFT), "rollback:peer-left") == 0);

    /* A Live timeout ends quietly with no runner stop: the opponent hears this side
       once, and the fight goes back to the game. */
    reset_end();
    CHECK(recomp_live_end_session(true, RECOMP_LIVE_END_TIMEOUT, &ops));
    CHECK(stopped == 0u && handed_back == 1u && told_opponent == 1u);
    CHECK(strcmp(ended_stop, "rollback:peer-timeout") == 0);

    /* A desync and a failed rewind end the same way. */
    reset_end();
    CHECK(recomp_live_end_session(true, RECOMP_LIVE_END_DESYNC, &ops));
    CHECK(stopped == 0u && handed_back == 1u && told_opponent == 1u);
    CHECK(strcmp(ended_stop, "rollback:desync") == 0);
    reset_end();
    CHECK(recomp_live_end_session(true, RECOMP_LIVE_END_REWIND, &ops));
    CHECK(stopped == 0u && handed_back == 1u && told_opponent == 1u);
    CHECK(strcmp(ended_stop, "rollback:unrestorable") == 0);

    /* The other runner's goodbye (netplay.c sets peer_left; live_transport_test pins it
       crossing the route) ends a Live session at once, with no answer: they are gone. */
    reset_end();
    CHECK(recomp_live_end_session(true, RECOMP_LIVE_END_PEER_LEFT, &ops));
    CHECK(stopped == 0u && handed_back == 1u && told_opponent == 0u);
    CHECK(strcmp(ended_stop, "rollback:peer-left") == 0);

    /* Any other session still stops the runner, with the same stop id. */
    reset_end();
    CHECK(!recomp_live_end_session(false, RECOMP_LIVE_END_TIMEOUT, &ops));
    CHECK(stopped == 1u && handed_back == 0u && told_opponent == 0u);
    CHECK(strcmp(ended_stop, "rollback:peer-timeout") == 0);
    reset_end();
    CHECK(!recomp_live_end_session(false, RECOMP_LIVE_END_DESYNC, &ops));
    CHECK(stopped == 1u && handed_back == 0u);
    CHECK(strcmp(ended_stop, "rollback:desync") == 0);
    reset_end();
    CHECK(!recomp_live_end_session(false, RECOMP_LIVE_END_REWIND, &ops));
    CHECK(stopped == 1u && handed_back == 0u);
    CHECK(strcmp(ended_stop, "rollback:unrestorable") == 0);

    /* Missing ops end nothing and crash nothing. */
    CHECK(!recomp_live_end_session(false, RECOMP_LIVE_END_TIMEOUT, NULL));
    CHECK(recomp_live_end_session(true, RECOMP_LIVE_END_TIMEOUT, NULL));
}

/* --- the fight hooks --------------------------------------------------- */

static uint8_t *pad(uint32_t slot)
{
    return recomp_memory(0x00a94a70u + slot * 0x30u, 0x30u);
}

static void reset_guest(void)
{
    memset(pad_ram, 0, sizeof pad_ram);
    memset(session_ram, 0, sizeof session_ram);
    recomp_runtime.registers.esp = 0x1004u;
}

static void test_lookup(void)
{
    live_on = false;
    CHECK(recomp_live_fight_lookup(0x00298930u) == NULL);
    CHECK(recomp_live_fight_lookup(0x000d1000u) == NULL);
    CHECK(recomp_live_fight_lookup(0x00293980u) == NULL);
    live_on = true;
    CHECK(recomp_live_fight_lookup(0x00298930u) != NULL);
    CHECK(recomp_live_fight_lookup(0x000d1000u) != NULL);
    CHECK(recomp_live_fight_lookup(0x00293980u) != NULL);
    CHECK(recomp_live_fight_lookup(0x00298931u) == NULL);
}

static void test_lockstep_frame(void)
{
    reset_guest();
    live_on = true;
    const RecompFunction frame = recomp_live_fight_lookup(0x00298930u);
    CHECK(frame != NULL);
    if (frame == NULL) return;

    /* Outside a session the game's own lockstep runs. */
    session_active = false;
    frame();
    CHECK(original_lockstep_frames == 1u);
    CHECK(*(uint32_t *)recomp_memory(0x00f0a560u, 4u) == 0u);

    /* In one, both session pads become the virtual pad slots. */
    session_active = true;
    session_player = 0u;
    for (unsigned i = 0u; i < 0x30u; ++i) {
        pad(0u)[i] = (uint8_t)(0x10u + i);
        pad(1u)[i] = (uint8_t)(0x80u + i);
    }
    *(uint32_t *)recomp_memory(0x00f0a540u, 4u) = 7u; /* stalled */
    *(uint32_t *)recomp_memory(0x00f0a560u, 4u) = 41u; /* batch */
    recomp_runtime.registers.esp = 0x1004u;
    frame();
    CHECK(original_lockstep_frames == 1u);
    CHECK(memcmp(pad(5u), pad(0u), 0x30u) == 0);
    CHECK(memcmp(pad(6u), pad(1u), 0x30u) == 0);
    CHECK(*(uint32_t *)recomp_memory(0x00f0a540u, 4u) == 0u);
    CHECK(*(uint32_t *)recomp_memory(0x00f0a560u, 4u) == 42u);
    CHECK(recomp_runtime.registers.esp == 0x1008u); /* cdecl: the caller's arguments stay */

    /* The higher-addressed fighter is player 1, so its pads swap sides. */
    session_player = 1u;
    recomp_runtime.registers.esp = 0x1004u;
    frame();
    CHECK(memcmp(pad(5u), pad(1u), 0x30u) == 0);
    CHECK(memcmp(pad(6u), pad(0u), 0x30u) == 0);
}

static void test_request_stop(void)
{
    reset_guest();
    live_on = true;
    const RecompFunction stop = recomp_live_fight_lookup(0x00293980u);
    CHECK(stop != NULL);
    if (stop == NULL) return;

    session_active = false;
    stop();
    CHECK(original_stop_calls == 1u);
    CHECK(stop_requests == 0u);

    session_active = true;
    session_hidden = false;
    recomp_runtime.registers.esp = 0x1004u;
    stop();
    CHECK(original_stop_calls == 1u);
    CHECK(stop_requests == 1u);
    CHECK(recomp_runtime.registers.esp == 0x1008u);

    /* netplay_session.cpp drops the request in a hidden frame; the hook still passes it on. */
    session_hidden = true;
    recomp_runtime.registers.esp = 0x1004u;
    stop();
    CHECK(stop_requests == 2u);
}

static void test_stop_lockstep(void)
{
    reset_guest();
    lockstep_resets = 0u;
    recomp_live_stop_lockstep();
    CHECK(lockstep_resets == 1u);
    CHECK(recomp_runtime.registers.esp == 0x1004u); /* kernel_call_guest restores ESP */
}

/* The session player records: g_session_players 0xf192a8, records at 0xf0a5a0,
   sockaddr sin_addr at +0x18, XUID at +0, the local mark at +0x2c. */
enum { RECORD_BYTES = 0x30, RECORD_A = 0x00f0a5a0u, RECORD_B = 0x00f0a5d0u, USERS = 0x00f1cba8u };

static void set_member(uint32_t record, uint64_t xuid, uint32_t ina, bool local_mark)
{
    memcpy(recomp_memory(record, 8u), &xuid, 8u);
    memcpy(recomp_memory(record + 0x18u, 4u), &ina, 4u);
    *recomp_memory(record + 0x2cu, 1u) = local_mark ? 1u : 0u;
}

static void set_slots(uint32_t first, uint32_t second)
{
    memcpy(recomp_memory(0x00f192a8u, 4u), &first, 4u);
    memcpy(recomp_memory(0x00f192acu, 4u), &second, 4u);
}

static void test_fight_pair(void)
{
    const uint64_t me = 0x0009000000000001ull, other = 0x0009000000000002ull;
    const uint32_t mine_ina = ina(10u, 0u, 0u, 2u), their_ina = ina(10u, 0u, 0u, 1u);
    uint32_t local = 0u, opponent = 0u;

    /* The logged-on XUID names the local fighter. */
    reset_guest();
    memcpy(recomp_memory(USERS, 8u), &me, 8u);
    set_member(RECORD_A, me, mine_ina, false);
    set_member(RECORD_B, other, their_ina, false);
    set_slots(RECORD_A, RECORD_B);
    CHECK(recomp_game_live_fight_pair(&local, &opponent));
    CHECK(local == mine_ina);
    CHECK(opponent == their_ina);
    CHECK(recomp_live_player_index(local, opponent) == 1u);
    /* The same pair from the other side's seat. */
    memcpy(recomp_memory(USERS, 8u), &other, 8u);
    CHECK(recomp_game_live_fight_pair(&local, &opponent));
    CHECK(local == their_ina);
    CHECK(opponent == mine_ina);
    CHECK(recomp_live_player_index(local, opponent) == 0u);

    /* Without a logged-on user the game's own local mark decides. */
    memset(recomp_memory(USERS, 8u), 0, 8u);
    set_member(RECORD_A, me, mine_ina, true);
    set_member(RECORD_B, other, their_ina, false);
    CHECK(recomp_game_live_fight_pair(&local, &opponent));
    CHECK(local == mine_ina);
    CHECK(opponent == their_ina);

    /* Both marked, or only one fighter in the table: no session. */
    set_member(RECORD_A, me, mine_ina, true);
    set_member(RECORD_B, other, their_ina, true);
    CHECK(!recomp_game_live_fight_pair(&local, &opponent));
    set_slots(RECORD_A, 0u);
    CHECK(!recomp_game_live_fight_pair(&local, &opponent));
    set_slots(0u, 0u);
    CHECK(!recomp_game_live_fight_pair(&local, &opponent));

    /* Two fighters at one address cannot be ordered into two players: both
       sides would pick the same index and swap the pads. */
    set_slots(RECORD_A, RECORD_B);
    set_member(RECORD_A, me, mine_ina, true);
    set_member(RECORD_B, other, mine_ina, false);
    CHECK(!recomp_game_live_fight_pair(&local, &opponent));
}

static void test_round_start(void)
{
    const uint64_t me = 0x0009000000000001ull, other = 0x0009000000000002ull;
    const uint32_t mine_ina = ina(10u, 0u, 0u, 2u), their_ina = ina(10u, 0u, 0u, 1u);

    reset_guest();
    live_on = true;
    memcpy(recomp_memory(USERS, 8u), &me, 8u);
    set_member(RECORD_A, me, mine_ina, true);
    set_member(RECORD_B, other, their_ina, false);
    set_slots(RECORD_A, RECORD_B);
    const RecompFunction round_start = recomp_live_fight_lookup(0x000d1000u);
    CHECK(round_start != NULL);
    if (round_start == NULL) return;

    /* The game's handshake runs first, then all four fighters' bones are
       zeroed, and nothing beside them, and the session is asked for with the
       other fighter's address. */
    memset(bone_ram, 0xa5, sizeof bone_ram);
    round_start_eax = 0u;
    session_active = false;
    round_start();
    CHECK(original_round_starts == 1u);
    CHECK(round_starts == 1u);
    CHECK(started_opponent == their_ina);
    bool bones_clear = true;
    for (size_t i = 1u; i <= 0x2000u; ++i) bones_clear = bones_clear && bone_ram[i] == 0u;
    CHECK(bones_clear);
    CHECK(bone_ram[0] == 0xa5u && bone_ram[0x2001] == 0xa5u);

    /* An aborted handshake starts nothing and leaves the bones. */
    memset(bone_ram, 0xa5, sizeof bone_ram);
    round_start_eax = 1u;
    round_start();
    CHECK(original_round_starts == 2u);
    CHECK(round_starts == 1u);
    CHECK(bone_ram[1] == 0xa5u);
}

int main(void)
{
    test_stop_bit();
    test_player_index();
    test_hash_ranges();
    test_window();
    test_session_ends();
    test_lookup();
    test_lockstep_frame();
    test_request_stop();
    test_stop_lockstep();
    test_fight_pair();
    test_round_start();
    if (failures != 0) {
        fprintf(stderr, "live_fight_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("live_fight_test: all checks passed\n");
    return 0;
}
