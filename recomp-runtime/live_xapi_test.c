/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_xapi.h"
#include "live_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Drives the guest XOnline thunks the way the game does: arguments are pushed
   at ESP and kernel_return's cleanup is checked, then the guest records the
   handlers wrote are read back. Guest memory is one flat array here, so the
   addresses below are the test's own choice except where the game's own
   globals are named (the profile's bound XUID). */

enum {
    STACK = 0x1000,
    USERS = 0x2000,
    USERS2 = 0x2200,
    ATTRIBUTES = 0x3000,
    WIDE_SOURCE = 0x3400,
    BLOB_SOURCE = 0x3500,
    SPECS = 0x3600,
    REQUEST = 0x4000,
    REQUEST2 = 0x4018,
    STAT_ENTRIES = 0x4100,
    STAT_ENTRIES2 = 0x4280,
    OUT = 0x5000,
    TASK_OUT = OUT + 0x20,
    KID_OUT = OUT + 0x30,
    KEY_OUT = OUT + 0x40,
    COUNT_OUT = OUT + 0x60,
    RESULT_ARRAY = OUT + 0x70,
    BOARD_OUT = OUT + 0x80,
    ROWS_OUT = OUT + 0x90,
    STATS_OUT = OUT + 0xa0,
    TAG_OUT = OUT + 0xb0,
    FRIEND_RECORD = 0x4400,
    FRIEND_XUID_LO = FRIEND_RECORD + 0x00,
    FRIEND_XUID_HI = FRIEND_RECORD + 0x04,
    MUTES = 0x4500,
    FRIENDS = 0x4600,
    /* Group 7: 63 entries of 16 bytes for the write and read records, and the
       62 requested ids of its leaderboard. */
    GROUP7_STATS = 0x4800,
    GROUP7_STAT_IDS = 0x4c00,
    LEADER_ROWS = 0x6a00,
    LEADER_STATS = 0x6b00,
    DEST = 0x6c00,
    GROUP7_LEADER_STATS = 0x7000, /* 62 row entries of 16 bytes */
    GUEST_BYTES = 0x800000,
    HEAP_START = 0x600000,
    PROFILE_XUID = 0x007ef8e0u + 0xabe8u,
};

enum {
    TASK_SUCCESS = 0x001500f0u,
    TASK_RESULTS = 0x001500f1u,
    TASK_IDLE = 0x001500f2u,
    TASK_LOGON = 0x001510f0u,
    E_INVALIDARG = 0x80150005u,
    E_FAIL = 0x80004005u,
};

static const uint32_t thunk_get_users = 0x00393b96u;
static const uint32_t thunk_logon = 0x00393ba1u;
static const uint32_t thunk_logon_results = 0x00393bb0u;
static const uint32_t thunk_logon_users = 0x00393bbbu;
static const uint32_t thunk_service_info = 0x00393bc6u;
static const uint32_t thunk_task_continue = 0x00393b80u;
static const uint32_t thunk_task_close = 0x00393b8bu;
static const uint32_t thunk_mutelist_get = 0x00393da9u;
static const uint32_t thunk_mutelist_startup = 0x00393db8u;
static const uint32_t thunk_notification = 0x00393ca8u;
static const uint32_t thunk_friends_startup = 0x00393cd7u;
static const uint32_t thunk_friends_enumerate = 0x00393ce2u;
static const uint32_t thunk_friends_latest = 0x00393cf8u;
static const uint32_t thunk_friends_finish = 0x00393cedu;
static const uint32_t thunk_friends_title_name = 0x00393d03u;
static const uint32_t thunk_friends_remove = 0x00393d0eu;
static const uint32_t thunk_friends_request = 0x00393d19u;
static const uint32_t thunk_friends_invite = 0x00393d3bu;
static const uint32_t thunk_friends_revoke = 0x00393d5cu;
static const uint32_t thunk_friends_answer_request = 0x00393d7du;
static const uint32_t thunk_friends_answer_invite = 0x00393d88u;
static const uint32_t thunk_friends_join = 0x00393d93u;
static const uint32_t thunk_friends_accepted = 0x00393d9eu;
static const uint32_t thunk_same_title = 0x00393c9du;
static const uint32_t thunk_session_create = 0x00393be7u;
static const uint32_t thunk_session_update = 0x00393bf6u;
static const uint32_t thunk_session_info = 0x00393c26u;
static const uint32_t thunk_session_delete = 0x00393c31u;
static const uint32_t thunk_session_find = 0x00393c4fu;
static const uint32_t thunk_search = 0x00393c6du;
static const uint32_t thunk_search_results = 0x00393c7cu;
static const uint32_t thunk_search_parse = 0x00393c87u;
static const uint32_t thunk_search_len = 0x00393c92u;
static const uint32_t thunk_feedback = 0x00393e07u;
static const uint32_t thunk_stat_write = 0x00393e37u;
static const uint32_t thunk_stat_read = 0x00393e42u;
static const uint32_t thunk_stat_read_result = 0x00393e4du;
static const uint32_t thunk_leader = 0x00393e5cu;
static const uint32_t thunk_leader_results = 0x00393e6bu;
static const uint32_t thunk_string_verify = 0x00397394u;
static const uint32_t thunk_create_stream = 0x00366257u;
static const uint32_t thunk_register_key = 0x00380f89u;
static const uint32_t thunk_unregister_key = 0x00380f94u;
static const uint32_t thunk_qos_listen = 0x00380fd8u;
static const uint32_t thunk_qos_lookup = 0x00380fe7u;
static const uint32_t thunk_qos_release = 0x00381003u;
static const uint32_t thunk_random = 0x00380f7eu;
static const uint32_t thunk_title_xnaddr = 0x0038100eu;
static const uint32_t thunk_link_status = 0x002f9e3du;
static const uint32_t thunk_xnet_startup = 0x00381331u;
static const uint32_t thunk_wsa_startup = 0x00381348u;
static const uint32_t thunk_xonline_startup = 0x003941ceu;
static const uint32_t thunk_xonline_cleanup = 0x003942cbu;

/* Transport seam (live_transport.c in the transport work package): the guest
   API must forward its addresses here and nothing else. */
static uint32_t transport_calls[8];
static unsigned transport_count;
static void transport_stub(void) {}

RecompFunction recomp_live_transport_lookup(uint32_t guest_address)
{
    if (transport_count < sizeof transport_calls / sizeof transport_calls[0])
        transport_calls[transport_count] = guest_address;
    ++transport_count;
    return transport_stub;
}

/* The transport's peer table, which a logoff forgets. */
static unsigned forget_calls;
static uint32_t forget_ina;

void recomp_live_transport_forget(uint32_t virtual_ina)
{
    ++forget_calls;
    forget_ina = virtual_ina;
}

/* A hostile matchmaker: the backend's own answers, with one field changed. */
static const RecompLiveBackend *corrupt_inner;
static RecompLiveBackend corrupt_backend;

static uint32_t corrupting_results(void *self, RecompLiveRequest id, RecompLiveSession *out, uint32_t capacity)
{
    const uint32_t count = corrupt_inner->results(self, id, out, capacity);
    if (count != 0u) memset(out[0].attributes + 24u, 0x41, 34u); /* no terminator in a 34-byte field */
    return count;
}

static uint32_t corrupting_stats(void *self, RecompLiveRequest id, RecompLiveLeaderRow *out, uint32_t capacity)
{
    const uint32_t count = corrupt_inner->stat_results(self, id, out, capacity);
    if (count != 0u) {
        out[0].stats[0].id = 1u;
        out[0].stats[0].type = 5u; /* not one of the library's numeric types */
        out[0].stats[0].value = 0x1234;
    }
    return count;
}

/* A backend that reports more friends than it was given room for. */
static uint32_t inflating_friends(void *self, RecompLiveFriend *out, uint32_t capacity)
{
    return corrupt_inner->friends(self, out, capacity) + 50u;
}

static unsigned corrupt_polls;

static RecompLiveStatus corrupting_poll(void *self, RecompLiveRequest id, uint32_t *hresult)
{
    if (corrupt_polls != 0u) {
        --corrupt_polls;
        if (hresult != NULL) *hresult = 0x00001234u; /* not an HRESULT failure */
        return RECOMP_LIVE_FAILED;
    }
    return corrupt_inner->poll(self, id, hresult);
}

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram = NULL;
static uint8_t guest[GUEST_BYTES];
static uint32_t heap_cursor = HEAP_START;

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (width > GUEST_BYTES || address > GUEST_BYTES - width) abort();
    return &guest[address];
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, sizeof(uint32_t));
}

uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    const uint32_t aligned = (heap_cursor + 15u) & ~15u;
    if (aligned + size > GUEST_BYTES) abort();
    heap_cursor = aligned + size;
    return aligned;
}

static int failures;
static void check(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static uint32_t peek32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, &guest[address], sizeof value);
    return value;
}

static uint16_t peek16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, &guest[address], sizeof value);
    return value;
}

static int64_t peek64(uint32_t address)
{
    int64_t value;
    memcpy(&value, &guest[address], sizeof value);
    return value;
}

static void poke32(uint32_t address, uint32_t value)
{
    memcpy(&guest[address], &value, sizeof value);
}

/* Compares a NUL-terminated UTF-16 string the guest holds with ASCII text. */
static int wide_equals(uint32_t address, const char *text)
{
    for (unsigned i = 0u;; ++i) {
        const uint16_t unit = peek16(address + i * 2u);
        const unsigned expected = (unsigned char)text[i];
        if (unit != expected) return 0;
        if (expected == 0u) return 1;
    }
}

/* Calls a thunk with stdcall arguments at ESP and checks the cleanup the
   thunk's RET performs. */
static uint32_t call(uint32_t address, const uint32_t *arguments, unsigned count)
{
    RecompFunction function = recomp_live_lookup(address);
    if (function == NULL) {
        fprintf(stderr, "FAIL unbound thunk %08x\n", address);
        ++failures;
        return 0xffffffffu;
    }
    recomp_runtime.registers.esp = STACK;
    recomp_runtime.registers.eax = 0u;
    poke32(STACK, 0xdeadbeefu);
    for (unsigned i = 0u; i < count; ++i) poke32(STACK + 4u * (i + 1u), arguments[i]);
    function();
    const uint32_t result = recomp_runtime.registers.eax;
    if (recomp_runtime.registers.esp != STACK + 4u + 4u * count) {
        fprintf(stderr, "FAIL cleanup %08x argc %u esp %08x\n", address, count, recomp_runtime.registers.esp);
        ++failures;
    }
    return result;
}

static uint32_t call0(uint32_t address)
{
    return call(address, NULL, 0u);
}

static uint32_t call1(uint32_t address, uint32_t a0)
{
    const uint32_t arguments[1] = {a0};
    return call(address, arguments, 1u);
}

static uint32_t call2(uint32_t address, uint32_t a0, uint32_t a1)
{
    const uint32_t arguments[2] = {a0, a1};
    return call(address, arguments, 2u);
}

static void close_task(uint32_t handle)
{
    if (handle != 0u && handle != 0xffffffffu) check("task closes", call1(thunk_task_close, handle) == 0u);
}

/* The runner's online-mode hook: a logon turns it on, closing the logon task off. */
static unsigned online_changes;
static bool online_last;
static void note_online(bool online)
{
    ++online_changes;
    online_last = online;
}

/* One attribute record of the game's create/update array (27 x 16 bytes). */
static void set_attribute(unsigned index, uint32_t id, uint32_t value, uint32_t length)
{
    const uint32_t entry = ATTRIBUTES + index * 16u;
    poke32(entry + 0x0u, id);
    poke32(entry + 0x4u, 1u);
    poke32(entry + 0x8u, value);
    poke32(entry + 0xcu, length);
}

/* The 27 attributes the game hands XOnlineMatchSessionCreate: integers at 0,
   a wide string at 3 and the blob at 19. */
static void build_attributes(void)
{
    memset(&guest[ATTRIBUTES], 0, 27u * 16u);
    const uint8_t wide[6] = {'D', 0u, '2', 0u, 0u, 0u};
    memcpy(&guest[WIDE_SOURCE], wide, sizeof wide);
    memcpy(&guest[BLOB_SOURCE], "BLOB", 4u);
    set_attribute(0u, 1u, 7u, 0u); /* an 8-byte integer */
    set_attribute(1u, 2u, 0u, 0u);
    set_attribute(2u, 3u, 0u, 0u);
    for (unsigned i = 3u; i <= 11u; ++i) set_attribute(i, 0x00100000u + i, i == 3u ? WIDE_SOURCE : 0u, 0u);
    for (unsigned i = 12u; i <= 18u; ++i) set_attribute(i, 0x00000000u + i, 0u, 0u);
    set_attribute(19u, 0x00200012u, BLOB_SOURCE, 4u);
}

/* The game's stat request record: 0x18 bytes with the stat array at +0x14. */
static void set_stat_record(uint32_t address, uint64_t xuid, uint32_t board, uint32_t count, uint32_t array)
{
    poke32(address + 0x00u, (uint32_t)xuid);
    poke32(address + 0x04u, (uint32_t)(xuid >> 32));
    poke32(address + 0x0cu, board);
    poke32(address + 0x10u, count);
    poke32(address + 0x14u, array);
}

static void set_stat_entry(uint32_t address, uint16_t id, uint8_t type, int64_t value)
{
    uint8_t entry[16];
    memset(entry, 0, sizeof entry);
    memcpy(entry, &id, sizeof id);
    const uint32_t type_word = type;
    memcpy(entry + 4, &type_word, sizeof type_word);
    memcpy(entry + 8, &value, sizeof value);
    memcpy(&guest[address], entry, sizeof entry);
}

static uint32_t wait_task(uint32_t handle)
{
    return call1(thunk_task_continue, handle);
}

/* A presence counter over the in-memory backend. The game calls
   XOnlineNotificationSetState every frame; the shim must report only changes,
   as the retail library's kernel change bits do. */
static const RecompLiveBackend *presence_inner;
static unsigned presence_calls;
static RecompLiveBackend presence_backend;

static void counting_presence(void *self, uint32_t state, const uint8_t kid[8])
{
    ++presence_calls;
    if (presence_inner != NULL && presence_inner->presence != NULL)
        presence_inner->presence(self, state, kid);
}

/* The 27 attribute specs XOnlineMatchSearchResultsLen is called with
   (0x7bfd00): 3 integers, 9 wide strings of 34, 5 integers, the blob of 8,
   one integer and 8 more wide strings. */
static void build_search_specs(void)
{
    for (unsigned i = 0u; i < 27u; ++i) {
        uint32_t id = 0u, size = 8u;
        if ((i >= 3u && i <= 11u) || i >= 19u) {
            id = 0x00100000u;
            size = 34u;
        } else if (i == 17u) {
            id = 0x00200000u;
            size = 8u;
        }
        poke32(SPECS + i * 8u, id);
        poke32(SPECS + i * 8u + 4u, size);
    }
}

int main(void)
{
    /* Without a backend the guest keeps its own library. */
    check("live is off with no backend", recomp_live_lookup(thunk_get_users) == NULL);
    check("nothing is forwarded with no backend", recomp_live_lookup(0x00381019u) == NULL);

    recomp_live_backend_local_reset();
    presence_inner = recomp_live_backend_local();
    presence_backend = *presence_inner;
    presence_backend.presence = counting_presence;
    recomp_live_backend_set(&presence_backend);

    /* Transport addresses forward to the transport seam; the rest do not. */
    transport_count = 0u;
    check("socket forwards to the transport", recomp_live_lookup(0x00381019u) == transport_stub);
    check("sendto forwards to the transport", recomp_live_lookup(0x003810c0u) == transport_stub);
    check("XnAddrToInAddr forwards to the transport", recomp_live_lookup(0x00380f9fu) == transport_stub);
    check("three forwards", transport_count == 3u && transport_calls[0] == 0x00381019u &&
        transport_calls[1] == 0x003810c0u && transport_calls[2] == 0x00380f9fu);
    transport_count = 0u;
    check("XOnlineGetUsers stays with the guest API", recomp_live_lookup(thunk_get_users) != NULL &&
        recomp_live_lookup(thunk_get_users) != transport_stub && transport_count == 0u);

    /* --- account select and logon --- */
    check("account select", call2(thunk_get_users, USERS, OUT) == 0u);
    const uint64_t local_xuid = 0x0009000000a1b2c2ull;
    uint64_t seen_xuid;
    memcpy(&seen_xuid, &guest[USERS], sizeof seen_xuid);
    check("one user", peek32(OUT) == 1u && peek32(USERS + 0x08u) == 0u);
    check("user xuid", seen_xuid == local_xuid);
    check("user gamertag", strcmp((const char *)&guest[USERS + 0x0cu], "LocalPlayer") == 0);
    uint64_t bound_xuid;
    memcpy(&bound_xuid, &guest[PROFILE_XUID], sizeof bound_xuid);
    check("profile is bound to the account", bound_xuid == local_xuid);

    uint32_t service_ids[5] = {1u, 2u, 6u, 14u, 15u};
    memcpy(&guest[REQUEST], service_ids, sizeof service_ids);
    uint32_t logon_args[5] = {USERS, REQUEST, 5u, 0u, TASK_OUT};
    recomp_live_online_hook = note_online;
    check("logon returns S_OK", call(thunk_logon, logon_args, 5u) == 0u);
    const uint32_t logon_task = peek32(TASK_OUT);
    check("logon returns a task", logon_task != 0u && logon_task != 0xffffffffu);
    check("logon reports online", online_changes == 1u && online_last);
    check("logon completes", wait_task(logon_task) == TASK_LOGON);
    check("logon results", call1(thunk_logon_results, logon_task) == TASK_LOGON);
    /* The game's array is on its stack, so the logon users are a lasting copy. */
    const uint32_t logon_users = call0(thunk_logon_users);
    uint64_t logon_xuid = 0u;
    if (logon_users != 0u) memcpy(&logon_xuid, &guest[logon_users], sizeof logon_xuid);
    check("logon users are a copy of the account array", logon_users != 0u && logon_users != USERS &&
        logon_xuid == local_xuid);
    check("the user's logon succeeded", logon_users != 0u && peek32(logon_users + 0x6cu) == 0u);
    check("services are available", call2(thunk_service_info, 1u, 0u) == 0u &&
        call2(thunk_service_info, 14u, 0u) == 0u);
    close_task(logon_task);
    check("closing the logon task reports offline", online_changes == 2u && !online_last);

    /* The account page can put the account in another of the four user slots;
       the logon user record has to land there. */
    memset(&guest[USERS2], 0, 4u * 0x70u);
    memcpy(&guest[USERS2 + 0x70u], &guest[USERS], 0x70u);
    logon_args[0] = USERS2;
    check("logon at slot 1 returns S_OK", call(thunk_logon, logon_args, 5u) == 0u);
    const uint32_t slot_task = peek32(TASK_OUT);
    check("logon at slot 1 completes", wait_task(slot_task) == TASK_LOGON);
    const uint32_t slot_users = call0(thunk_logon_users);
    check("logon users are a copy of the slot-1 array", slot_users != 0u && slot_users != USERS2);
    uint64_t slot_xuid = 0u;
    if (slot_users != 0u) memcpy(&slot_xuid, &guest[slot_users + 0x70u], sizeof slot_xuid);
    check("the user is written at its slot", slot_xuid == local_xuid &&
        peek32(slot_users) == 0u && peek32(slot_users + 0x70u + 0x6cu) == 0u);
    close_task(slot_task);

    /* Notification is a presence change, not a per-frame packet. */
    presence_calls = 0u;
    check("notification state", call(thunk_notification,
        (const uint32_t[]){0u, 1u, 0u, 0u, 2u, 0u}, 6u) == 0u);
    check("notification reports once", presence_calls == 1u);
    check("same notification state", call(thunk_notification,
        (const uint32_t[]){0u, 1u, 0u, 0u, 2u, 0u}, 6u) == 0u);
    check("unchanged state is not reported", presence_calls == 1u);
    check("session notification state", call(thunk_notification,
        (const uint32_t[]){0u, 0x13u, 0x11223344u, 0x55667788u, 2u, 0u}, 6u) == 0u);
    check("changed state is reported", presence_calls == 2u);
    check("repeated session state is not reported", call(thunk_notification,
        (const uint32_t[]){0u, 0x13u, 0x11223344u, 0x55667788u, 2u, 0u}, 6u) == 0u &&
        presence_calls == 2u);

    /* The retail notification context must be refused, or friends invites
       switch to the library internals and create retail task objects this
       shim cannot poll. */
    memset(&guest[OUT], 0, 0x40u);
    check("notification context is refused", recomp_live_lookup(0x00395bb4u) != NULL &&
        call(0x00395bb4u, (const uint32_t[]){0u, 0u, 0u, 0u, 0u, 0u, 0u, OUT}, 8u) == E_FAIL &&
        peek32(OUT) == 0u);

    /* --- mutelist (logon aborts without it) --- */
    check("mutelist startup returns S_OK", call2(thunk_mutelist_startup, 0u, TASK_OUT) == 0u);
    const uint32_t mutelist_start = peek32(TASK_OUT);
    check("mutelist startup task", mutelist_start != 0u);
    check("mutelist startup completes", wait_task(mutelist_start) == TASK_IDLE);
    close_task(mutelist_start);
    const uint32_t mutelist_args[6] = {0u, 250u, 0u, TASK_OUT, MUTES, COUNT_OUT};
    check("mutelist get returns S_OK", call(thunk_mutelist_get, mutelist_args, 6u) == 0u);
    const uint32_t mutelist_task = peek32(TASK_OUT);
    check("mutelist get completes", wait_task(mutelist_task) == TASK_SUCCESS);
    uint64_t muted;
    memcpy(&muted, &guest[MUTES], sizeof muted);
    check("mutelist has the muted player", peek32(COUNT_OUT) == 1u && muted == 0x0009000000a1b2c5ull);
    close_task(mutelist_task);

    /* --- create a session (27 attributes: integer, wide string, blob) --- */
    build_attributes();
    const uint32_t create_args[8] = {1u, 3u, 0u, 0u, 27u, ATTRIBUTES, 0u, TASK_OUT};
    check("session create returns S_OK", call(thunk_session_create, create_args, 8u) == 0u);
    const uint32_t create_task = peek32(TASK_OUT);
    check("session create task", create_task != 0u && create_task != 0xffffffffu);
    check("session create completes", wait_task(create_task) == TASK_SUCCESS);
    check("session info", call(thunk_session_info,
        (const uint32_t[]){create_task, KID_OUT, KEY_OUT}, 3u) == 0u);
    uint8_t kid[8], key[16];
    memcpy(kid, &guest[KID_OUT], sizeof kid);
    memcpy(key, &guest[KEY_OUT], sizeof key);
    int kid_nonzero = 0, key_nonzero = 0;
    for (unsigned i = 0u; i < sizeof kid; ++i) kid_nonzero |= kid[i] != 0u;
    for (unsigned i = 0u; i < sizeof key; ++i) key_nonzero |= key[i] != 0u;
    check("session id", kid_nonzero && key_nonzero);

    /* Find the created session and read the packed attributes back.
       XOnlineMatchSearchGetResults(task, &array, &count): the middle argument
       receives the pointer array. */
    const uint32_t find_args[4] = {peek32(KID_OUT), peek32(KID_OUT + 4u), 0u, TASK_OUT};
    check("find returns S_OK", call(thunk_session_find, find_args, 4u) == 0u);
    const uint32_t find_task = peek32(TASK_OUT);
    check("find task", find_task != 0u && find_task != 0xffffffffu);
    check("find completes", wait_task(find_task) == TASK_SUCCESS);
    check("find results", call(thunk_search_results,
        (const uint32_t[]){find_task, RESULT_ARRAY, COUNT_OUT}, 3u) == 0u &&
        peek32(COUNT_OUT) == 1u);
    const uint32_t found = peek32(peek32(RESULT_ARRAY));
    check("found record kid", memcmp(&guest[found + 0x04u], kid, sizeof kid) == 0);
    check("found record key", memcmp(&guest[found + 0x30u], key, sizeof key) == 0);
    check("search record gate", peek32(found + 0x50u) == 0u);
    check("found record slots", peek32(found + 0x40u) == 3u && peek32(found + 0x44u) == 0u &&
        peek32(found + 0x48u) == 1u && peek32(found + 0x4cu) == 0u); /* open/open, filled/filled */
    check("found record xnaddr", guest[found + 0x0cu] == 10u && guest[found + 0x0fu] == 1u &&
        guest[found + 0x10u] == 10u && guest[found + 0x16u] == 0x00u && guest[found + 0x17u] == 0x50u);
    check("packed integer attribute", peek32(found + 0x54u) == 7u && peek32(found + 0x58u) == 0u);
    check("packed wide string attribute",
        memcmp(&guest[found + 0x54u + 24u], "D\0" "2\0" "\0\0", 6u) == 0);
    check("packed blob attribute", memcmp(&guest[found + 0x54u + 386u], "BLOB", 4u) == 0);
    close_task(find_task);

    /* Search (Quick Match / OptiMatch) returns the other players' sessions. */
    build_search_specs();
    check("search results length", call(thunk_search_len,
        (const uint32_t[]){25u, 27u, SPECS}, 3u) == 0x99e0u);
    const uint32_t search_args[7] = {1u, 25u, 7u, ATTRIBUTES, 0x99e0u, 0u, TASK_OUT};
    check("search returns S_OK", call(thunk_search, search_args, 7u) == 0u);
    const uint32_t search_task = peek32(TASK_OUT);
    check("search task", search_task != 0u && search_task != 0xffffffffu);
    check("search completes", wait_task(search_task) == TASK_SUCCESS);
    check("search results", call(thunk_search_results,
        (const uint32_t[]){search_task, RESULT_ARRAY, COUNT_OUT}, 3u) == 0u && peek32(COUNT_OUT) == 2u);
    const uint32_t array = peek32(RESULT_ARRAY);
    const uint32_t first = peek32(array), second = peek32(array + 4u);
    check("search records differ", first != second && peek32(first + 0x50u) == 0u && peek32(second + 0x50u) == 0u);
    check("search xnaddr is not loopback", guest[first + 0x0cu] == 10u && guest[first + 0x0fu] != 127u);
    check("parse copies the attributes", call(thunk_search_parse,
        (const uint32_t[]){first, peek32(first + 0x50u), 0u, DEST}, 4u) == 0u &&
        memcmp(&guest[DEST], &guest[first + 0x54u], RECOMP_LIVE_ATTRIBUTES_BYTES) == 0);
    close_task(search_task);

    /* A host's attributes are untrusted text: an unterminated 34-byte wide
       field gets its last unit cleared before the game parses the record. */
    corrupt_inner = presence_inner;
    corrupt_backend = *presence_inner;
    corrupt_backend.results = corrupting_results;
    recomp_live_backend_set(&corrupt_backend);
    check("a hostile search returns S_OK", call(thunk_search, search_args, 7u) == 0u);
    const uint32_t hostile_search = peek32(TASK_OUT);
    check("the hostile search completes", wait_task(hostile_search) == TASK_SUCCESS);
    check("its records", call(thunk_search_results,
        (const uint32_t[]){hostile_search, RESULT_ARRAY, COUNT_OUT}, 3u) == 0u && peek32(COUNT_OUT) == 2u);
    const uint32_t hostile_record = peek32(peek32(RESULT_ARRAY));
    check("a hostile wide string is terminated", guest[hostile_record + 0x54u + 24u + 32u] == 0u &&
        guest[hostile_record + 0x54u + 24u + 33u] == 0u && guest[hostile_record + 0x54u + 24u] == 0x41u);
    close_task(hostile_search);
    recomp_live_backend_set(&presence_backend);

    /* Update and delete the session we created. */
    const uint32_t update_args[10] = {peek32(KID_OUT), peek32(KID_OUT + 4u), 2u, 2u, 0u, 0u,
        27u, ATTRIBUTES, 0u, TASK_OUT};
    check("session update returns S_OK", call(thunk_session_update, update_args, 10u) == 0u);
    const uint32_t update_task = peek32(TASK_OUT);
    check("session update completes", wait_task(update_task) == TASK_SUCCESS);
    close_task(update_task);
    const uint32_t delete_args[4] = {peek32(KID_OUT), peek32(KID_OUT + 4u), 0u, TASK_OUT};
    check("session delete returns S_OK", call(thunk_session_delete, delete_args, 4u) == 0u);
    const uint32_t delete_task = peek32(TASK_OUT);
    check("session delete completes", wait_task(delete_task) == TASK_SUCCESS);
    close_task(delete_task);

    /* --- friends: a list with one friend and one invite --- */
    check("friends startup returns an HRESULT", call2(thunk_friends_startup, 0u, TASK_OUT) == 0u);
    const uint32_t friends_start = peek32(TASK_OUT);
    check("friends startup goes idle", wait_task(friends_start) == TASK_IDLE);
    close_task(friends_start);
    check("friends enumerate returns an HRESULT", call(thunk_friends_enumerate,
        (const uint32_t[]){0u, 0u, TASK_OUT}, 3u) == 0u);
    const uint32_t enumerate_task = peek32(TASK_OUT);
    check("friends enumerate has results", wait_task(enumerate_task) == TASK_RESULTS);
    check("friends list", call(thunk_friends_latest,
        (const uint32_t[]){0u, 100u, FRIENDS}, 3u) == 3u);
    const uint32_t ann = FRIENDS, bob = FRIENDS + 0x56u, cara = FRIENDS + 0xacu;
    uint64_t ann_xuid, bob_xuid;
    memcpy(&ann_xuid, &guest[ann], sizeof ann_xuid);
    memcpy(&bob_xuid, &guest[bob], sizeof bob_xuid);
    check("friend gamertag", strcmp((const char *)&guest[ann + 0x0cu], "Ann") == 0);
    /* The game's own bits: 1 online, 2 playing, 0x10 joinable (friend_ui_state
       turns 0x10 into its UI joinable bit, gated on no received invite). */
    check("friend online playing and joinable", peek32(ann + 0x1cu) == 0x00000013u);
    /* Presence[0] must equal g_presence_state_blob (0x7bfdf0), which the XBE
       data section initializes to 1. */
    check("friend presence", guest[ann + 0x34u] == 2u && guest[ann + 0x35u] == 1u);
    check("friend xuid", ann_xuid == 0x0009000000a1b2c3ull && bob_xuid == 0x0009000000a1b2c4ull);
    check("friend request flag", (peek32(bob + 0x1cu) & 0x80000000u) != 0u);
    check("request-only friend has no presence", guest[bob + 0x34u] == 0u);
    corrupt_inner = presence_inner;
    corrupt_backend = *presence_inner;
    corrupt_backend.friends = inflating_friends;
    recomp_live_backend_set(&corrupt_backend);
    check("an inflated friend count is clamped to the game's array", call(thunk_friends_latest,
        (const uint32_t[]){0u, 2u, FRIENDS}, 3u) == 2u);
    recomp_live_backend_set(&presence_backend);
    const uint8_t invite_kid[8] = {0x44u, 0x32u, 0x49u, 0x4eu, 0x56u, 0x01u, 0x03u, 0x02u};
    check("friend invite kid", memcmp(&guest[cara + 0x28u], invite_kid, sizeof invite_kid) == 0);
    check("friend received invite", peek32(cara + 0x1cu) == 0x08000001u);
    check("friend title", peek32(cara + 0x30u) == 0x54430006u);
    check("friends finish", call1(thunk_friends_finish, enumerate_task) == 0u &&
        wait_task(enumerate_task) == TASK_SUCCESS);
    close_task(enumerate_task);

    /* Friend operations no scripted run has reached. */
    poke32(FRIEND_RECORD, peek32(ann));
    poke32(FRIEND_RECORD + 4u, peek32(ann + 4u));
    check("friend request sent", call(thunk_friends_request,
        (const uint32_t[]){0u, peek32(ann), peek32(ann + 4u), 0u}, 4u) == 0u);
    recomp_live_pump(1000u);
    check("friend request visible", call(thunk_friends_latest,
        (const uint32_t[]){0u, 100u, FRIENDS}, 3u) == 3u &&
        (peek32(FRIENDS + 0x1cu) & 0x40000000u) != 0u);
    poke32(FRIEND_RECORD, peek32(bob));
    poke32(FRIEND_RECORD + 4u, peek32(bob + 4u));
    check("friend request answered", call(thunk_friends_answer_request,
        (const uint32_t[]){0u, FRIEND_RECORD, 1u}, 3u) == 0u);
    recomp_live_pump(1000u);
    check("answer clears the request", call(thunk_friends_latest,
        (const uint32_t[]){0u, 100u, FRIENDS}, 3u) == 3u &&
        (peek32(FRIENDS + 0x56u + 0x1cu) & 0x80000000u) == 0u);

    poke32(FRIEND_RECORD, peek32(ann));
    poke32(FRIEND_RECORD + 4u, peek32(ann + 4u));
    check("game invite", call(thunk_friends_invite,
        (const uint32_t[]){0u, 0x11223344u, 0x55667788u, 1u, FRIEND_RECORD}, 5u) == 0u);
    recomp_live_pump(1000u);
    check("invite flag and kid", call(thunk_friends_latest,
        (const uint32_t[]){0u, 100u, FRIENDS}, 3u) == 3u &&
        (peek32(FRIENDS + 0x1cu) & 0x04000000u) != 0u &&
        peek32(FRIENDS + 0x28u) == 0x11223344u && peek32(FRIENDS + 0x2cu) == 0x55667788u);
    check("revoke invite", call(thunk_friends_revoke,
        (const uint32_t[]){0u, 0x11223344u, 0x55667788u, 1u, FRIEND_RECORD}, 5u) == 0u);
    recomp_live_pump(1000u);
    check("invite revoked", call(thunk_friends_latest,
        (const uint32_t[]){0u, 100u, FRIENDS}, 3u) == 3u &&
        (peek32(FRIENDS + 0x1cu) & 0x04000000u) == 0u);
    check("answer game invite", call(thunk_friends_answer_invite,
        (const uint32_t[]){0u, FRIEND_RECORD, 1u}, 3u) == 0u);
    recomp_live_pump(1000u);
    check("join game", call2(thunk_friends_join, 0u, FRIEND_RECORD) == 0u);

    /* The accepted invite the matchmaker pushed for the local player. */
    poke32(FRIEND_RECORD, peek32(cara));
    poke32(FRIEND_RECORD + 4u, peek32(cara + 4u));
    check("accept the pushed invite", call(thunk_friends_answer_invite,
        (const uint32_t[]){0u, FRIEND_RECORD, 1u}, 3u) == 0u);
    recomp_live_pump(1000u);
    memset(&guest[OUT], 0, 0x80u);
    check("accepted invite", call1(thunk_friends_accepted, FRIEND_RECORD) == 0u);
    uint64_t invite_from;
    memcpy(&invite_from, &guest[FRIEND_RECORD], sizeof invite_from);
    check("accepted invite is the player's", invite_from == 0x0009000000a1b2c5ull);
    /* The game joins through the record's session id and title, and requires
       presence data before it will use the same-title path. */
    check("accepted invite session id", memcmp(&guest[FRIEND_RECORD + 0x28u], invite_kid,
        sizeof invite_kid) == 0 && peek32(FRIEND_RECORD + 0x30u) == 0x54430006u);
    check("accepted invite presence", guest[FRIEND_RECORD + 0x34u] == 2u &&
        guest[FRIEND_RECORD + 0x35u] == 1u);

    memset(&guest[TAG_OUT], 0xff, 0x40u);
    check("title name", call(thunk_friends_title_name,
        (const uint32_t[]){0x54430006u, 0u, 64u, TAG_OUT}, 4u) == 0u);
    check("title name is the game's title", wide_equals(TAG_OUT, "DEAD OR ALIVE ULTIMATE"));
    memset(&guest[TAG_OUT], 0xff, 0x40u);
    check("title name is truncated to the count", call(thunk_friends_title_name,
        (const uint32_t[]){0x54430006u, 1u, 5u, TAG_OUT}, 4u) == 0u && wide_equals(TAG_OUT, "DEAD"));
    memset(&guest[TAG_OUT], 0xff, 0x40u);
    check("unknown title has no name", call(thunk_friends_title_name,
        (const uint32_t[]){0x41550003u, 0u, 64u, TAG_OUT}, 4u) == 0u && peek16(TAG_OUT) == 0u);
    check("same title", call1(thunk_same_title, 0x54430006u) == 1u);
    check("other title is not the same", call1(thunk_same_title, 0x41550003u) == 0u);

    /* Remove the last friend. */
    poke32(FRIEND_RECORD, peek32(cara));
    poke32(FRIEND_RECORD + 4u, peek32(cara + 4u));
    check("friend removed", call2(thunk_friends_remove, 0u, FRIEND_RECORD) == 0u);
    recomp_live_pump(1000u);
    check("friend list shrinks", call(thunk_friends_latest,
        (const uint32_t[]){0u, 100u, FRIENDS}, 3u) == 2u);

    /* --- stats: write, read back with the pseudo ids, then the leaderboard ---
       The write record is the game's: board at +0xc, count +0x10, array +0x14.
       The score pseudo id (-2 = 0xfffe) is a real stored value; the gamertag
       (-3 = 0xfffd) is a wide-string pointer and the shim must not forward it. */
    set_stat_record(REQUEST, local_xuid, 1u, 3u, STAT_ENTRIES);
    set_stat_entry(STAT_ENTRIES + 0x00u, 1u, 2u, 42);
    set_stat_entry(STAT_ENTRIES + 0x10u, 2u, 2u, 3);
    set_stat_entry(STAT_ENTRIES + 0x20u, 0xfffeu, 2u, 99);
    const uint32_t write_args[4] = {1u, REQUEST, 0u, TASK_OUT};
    check("stat write starts", call(thunk_stat_write, write_args, 4u) == 0u);
    const uint32_t write_task = peek32(TASK_OUT);
    check("stat write task", write_task != 0u && write_task != 0xffffffffu);
    check("stat write completes", wait_task(write_task) == TASK_SUCCESS);
    close_task(write_task);

    /* The read record asks for the group's ids: two stored stats, then the
       gamertag, the score and the rank pseudo ids the library synthesizes from
       the row and board. */
    set_stat_record(REQUEST, local_xuid, 1u, 5u, STAT_ENTRIES);
    set_stat_entry(STAT_ENTRIES + 0x00u, 1u, 0u, 0);
    set_stat_entry(STAT_ENTRIES + 0x10u, 2u, 0u, 0);
    set_stat_entry(STAT_ENTRIES + 0x20u, 0xfffdu, 4u, 0);
    set_stat_entry(STAT_ENTRIES + 0x30u, 0xfffeu, 2u, 0);
    set_stat_entry(STAT_ENTRIES + 0x40u, 0xffffu, 0u, 0);
    const uint32_t read_args[4] = {1u, REQUEST, 0u, TASK_OUT};
    check("stat read starts", call(thunk_stat_read, read_args, 4u) == 0u);
    const uint32_t read_task = peek32(TASK_OUT);
    check("stat read completes", wait_task(read_task) == TASK_SUCCESS);
    check("stat read result", call(thunk_stat_read_result,
        (const uint32_t[]){read_task, 1u, REQUEST, 128u, TAG_OUT}, 5u) == 0u);
    check("stat values come back", peek16(STAT_ENTRIES) == 1u && peek32(STAT_ENTRIES + 4u) == 2u &&
        peek32(STAT_ENTRIES + 8u) == 42u && peek16(STAT_ENTRIES + 0x10u) == 2u &&
        peek32(STAT_ENTRIES + 0x18u) == 3u);
    check("stat read score pseudo", peek16(STAT_ENTRIES + 0x30u) == 0xfffeu &&
        peek32(STAT_ENTRIES + 0x34u) == 2u && peek32(STAT_ENTRIES + 0x38u) == 99u &&
        peek32(STAT_ENTRIES + 0x3cu) == 0u);
    check("stat read gamertag pseudo", peek32(STAT_ENTRIES + 0x24u) == 4u &&
        peek32(STAT_ENTRIES + 0x28u) == TAG_OUT &&
        memcmp(&guest[TAG_OUT], "L\0o\0c\0a\0l\0P\0l\0a\0y\0e\0r\0", 22u) == 0 &&
        peek16(TAG_OUT + 22u) == 0u);
    check("stat read rank pseudo", peek32(STAT_ENTRIES + 0x44u) == 1u &&
        peek32(STAT_ENTRIES + 0x48u) == 1u && peek32(STAT_ENTRIES + 0x4cu) == 0u);
    close_task(read_task);

    /* A stat the row has no value for reads as no data, not as zero. The game
       pre-fills every entry's type from its group before a read
       (ranking_stat_entries_init 0x294510), so an entry the service left alone
       would look like a real 0; the retail service echoed the id with type 0 and
       the shim does the same. */
    set_stat_record(REQUEST, local_xuid, 1u, 2u, STAT_ENTRIES);
    set_stat_entry(STAT_ENTRIES + 0x00u, 1u, 1u, 0);      /* pre-filled type and value, as the game does */
    set_stat_entry(STAT_ENTRIES + 0x10u, 0x7fffu, 1u, 7); /* an id the row never stored */
    check("stat read with an absent stat starts", call(thunk_stat_read, read_args, 4u) == 0u);
    const uint32_t absent_task = peek32(TASK_OUT);
    check("the stat read with an absent stat completes", wait_task(absent_task) == TASK_SUCCESS);
    check("its result", call(thunk_stat_read_result,
        (const uint32_t[]){absent_task, 1u, REQUEST, 128u, TAG_OUT}, 5u) == 0u);
    check("a stored stat is data", peek16(STAT_ENTRIES) == 1u && peek32(STAT_ENTRIES + 4u) == 2u &&
        peek32(STAT_ENTRIES + 8u) == 42u);
    check("an absent stat is no data", peek16(STAT_ENTRIES + 0x10u) == 0x7fffu &&
        peek32(STAT_ENTRIES + 0x14u) == 0u && peek32(STAT_ENTRIES + 0x18u) == 0u);
    close_task(absent_task);

    /* A host's stat row with an unusable type is zeroed, never a NULL string
       pointer the game would read. */
    set_stat_record(REQUEST, local_xuid, 1u, 1u, STAT_ENTRIES);
    set_stat_entry(STAT_ENTRIES, 1u, 0u, 0);
    corrupt_inner = presence_inner;
    corrupt_backend = *presence_inner;
    corrupt_backend.stat_results = corrupting_stats;
    recomp_live_backend_set(&corrupt_backend);
    check("a hostile stat read starts", call(thunk_stat_read, read_args, 4u) == 0u);
    const uint32_t hostile_read = peek32(TASK_OUT);
    check("the hostile stat read completes", wait_task(hostile_read) == TASK_SUCCESS);
    check("its result", call(thunk_stat_read_result,
        (const uint32_t[]){hostile_read, 1u, REQUEST, 128u, TAG_OUT}, 5u) == 0u);
    check("an unknown stat type is zeroed", peek16(STAT_ENTRIES) == 1u && peek32(STAT_ENTRIES + 4u) == 0u &&
        peek32(STAT_ENTRIES + 8u) == 0u);
    close_task(hostile_read);
    recomp_live_backend_set(&presence_backend);

    /* Two records in one call: the shim batches them into one backend read and
       gives each row its own 32-byte slot in the text buffer (the game's
       count * XONLINE_GAMERTAG_SIZE allocation). */
    set_stat_record(REQUEST, local_xuid, 1u, 2u, STAT_ENTRIES);
    set_stat_entry(STAT_ENTRIES + 0x00u, 1u, 0u, 0);
    set_stat_entry(STAT_ENTRIES + 0x10u, 0xfffdu, 4u, 0);
    set_stat_record(REQUEST2, 0x0009000000a1b2c3ull, 1u, 2u, STAT_ENTRIES2);
    set_stat_entry(STAT_ENTRIES2 + 0x00u, 1u, 0u, 0);
    set_stat_entry(STAT_ENTRIES2 + 0x10u, 0xfffdu, 4u, 0);
    check("stat read pair starts", call(thunk_stat_read,
        (const uint32_t[]){2u, REQUEST, 0u, TASK_OUT}, 4u) == 0u);
    const uint32_t pair_task = peek32(TASK_OUT);
    check("stat read pair completes", wait_task(pair_task) == TASK_SUCCESS);
    check("stat read pair result", call(thunk_stat_read_result,
        (const uint32_t[]){pair_task, 2u, REQUEST, 128u, TAG_OUT}, 5u) == 0u);
    check("stat read pair values", peek32(STAT_ENTRIES + 8u) == 42u && peek32(STAT_ENTRIES2 + 8u) == 25u);
    check("stat read pair tags", peek32(STAT_ENTRIES + 0x18u) == TAG_OUT &&
        peek32(STAT_ENTRIES2 + 0x18u) == TAG_OUT + 32u &&
        memcmp(&guest[TAG_OUT + 32u], "A\0n\0n\0", 6u) == 0);
    close_task(pair_task);

    /* Board 1's RANKINGS group: ids 0..1, gamertag, score, rank. The shim must
       fill the requested ids (not the backend row's own order), leave the
       struct's dwRank slot at +8 zero as the retail parser does, and put the
       rank in the -1 entry. */
    const uint32_t stat_ids = 0x4300u;
    const uint16_t group_ids[5] = {1u, 2u, 0xfffdu, 0xfffeu, 0xffffu};
    memcpy(&guest[stat_ids], group_ids, sizeof group_ids);
    const uint32_t leader_args[8] = {0u, 1u, 4u, 1u, 5u, stat_ids, 0u, TASK_OUT};
    check("leaderboard starts", call(thunk_leader, leader_args, 8u) == 0u);
    const uint32_t leader_task = peek32(TASK_OUT);
    check("leaderboard task", leader_task != 0u && leader_task != 0xffffffffu);
    check("leaderboard completes", wait_task(leader_task) == TASK_SUCCESS);
    const uint32_t leader_results[9] = {leader_task, 4u, LEADER_ROWS, 20u, LEADER_STATS,
        BOARD_OUT, ROWS_OUT, 128u, TAG_OUT};
    check("leaderboard results", call(thunk_leader_results, leader_results, 9u) == 0u);
    check("leaderboard board and count", peek32(BOARD_OUT) == 1u && peek32(ROWS_OUT) == 4u);
    check("leaderboard first row is the local player",
        peek32(LEADER_ROWS) == (uint32_t)local_xuid && peek32(LEADER_ROWS + 4u) == (uint32_t)(local_xuid >> 32));
    check("leaderboard dwRank slot stays zero", peek32(LEADER_ROWS + 0x08u) == 0u);
    check("leaderboard gamertag", memcmp(&guest[LEADER_ROWS + 0x0cu], "L\0o\0c\0a\0l\0", 10u) == 0);
    {
        /* Past the tag's NUL, the 16-unit field holds nothing of the host's stack. */
        unsigned unit = 0u;
        while (unit < 16u && peek16(LEADER_ROWS + 0x0cu + 2u * unit) != 0u) ++unit;
        bool zero = unit < 16u;
        for (; unit < 16u; ++unit) zero = zero && peek16(LEADER_ROWS + 0x0cu + 2u * unit) == 0u;
        check("leaderboard gamertag tail is zero", zero);
    }
    check("leaderboard stat", peek16(LEADER_STATS) == 1u && peek32(LEADER_STATS + 4u) == 2u &&
        peek32(LEADER_STATS + 8u) == 42u);
    check("leaderboard score pseudo", peek16(LEADER_STATS + 0x30u) == 0xfffeu &&
        peek32(LEADER_STATS + 0x34u) == 2u && peek32(LEADER_STATS + 0x38u) == 99u);
    check("leaderboard gamertag pseudo", peek32(LEADER_STATS + 0x24u) == 4u &&
        peek32(LEADER_STATS + 0x28u) == TAG_OUT &&
        memcmp(&guest[TAG_OUT], "L\0o\0c\0a\0l\0", 10u) == 0);
    check("leaderboard rank pseudo", peek32(LEADER_STATS + 0x44u) == 1u &&
        peek32(LEADER_STATS + 0x48u) == 1u);
    close_task(leader_task);

    /* --- group 7, the largest RANKINGS group (62 entries) round trips ---
       A write carries the group minus -1: ids 0..59, all int64, and the score
       -2, 61 entries. A read asks 62: ids 0..58, the gamertag -3, the score -2
       and the rank -1. The game sizes the entry buffer from
       g_ranking_stat_count 0xf1c944 (62 here) and the text buffer as the row
       count * 32 bytes; the shim must fill exactly the record's count and one
       32-byte tag per row. All values are above 2^32 so a truncated int64
       would show. */
    const int64_t group7_base = 0x100000000ll;
    const int64_t group7_score = 1234567890123ll;
    set_stat_record(REQUEST, local_xuid, 7u, 61u, GROUP7_STATS);
    for (uint16_t id = 0u; id < 60u; ++id)
        set_stat_entry(GROUP7_STATS + id * 16u, id, 2u, group7_base + id);
    set_stat_entry(GROUP7_STATS + 60u * 16u, 0xfffeu, 2u, group7_score);
    set_stat_entry(GROUP7_STATS + 61u * 16u, 0x7ffeu, 1u, 0); /* beyond the write's 61 */
    check("group 7 write starts",
        call(thunk_stat_write, (const uint32_t[]){1u, REQUEST, 0u, TASK_OUT}, 4u) == 0u);
    const uint32_t group7_write = peek32(TASK_OUT);
    check("group 7 write completes", wait_task(group7_write) == TASK_SUCCESS);
    close_task(group7_write);

    set_stat_record(REQUEST, local_xuid, 7u, 62u, GROUP7_STATS);
    for (uint16_t id = 0u; id < 59u; ++id) set_stat_entry(GROUP7_STATS + id * 16u, id, 0u, 0);
    set_stat_entry(GROUP7_STATS + 59u * 16u, 0xfffdu, 4u, 0);
    set_stat_entry(GROUP7_STATS + 60u * 16u, 0xfffeu, 2u, 0);
    set_stat_entry(GROUP7_STATS + 61u * 16u, 0xffffu, 0u, 0);
    set_stat_entry(GROUP7_STATS + 62u * 16u, 0x7fffu, 1u, 0); /* beyond the read's 62 */
    check("group 7 read starts",
        call(thunk_stat_read, (const uint32_t[]){1u, REQUEST, 0u, TASK_OUT}, 4u) == 0u);
    const uint32_t group7_read = peek32(TASK_OUT);
    check("group 7 read completes", wait_task(group7_read) == TASK_SUCCESS);
    check("group 7 read result", call(thunk_stat_read_result,
        (const uint32_t[]){group7_read, 1u, REQUEST, 32u, TAG_OUT}, 5u) == 0u);
    bool group7_ok = true;
    for (uint16_t id = 0u; id < 59u; ++id) {
        const uint32_t entry = GROUP7_STATS + id * 16u;
        group7_ok = group7_ok && peek16(entry) == id && peek32(entry + 4u) == 2u &&
            peek64(entry + 8u) == group7_base + id;
    }
    check("group 7 keeps all 59 stored ids", group7_ok);
    check("group 7 gamertag pseudo", peek16(GROUP7_STATS + 59u * 16u) == 0xfffdu &&
        peek32(GROUP7_STATS + 59u * 16u + 4u) == 4u && peek32(GROUP7_STATS + 59u * 16u + 8u) == TAG_OUT &&
        memcmp(&guest[TAG_OUT], "L\0o\0c\0a\0l\0P\0l\0a\0y\0e\0r\0", 22u) == 0);
    check("group 7 score pseudo", peek16(GROUP7_STATS + 60u * 16u) == 0xfffeu &&
        peek32(GROUP7_STATS + 60u * 16u + 4u) == 2u &&
        peek64(GROUP7_STATS + 60u * 16u + 8u) == group7_score);
    check("group 7 rank pseudo", peek32(GROUP7_STATS + 61u * 16u + 4u) == 1u &&
        peek32(GROUP7_STATS + 61u * 16u + 8u) == 1u &&
        peek32(GROUP7_STATS + 61u * 16u + 12u) == 0u);
    check("group 7 wrote only the record's 62 entries", peek16(GROUP7_STATS + 62u * 16u) == 0x7fffu);
    close_task(group7_read);

    /* Group 7's own leaderboard spec: ids 0..59, -2 and -1, 62 entries a row. */
    uint16_t group7_ids[62];
    for (uint16_t id = 0u; id < 60u; ++id) group7_ids[id] = id;
    group7_ids[60] = 0xfffeu;
    group7_ids[61] = 0xffffu;
    memcpy(&guest[GROUP7_STAT_IDS], group7_ids, sizeof group7_ids);
    check("group 7 leaderboard starts",
        call(thunk_leader, (const uint32_t[]){0u, 1u, 4u, 7u, 62u, GROUP7_STAT_IDS, 0u, TASK_OUT}, 8u) == 0u);
    const uint32_t group7_leader = peek32(TASK_OUT);
    check("group 7 leaderboard completes", wait_task(group7_leader) == TASK_SUCCESS);
    check("group 7 leaderboard results", call(thunk_leader_results,
        (const uint32_t[]){group7_leader, 4u, LEADER_ROWS, 62u, GROUP7_LEADER_STATS,
            BOARD_OUT, ROWS_OUT, 32u, TAG_OUT}, 9u) == 0u);
    check("group 7 leaderboard board and rows", peek32(BOARD_OUT) == 7u && peek32(ROWS_OUT) == 1u);
    check("group 7 leaderboard first and last stored ids",
        peek16(GROUP7_LEADER_STATS) == 0u && peek64(GROUP7_LEADER_STATS + 8u) == group7_base &&
        peek16(GROUP7_LEADER_STATS + 59u * 16u) == 59u &&
        peek64(GROUP7_LEADER_STATS + 59u * 16u + 8u) == group7_base + 59);
    check("group 7 leaderboard score and rank",
        peek16(GROUP7_LEADER_STATS + 60u * 16u) == 0xfffeu &&
        peek64(GROUP7_LEADER_STATS + 60u * 16u + 8u) == group7_score &&
        peek16(GROUP7_LEADER_STATS + 61u * 16u) == 0xffffu &&
        peek32(GROUP7_LEADER_STATS + 61u * 16u + 8u) == 1u);
    close_task(group7_leader);

    /* --- feedback and the string logon verifies --- */
    const uint32_t feedback_args[8] = {0u, peek32(USERS), peek32(USERS + 4u),
        0u, 5u, STAT_ENTRIES, 0u, TASK_OUT};
    check("feedback returns S_OK", call(thunk_feedback, feedback_args, 8u) == 0u);
    const uint32_t feedback_task = peek32(TASK_OUT);
    check("feedback completes", wait_task(feedback_task) == TASK_SUCCESS);
    close_task(feedback_task);
    check("string verify returns S_OK", call(thunk_string_verify,
        (const uint32_t[]){0u, 0u, 0u, 0u, TASK_OUT}, 5u) == 0u);
    const uint32_t verify_task = peek32(TASK_OUT);
    check("string verify completes", verify_task != 0u && wait_task(verify_task) == TASK_SUCCESS);
    close_task(verify_task);

    /* --- voice, keys, QoS --- */
    check("no voice streams", call2(thunk_create_stream, 0u, OUT) == E_FAIL && peek32(OUT) == 0u);
    check("register key", call2(thunk_register_key, KID_OUT, KEY_OUT) == 0u);
    check("unregister key", call1(thunk_unregister_key, KID_OUT) == 0u);
    check("qos listen", call(thunk_qos_listen,
        (const uint32_t[]){KID_OUT, 0u, 0u, 0u, 1u}, 5u) == 0u);
    uint32_t qos_args[12] = {1u, 0u, 0u, 0u, 0u, 0u, 0u, 8u, 0u, 0u, 0u, OUT + 0x100u};
    check("qos lookup", call(thunk_qos_lookup, qos_args, 12u) == 0u);
    const uint32_t qos = peek32(OUT + 0x100u);
    check("qos handle", qos != 0u && peek32(qos + 4u) == 0u && peek32(qos) == 1u);
    check("qos entries complete", (guest[qos + 8u] & 1u) == 1u && peek16(qos + 8u + 0x0eu) == 50u);

    /* A search probes one address per result, up to 25. The handle must carry
       all 25 entries and a second lookup must not alias the first one. */
    qos_args[0] = 25u;
    check("qos lookup 25", call(thunk_qos_lookup, qos_args, 12u) == 0u);
    const uint32_t qos25 = peek32(OUT + 0x100u);
    const uint32_t qos25_last = qos25 + 8u + 24u * 0x18u;
    check("qos 25 handle is separate", qos25 != 0u && qos25 != qos);
    check("qos 25 entries", peek32(qos25) == 25u && peek32(qos25 + 4u) == 0u &&
        (guest[qos25 + 8u] & 1u) == 1u && (guest[qos25_last] & 1u) == 1u &&
        peek16(qos25_last + 0x0eu) == 50u);
    check("qos entry past 8 is inside the handle", qos25_last + 0x18u <= qos25 + 8u + 25u * 0x18u);
    check("qos release", call1(thunk_qos_release, qos) == 0u);
    check("qos release 25", call1(thunk_qos_release, qos25) == 0u);

    /* A released handle can be reused, and unqueried entries stay incomplete. */
    qos_args[0] = 2u;
    check("qos lookup 2", call(thunk_qos_lookup, qos_args, 12u) == 0u);
    const uint32_t qos2 = peek32(OUT + 0x100u);
    check("qos 2 handle", qos2 != 0u && peek32(qos2) == 2u &&
        (guest[qos2 + 8u] & 1u) == 1u && (guest[qos2 + 8u + 0x18u] & 1u) == 1u &&
        (guest[qos2 + 8u + 2u * 0x18u] & 1u) == 0u);
    check("qos release 2", call1(thunk_qos_release, qos2) == 0u);

    /* --- XNet identity, link and startup --- */
    check("link up", call0(thunk_link_status) == 0x0bu);
    memset(&guest[OUT], 0, 0x40u);
    check("title xnaddr", call1(thunk_title_xnaddr, OUT) == 0xe6u);
    check("xnaddr is the virtual address", guest[OUT] == 10u && guest[OUT + 3u] == 1u &&
        guest[OUT + 4u] == 10u && guest[OUT + 10u] == 0x00u && guest[OUT + 11u] == 0x50u &&
        guest[OUT + 12u] == 0xf2u);
    memset(&guest[OUT], 0, 0x40u);
    check("random bytes", call2(thunk_random, OUT, 16u) == 0u);
    int random_nonzero = 0;
    for (unsigned i = 0u; i < 16u; ++i) random_nonzero |= guest[OUT + i] != 0u;
    check("random fills", random_nonzero);
    check("xnet startup", call1(thunk_xnet_startup, OUT) == 0u);
    check("wsa startup", call2(thunk_wsa_startup, 0x202u, OUT) == 0u);
    check("xonline startup", call1(thunk_xonline_startup, 0u) == 0u);

    /* A server code that is not an HRESULT failure must not read as success in
       a guest task: it reports E_FAIL. */
    corrupt_inner = presence_inner;
    corrupt_backend = *presence_inner;
    corrupt_backend.poll = corrupting_poll;
    recomp_live_backend_set(&corrupt_backend);
    corrupt_polls = 1u;
    const uint32_t bad_mutelist[6] = {0u, 250u, 0u, TASK_OUT, MUTES, COUNT_OUT};
    check("a task with a bad server code starts", call(thunk_mutelist_get, bad_mutelist, 6u) == 0u);
    const uint32_t bad_task = peek32(TASK_OUT);
    check("a non-failure server code becomes E_FAIL", wait_task(bad_task) == E_FAIL);
    close_task(bad_task);
    recomp_live_backend_set(&presence_backend);

    /* Logging off forgets every peer and route the account introduced. */
    const unsigned forgotten = forget_calls;
    check("xonline cleanup", call0(thunk_xonline_cleanup) == 0u && forget_calls == forgotten + 1u &&
        forget_ina == 0u);

    /* --- handles the game closes twice, or closes with none --- */
    check("close zero", call1(thunk_task_close, 0u) == 0u);
    check("close unknown", call1(thunk_task_close, 0x12345678u) == 0u);
    check("continue unknown", call1(thunk_task_continue, 0x12345678u) == E_INVALIDARG);
    check("logon results unknown", call1(thunk_logon_results, 0x12345678u) == E_INVALIDARG);

    recomp_live_backend_set(NULL);
    check("live is off again", recomp_live_lookup(thunk_get_users) == NULL);
    if (failures != 0) fprintf(stderr, "%d failure(s)\n", failures);
    else fprintf(stderr, "live xapi: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
