/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "unlock_profile.h"
#include "xbox_memory_layout.h"

#include <string.h>

/* The game data pointer at 0x87cfa8 points 0xf200 into the active profile's
   0x12118-byte save slot. It holds one costume byte per character at +0x2ed8,
   in the 15-character order from 0x6959dc mapped through 0x6959c0, and the
   hidden-character flags at +0x2ed2 to +0x2ed4; unlocked, each costume byte
   is its count from 0x6e0f08 and each flag is 1.

   Every save of a slot goes through 0x21b50 or 0x21bb0 (cdecl name, slot),
   every load through 0x21d90 or 0x21de0 (the same arguments), and every
   award through 0xdc840, which raises costume bytes (0xdc640, 0xdc090) and
   flags (0xdc100) after a fight. New profiles start at 2 costumes, which is
   never the unlocked set.

   The earned values live in guest memory, in the reserved kernel page at
   +0xd00, so rollback and replays restore them with the rest of the frame. */
enum {
    GAME_DATA = 0x0087cfa8u,
    CHARACTER_ORDER = 0x006959dcu,
    CHARACTER_SLOT = 0x006959c0u,
    COSTUME_COUNT = 0x006e0f08u,
    COSTUMES = 0x2ed8u,
    FLAGS = 0x2ed2u,
    CHARACTERS = 15u,
    VALUES = CHARACTERS + 3u,
    SLOT_SIZE = 0x12118u,
    SAVE = 0x00021b50u,
    SAVE_OTHER = 0x00021bb0u,
    LOAD = 0x00021d90u,
    LOAD_OTHER = 0x00021de0u,
    AWARD = 0x000dc840u,
    RECORDS = XBOX_KERNEL_DATA_BASE + 0xd00u,
    RECORD_SIZE = 0x20u, /* game data pointer, then the earned values */
    RECORD_COUNT = 8u,
};

extern void sub_00021B50(void);
extern void sub_00021BB0(void);
extern void sub_00021D90(void);
extern void sub_00021DE0(void);
extern void sub_000DC840(void);

static uint32_t value_address(uint32_t data, unsigned i)
{
    if (i >= CHARACTERS) return data + FLAGS + (i - CHARACTERS);
    const uint8_t character = *recomp_memory(CHARACTER_ORDER + i, 1u);
    return data + COSTUMES + *recomp_memory(CHARACTER_SLOT + character, 1u);
}

static void read_values(uint32_t data, uint8_t values[VALUES])
{
    for (unsigned i = 0u; i < VALUES; ++i) values[i] = *recomp_memory(value_address(data, i), 1u);
}

static void write_values(uint32_t data, const uint8_t values[VALUES])
{
    for (unsigned i = 0u; i < VALUES; ++i) *recomp_memory(value_address(data, i), 1u) = values[i];
}

static void unlocked_values(uint8_t values[VALUES])
{
    for (unsigned i = 0u; i < CHARACTERS; ++i) {
        const uint8_t character = *recomp_memory(CHARACTER_ORDER + i, 1u);
        values[i] = *recomp_memory(COSTUME_COUNT + character, 1u);
    }
    memset(values + CHARACTERS, 1, 3u);
}

static uint32_t *record_data(unsigned record)
{
    return recomp_memory_u32(RECORDS + record * RECORD_SIZE);
}

static uint8_t *record_earned(unsigned record)
{
    return recomp_memory(RECORDS + record * RECORD_SIZE + 4u, VALUES);
}

static int find_record(uint32_t data)
{
    for (unsigned record = 0u; record < RECORD_COUNT; ++record)
        if (*record_data(record) == data) return (int)record;
    return -1;
}

void recomp_unlock_profile_reset(void)
{
    memset(recomp_memory(RECORDS, RECORD_COUNT * RECORD_SIZE), 0, RECORD_COUNT * RECORD_SIZE);
}

void recomp_unlock_profile_frame(bool enabled)
{
    uint8_t unlocked[VALUES], current[VALUES];
    unlocked_values(unlocked);
    if (!enabled) {
        for (unsigned record = 0u; record < RECORD_COUNT; ++record) {
            const uint32_t data = *record_data(record);
            if (data == 0u) continue;
            read_values(data, current);
            /* A profile loaded over this one since holds its own values. */
            if (memcmp(current, unlocked, VALUES) == 0) write_values(data, record_earned(record));
            *record_data(record) = 0u;
        }
        return;
    }
    const uint32_t data = *recomp_memory_u32(GAME_DATA);
    if (data == 0u) return;
    read_values(data, current);
    int record = find_record(data);
    if (record < 0) {
        record = find_record(0u);
        if (record < 0) return; /* no room: leave this profile as earned */
        *record_data((unsigned)record) = data;
        memcpy(record_earned((unsigned)record), current, VALUES);
    } else if (memcmp(current, unlocked, VALUES) != 0) {
        /* Rewritten since the last frame (a new profile, or a load not
           through the hooks): what is there now is what was earned. */
        memcpy(record_earned((unsigned)record), current, VALUES);
    }
    write_values(data, unlocked);
}

/* The records whose game data lies in the slot an entry's second argument
   names (cdecl name, slot). */
static uint32_t slot_records(void)
{
    const uint32_t slot = *recomp_memory_u32(recomp_runtime.registers.esp + 8u);
    uint32_t mask = 0u;
    for (unsigned record = 0u; record < RECORD_COUNT; ++record) {
        const uint32_t data = *record_data(record);
        if (data != 0u && data >= slot && data - slot < SLOT_SIZE) mask |= 1u << record;
    }
    return mask;
}

/* The slot is written with the earned values, then unlocked again. */
static void save_earned(void (*save)(void))
{
    const uint32_t mask = slot_records();
    uint8_t current[RECORD_COUNT][VALUES];
    for (unsigned record = 0u; record < RECORD_COUNT; ++record) {
        if ((mask & (1u << record)) == 0u) continue;
        read_values(*record_data(record), current[record]);
        write_values(*record_data(record), record_earned(record));
    }
    save();
    for (unsigned record = 0u; record < RECORD_COUNT; ++record)
        if ((mask & (1u << record)) != 0u) write_values(*record_data(record), current[record]);
}

static void save(void) { save_earned(sub_00021B50); }
static void save_other(void) { save_earned(sub_00021BB0); }

/* A load replaces the slot's profile, so the next frame records it anew. */
static void load_fresh(void (*load)(void))
{
    const uint32_t mask = slot_records();
    load();
    for (unsigned record = 0u; record < RECORD_COUNT; ++record)
        if ((mask & (1u << record)) != 0u) *record_data(record) = 0u;
}

static void load(void) { load_fresh(sub_00021D90); }
static void load_other(void) { load_fresh(sub_00021DE0); }

/* Awards count from what was earned, so they still unlock (with the game's
   own notices) and are kept. */
static void award(void)
{
    const uint32_t data = *recomp_memory_u32(GAME_DATA);
    const int record = data != 0u ? find_record(data) : -1;
    if (record < 0) {
        sub_000DC840();
        return;
    }
    uint8_t current[VALUES];
    read_values(data, current);
    write_values(data, record_earned((unsigned)record));
    sub_000DC840();
    read_values(data, record_earned((unsigned)record));
    write_values(data, current);
}

RecompFunction recomp_unlock_profile_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case SAVE: return save;
    case SAVE_OTHER: return save_other;
    case LOAD: return load;
    case LOAD_OTHER: return load_other;
    case AWARD: return award;
    default: return NULL;
    }
}
