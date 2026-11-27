/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_xapi.h"
#include "live_backend.h"
#include "kernel_abi.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

/* live_transport.c owns the peer table; a logoff forgets every route it holds.
   The fallback at the bottom of this file covers the targets that link this
   guest API without the transport, as recomp_live_transport_lookup's does. */
void recomp_live_transport_forget(uint32_t virtual_ina);

/* Set by the runner: the guest logged on, or closed the task that carried the
   logon. A failed logon the game then closes counts as leaving. */
void (*recomp_live_online_hook)(bool online) = NULL;

static const void *live_logon_task; /* the task of the newest logon */

static void live_online_changed(bool online)
{
    if (recomp_live_online_hook != NULL) recomp_live_online_hook(online);
}

/* The guest's statically linked XOnline/XNet service, replaced at the API
   boundary (docs/xbox-live.md). Every entry is a stdcall thunk whose argument
   count was read from the function's RET in the retail executable; the
   handlers read their arguments with kernel_arg() and finish with
   kernel_return(count, result), which applies the same cleanup.

   Guest layouts are named below, from the retail call sites. */
enum {
    LIVE_USER_RECORD_BYTES = 0x70,
    LIVE_LOGON_USERS = 4, /* XOnlineLogon takes one user per controller */
    LIVE_USER_XUID = 0x00,
    LIVE_USER_FLAGS = 0x08,
    LIVE_USER_GAMERTAG = 0x0c,
    LIVE_USER_HRESULT = 0x6c,

    LIVE_SEARCH_RECORD_BYTES = 0x2f4,
    LIVE_SEARCH_KID = 0x04,
    LIVE_SEARCH_XNADDR = 0x0c,
    LIVE_SEARCH_KEY = 0x30,
    LIVE_SEARCH_SLOTS = 0x40,
    LIVE_SEARCH_GATE = 0x50,
    LIVE_SEARCH_ATTRIBUTES = 0x54,
    LIVE_SEARCH_HEADER_BYTES = LIVE_SEARCH_ATTRIBUTES,

    LIVE_ATTRIBUTE_BYTES = RECOMP_LIVE_ATTRIBUTES_BYTES,
    LIVE_ATTRIBUTE_COUNT = 27,
    LIVE_ATTRIBUTE_ENTRY_BYTES = 16,

    /* XONLINE_FRIEND, 0x56 bytes: the library's GetLatest copies 0x15 dwords
       and a word out of its own 0x74-byte record (xonline_friends_get_latest,
       0x39f69e), which fills the gamertag at +0x0c (FUN_003a0734) and the
       presence at +0x34/+0x35 (xonline_friend_record_update, 0x3a0559). The
       game reads +0x0c through get_friend_entry (0x2a0010, record + 0xc), the
       state flags at +0x1c through get_friend_id/friend_invite_state/
       friend_ui_state (0x2a0030), the session id at +0x28 and the title at
       +0x30 (XOnlineMatchSessionFindFromID takes record +0x28/+0x2c in
       online_find_match_session_from_id). */
    LIVE_FRIEND_RECORD_BYTES = 0x56,
    LIVE_FRIEND_XUID = 0x00,
    LIVE_FRIEND_KIND = 0x08, /* matched against the mutelist's kind, low 2 bits */
    LIVE_FRIEND_GAMERTAG = 0x0c,
    LIVE_FRIEND_FLAGS = 0x1c,
    LIVE_FRIEND_KID = 0x28,
    LIVE_FRIEND_TITLE = 0x30,
    LIVE_FRIEND_PRESENCE_LENGTH = 0x34,
    LIVE_FRIEND_PRESENCE = 0x35,
    LIVE_FRIEND_PRESENCE_BYTES = 2, /* what the game sends for itself (0x7bfdf0) */
    /* g_presence_state_blob (0x7bfdf0), the first of those two bytes: the XBE
       data section initializes it to 1 and nothing writes it. The same byte is
       the game number in friend_title_name's L"DEAD OR ALIVE %d ULTIMATE". */
    LIVE_FRIEND_SELF_PRESENCE = 1,

    LIVE_MUTE_RECORD_BYTES = 0x10,

    /* The record XOnlineStatWrite/XOnlineStatRead take: XUID at +0 (the user
       flag dword at +8 is skipped), board +0xc, stat count +0x10, stat array
       +0x14. The retail serializer xonline_stat_write_request 0x3a4d86 writes
       the header as context, xuid, board, count; xonline_stat_read_request
       0x3a4f60 reads the same. online_start_stat_read 0x2a36d0 passes query+8
       as the board, and ranking_read_records_build 0x299520 fills
       board/count/array from the query at +8/+0x18/+0x28. */
    LIVE_STAT_RECORD_BYTES = 0x18,
    LIVE_STAT_XUID = 0x00,
    LIVE_STAT_BOARD = 0x0c,
    LIVE_STAT_COUNT = 0x10,
    LIVE_STAT_ARRAY = 0x14,
    LIVE_STAT_ENTRY_BYTES = 0x10,
    LIVE_STAT_ID = 0x00,
    LIVE_STAT_TYPE = 0x04,  /* the library writes this as a dword */
    LIVE_STAT_VALUE = 0x08,

    /* One row of XOnlineStatLeaderEnumerateGetResults. The retail parser
       xonline_stat_leader_results_parse 0x3a46aa clears the dword at +8 (the
       struct's dwRank slot) and copies the wide gamertag to +0xc. The game
       reads the rank from pseudo stat -1 (ranking_results_apply 0x2b6740
       stores that entry's value as the row's rank), so +8 stays zero exactly
       as the library leaves it. */
    LIVE_LEADER_ROW_BYTES = 0x2c,
    LIVE_LEADER_ROW_XUID = 0x00,
    LIVE_LEADER_ROW_RANK = 0x08,
    LIVE_LEADER_ROW_GAMERTAG = 0x0c,
    LIVE_LEADER_GAMERTAG_UNITS = 16,

    /* XNQOS: dwNumProbes at +0, dwNumPending at +4, then one 0x18-byte probe
       entry per queried address from +8. The game reads entry+0 (bit 0 =
       complete, bit 2 = error) and entry+0xe (u16 median RTT in ms); see the
       comment above live_qos_lookup. A search can query all 25 result slots,
       so one handle must carry 25 entries, not eight. */
    LIVE_QOS_HANDLES = 8, /* one search plus up to seven session peers */
    LIVE_QOS_ENTRIES = RECOMP_LIVE_MAX_RESULTS,
    LIVE_QOS_ENTRY_BYTES = 0x18,
    LIVE_QOS_BYTES = 8 + LIVE_QOS_ENTRIES * LIVE_QOS_ENTRY_BYTES,
    LIVE_QOS_FLAGS = 0x00,
    LIVE_QOS_MEDIAN_RTT = 0x0e,
    LIVE_QOS_DEFAULT_RTT_MS = 50, /* (50*3+199)/100 = 3, the shipped delay */

    LIVE_TASK_SLOTS = 16,
    LIVE_TASK_REQUESTS = 4,
    LIVE_DETACHED_REQUESTS = 8,
};

/* Profile binding seam (temporary): a save keeps the XUID it
   was created with at 0x7ef8e0 + 0xabe8. A profile made under another account
   makes the account page say the account cannot be found, so the user query
   writes the logged-on player's XUID over it in memory. The save itself still
   carries the old value; the seam goes when profiles learn the account. */
enum { LIVE_PROFILE_XUID = 0x007ef8e0u + 0xabe8u };

/* The title this executable is. XOnlineStartup's initializer FUN_00393e99
   copies it from *(XBE certificate + 8) into the library singleton and
   XOnlineTitleIdIsSameTitle compares against that dword; the certificate at
   0x10184 in DOA2.xbe holds 0x54430006 and an empty name field, so the
   title-name query answers with the game's own name. */
enum { LIVE_TITLE_ID = 0x54430006u };
static const char live_title_name[] = "DEAD OR ALIVE ULTIMATE";

/* Results the game branches on, not HRESULTs. */
enum {
    LIVE_TASK_RUNNING = 0,
    LIVE_TASK_SUCCESS = 0x001500f0u,
    LIVE_TASK_RESULTS = 0x001500f1u, /* friends enumerate: GetLatest now */
    /* Startup tasks stay alive: online_logoff (0x2a4010) continues the
       friends startup task until it reads this code. */
    LIVE_TASK_IDLE = 0x001500f2u,
    LIVE_TASK_LOGON_SUCCESS = 0x001510f0u,
    LIVE_E_INVALIDARG = 0x80150005u,
    LIVE_E_FAIL = 0x80004005u,
};

/* The negative stat ids the retail library fills from the response instead of
   from stored stats (xonline_stat_leader_results_parse 0x3a46aa for
   leaderboard rows, xonline_stat_read_results_parse 0x3a441a for stat reads).
   The game's RANKINGS groups ask for them: group table 0x7bf8e0, types
   0x7bf900, boards 0x7bf940. The guest sees them as signed shorts. */
enum {
    LIVE_STAT_PSEUDO_BOARD = 0xfffc,    /* -4: the board id, from the response header */
    LIVE_STAT_PSEUDO_GAMERTAG = 0xfffd, /* -3: wide gamertag, copied to the text buffer */
    LIVE_STAT_PSEUDO_SCORE = 0xfffe,    /* -2: the row's 64-bit score */
    LIVE_STAT_PSEUDO_RANK = 0xffff,     /* -1: the row's rank */
};

/* XONLINE_STAT types; DAT_01068608 sizes them 0/4/8/8/65. */
enum {
    LIVE_STAT_TYPE_INT32 = 1,
    LIVE_STAT_TYPE_INT64 = 2,
    LIVE_STAT_TYPE_DOUBLE = 3,
    LIVE_STAT_TYPE_STRING = 4, /* wide string pointer in the guest entry */
};

/* The spec array passed to XOnlineMatchSearchResultsLen (at 0x7bfd00) makes 17
   of the 27 attributes wide strings of 34 bytes; the rest are 8-byte integers
   or the one blob, which the game passes 8 bytes for too. Index order is the
   game's create/update order (DAT_00f24918), also the parse order. */
static const uint8_t live_attribute_bytes[LIVE_ATTRIBUTE_COUNT] = {
    8, 8, 8, 34, 34, 34, 34, 34, 34, 34, 34, 34, 8, 8, 8, 8, 8, 8, 8, 34, 34, 34, 34, 34, 34, 34, 34};

/* The wide-string fields are fixed 34-byte slots, and a host's copy is not to
   be trusted: force a terminating NUL into each one's last unit before the game
   parses the record with XOnlineMatchSearchParse. */
static void live_terminate_attributes(uint8_t *attributes, uint32_t size)
{
    uint32_t offset = 0u;
    for (unsigned i = 0u; i < LIVE_ATTRIBUTE_COUNT; ++i) {
        const uint32_t field = live_attribute_bytes[i];
        if (offset + field > size) break;
        if (field == 34u) {
            attributes[offset + field - 2u] = 0u;
            attributes[offset + field - 1u] = 0u;
        }
        offset += field;
    }
}

/* Task kinds: which guest out-pointers a finished task fills and whether its
   backend request stays for a result read. */
enum {
    LIVE_KIND_OTHER = 1,
    LIVE_KIND_LOGON,
    LIVE_KIND_SESSION_CREATE,
    LIVE_KIND_SESSION_UPDATE,
    LIVE_KIND_SESSION_DELETE,
    LIVE_KIND_SEARCH,
    LIVE_KIND_FIND,
    LIVE_KIND_FRIENDS_STARTUP,
    LIVE_KIND_FRIENDS_ENUMERATE,
    LIVE_KIND_MUTELIST_STARTUP,
    LIVE_KIND_MUTELIST_GET,
    LIVE_KIND_FEEDBACK,
    LIVE_KIND_STAT_WRITE,
    LIVE_KIND_STAT_READ,
    LIVE_KIND_LEADER,
};

typedef struct LiveTask {
    uint32_t handle;
    uint32_t kind;
    RecompLiveRequest requests[LIVE_TASK_REQUESTS];
    unsigned request_count;
    uint32_t out[2];  /* guest out-pointers a finished task fills */
    uint32_t capacity;
    uint32_t board;
    uint32_t stat_count;
    uint32_t stat_ids; /* the leaderboard's requested id list (shorts) */
    uint32_t final;
    bool complete;
    bool keep; /* the request still holds a result the guest reads */
} LiveTask;

static LiveTask live_tasks[LIVE_TASK_SLOTS];
static uint32_t live_next_handle = 0x4c000001u;
static uint32_t live_logon_users;
static uint32_t live_logon_slot; /* which of the four XONLINE_USER records */
static uint32_t live_search_scratch;
static uint32_t live_search_pointers;
static uint32_t live_qos_pool;
static bool live_qos_used[LIVE_QOS_HANDLES];
static uint32_t live_notification_state;
static uint8_t live_notification_kid[8];
static bool live_notification_valid;
static RecompLiveRequest live_detached[LIVE_DETACHED_REQUESTS];
static uint8_t live_attributes[LIVE_ATTRIBUTE_BYTES];
static uint32_t live_attributes_size = LIVE_ATTRIBUTE_BYTES;
static RecompLiveSession live_sessions[RECOMP_LIVE_MAX_RESULTS];
static RecompLiveLeaderRow live_rows[RECOMP_LIVE_MAX_LEADER_ROWS];
static RecompLiveFriend live_friends[RECOMP_LIVE_MAX_FRIENDS];
static uint64_t live_mutes[RECOMP_LIVE_MAX_MUTES];

/* --- guest memory --- */

static void live_load(void *out, uint32_t address, size_t size)
{
    if (address == 0u || size == 0u) return;
    memcpy(out, recomp_memory(address, size), size);
}

static void live_store(uint32_t address, const void *data, size_t size)
{
    if (address == 0u || size == 0u) return;
    memcpy(recomp_memory(address, size), data, size);
}

static uint32_t live_u32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, recomp_memory(address, sizeof value), sizeof value);
    return value;
}

static uint16_t live_u16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, recomp_memory(address, sizeof value), sizeof value);
    return value;
}

static void live_set_u32(uint32_t address, uint32_t value)
{
    if (address == 0u) return;
    memcpy(recomp_memory(address, sizeof value), &value, sizeof value);
}

static const RecompLiveBackend *live_backend(void)
{
    return recomp_live_backend();
}

static void live_copy_tag(char *destination, const char *source, unsigned size)
{
    unsigned length = 0u;
    for (; source != NULL && source[length] != '\0' && length + 1u < size; ++length) destination[length] = source[length];
    destination[length] = '\0';
}

/* Guest scratch, allocated once from the kernel host's pool (QoS buffers and
   the search records the game reads). */
static uint32_t live_scratch(uint32_t *slot, uint32_t bytes)
{
    if (*slot == 0u) *slot = recomp_kernel_allocate_pool(bytes);
    return *slot;
}

/* --- XNADDR and user records --- */

static void live_fill_xnaddr(uint8_t xnaddr[0x24], const RecompLiveAddress *address)
{
    memset(xnaddr, 0, 0x24u);
    memcpy(xnaddr + 0x00, &address->virtual_ina, 4u);
    memcpy(xnaddr + 0x04, &address->virtual_ina, 4u);
    const uint16_t port = (uint16_t)((address->port >> 8) | (address->port << 8));
    memcpy(xnaddr + 0x08, &port, 2u);
    memcpy(xnaddr + 0x0a, address->mac, 6u);
}

static void live_write_user(uint32_t address, const RecompLiveUser *user)
{
    uint8_t record[LIVE_USER_RECORD_BYTES];
    memset(record, 0, sizeof record);
    memcpy(record + LIVE_USER_XUID, &user->xuid, sizeof user->xuid);
    live_copy_tag((char *)record + LIVE_USER_GAMERTAG, user->gamertag, RECOMP_LIVE_GAMERTAG_BYTES);
    live_store(address, record, sizeof record);
}

static void live_bind_profile_xuid(uint64_t xuid)
{
    uint64_t saved;
    live_load(&saved, LIVE_PROFILE_XUID, sizeof saved);
    if (saved == xuid) return;
    live_store(LIVE_PROFILE_XUID, &xuid, sizeof xuid);
}

/* --- task model --- */

/* Handles are opaque small integers. The game's own task helpers only pass
   them back to XOnlineTaskContinue/Close:
   online_task_continue 0x29f500, online_task_release 0x29f520 and 0x2935c0,
   online_task_array_close_all 0x2a1830 and online_task_close_all 0x2a3890
   load, store and compare the dword; none reads through it. The only task
   object dereferences are inside the retail library behind the replaced
   thunks: XOnlineTaskContinue 0x393b80 jumps to FUN_0039c7af 0x39c7af which
   calls handle[0], XOnlineTaskClose 0x393b8b jumps to FUN_0039c7c4 0x39c7c4
   which calls handle[1], and FUN_0039c7dc loops on one. The game never calls
   those helpers directly, so a handle only has to be non-zero and unique.
   Backing it with guest memory is not needed. The retail-created task objects
   at 0xf1ca64 (extended friend/voicemail paths) are a separate problem: they
   are not ours, and live_notification_context below keeps the game off the
   extended friends path so it uses the thunks this file owns. */

static LiveTask *live_task_find(uint32_t handle)
{
    if (handle == 0u) return NULL;
    for (unsigned i = 0u; i < LIVE_TASK_SLOTS; ++i)
        if (live_tasks[i].handle == handle) return &live_tasks[i];
    return NULL;
}

static void live_task_drop_requests(LiveTask *task)
{
    const RecompLiveBackend *backend = live_backend();
    if (backend != NULL && backend->release != NULL)
        for (unsigned i = 0u; i < task->request_count; ++i)
            if (task->requests[i] != 0u) backend->release(backend->self, task->requests[i]);
    memset(task->requests, 0, sizeof task->requests);
    task->request_count = 0u;
}

static void live_task_release(LiveTask *task)
{
    live_task_drop_requests(task);
    memset(task, 0, sizeof *task);
}

static LiveTask *live_task_new(uint32_t kind)
{
    for (unsigned i = 0u; i < LIVE_TASK_SLOTS; ++i) {
        if (live_tasks[i].handle != 0u) continue;
        LiveTask *task = &live_tasks[i];
        memset(task, 0, sizeof *task);
        task->handle = live_next_handle++;
        if (task->handle == 0u) task->handle = live_next_handle++;
        task->kind = kind;
        return task;
    }
    return NULL;
}

static bool live_task_add(LiveTask *task, RecompLiveRequest request)
{
    if (request == 0u || task->request_count >= LIVE_TASK_REQUESTS) return false;
    task->requests[task->request_count++] = request;
    return true;
}

static uint32_t live_task_finished(LiveTask *task);

static void live_task_complete_now(LiveTask *task)
{
    task->complete = true;
    task->final = live_task_finished(task);
}

static void live_finish_logon(void)
{
    const RecompLiveBackend *backend = live_backend();
    RecompLiveUser user;
    if (live_logon_users == 0u || backend == NULL || backend->local_user == NULL) return;
    if (!backend->local_user(backend->self, &user)) return;
    /* online_logon 0x29f7e0 copies the selected account into the slot chosen
       on the account page (its second argument), and online_update 0x2a5530
       later indexes the same slot (DAT_00f1cb0c) for the gamertag and the
       per-user logon result at +0x6c. Write the record where the game put the
       account, not always at slot 0. */
    live_write_user(live_logon_users + live_logon_slot * LIVE_USER_RECORD_BYTES, &user);
    live_bind_profile_xuid(user.xuid);
}

static void live_finish_mutelist(LiveTask *task)
{
    const RecompLiveBackend *backend = live_backend();
    if (backend == NULL || backend->mutes == NULL) return;
    const uint32_t capacity = task->capacity < RECOMP_LIVE_MAX_MUTES ? task->capacity : RECOMP_LIVE_MAX_MUTES;
    uint32_t count = backend->mutes(backend->self, live_mutes, capacity);
    if (count > capacity) count = capacity;
    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t record[LIVE_MUTE_RECORD_BYTES];
        memset(record, 0, sizeof record);
        memcpy(record, &live_mutes[i], sizeof live_mutes[i]);
        live_store(task->out[0] + i * LIVE_MUTE_RECORD_BYTES, record, sizeof record);
    }
    live_set_u32(task->out[1], count);
}

static uint32_t live_task_finished(LiveTask *task)
{
    uint32_t result = LIVE_TASK_SUCCESS;
    switch (task->kind) {
    case LIVE_KIND_LOGON:
        live_finish_logon();
        result = LIVE_TASK_LOGON_SUCCESS;
        break;
    case LIVE_KIND_FRIENDS_ENUMERATE:
        result = LIVE_TASK_RESULTS;
        break;
    case LIVE_KIND_FRIENDS_STARTUP:
    case LIVE_KIND_MUTELIST_STARTUP:
        result = LIVE_TASK_IDLE;
        break;
    case LIVE_KIND_MUTELIST_GET:
        live_finish_mutelist(task);
        break;
    case LIVE_KIND_SESSION_CREATE:
    case LIVE_KIND_SEARCH:
    case LIVE_KIND_FIND:
    case LIVE_KIND_STAT_READ:
    case LIVE_KIND_LEADER:
        task->keep = true; /* a result read follows */
        break;
    default:
        break;
    }
    if (!task->keep) live_task_drop_requests(task);
    return result;
}

/* A server code that is not an HRESULT failure (high bit clear) would read as
   success in a guest task result, so only a real failure keeps its value. */
static uint32_t live_task_hresult(uint32_t hresult)
{
    return hresult != 0u && (hresult & 0x80000000u) != 0u ? hresult : LIVE_E_FAIL;
}

static uint32_t live_task_poll(LiveTask *task)
{
    const RecompLiveBackend *backend = live_backend();
    if (task->complete) return task->final;
    if (task->request_count != 0u && backend != NULL && backend->poll != NULL) {
        bool pending = false;
        for (unsigned i = 0u; i < task->request_count; ++i) {
            uint32_t hresult = 0u;
            const RecompLiveStatus status = backend->poll(backend->self, task->requests[i], &hresult);
            if (status == RECOMP_LIVE_PENDING) {
                pending = true;
                continue;
            }
            if (status == RECOMP_LIVE_FAILED) {
                task->complete = true;
                task->final = live_task_hresult(hresult);
                live_task_drop_requests(task);
                return task->final;
            }
        }
        if (pending) return LIVE_TASK_RUNNING;
    }
    task->complete = true;
    task->final = live_task_finished(task);
    return task->final;
}

/* Calls whose guest API returns an HRESULT at once (friend operations) still
   have a backend request in flight; finish it on the next pump. */
static void live_detach(RecompLiveRequest request)
{
    if (request == 0u) return;
    for (unsigned i = 0u; i < LIVE_DETACHED_REQUESTS; ++i)
        if (live_detached[i] == 0u) {
            live_detached[i] = request;
            return;
        }
    /* The table is small and every guest friend operation waits for the
       previous one in the UI; a full table leaves the request to its
       backend's own timeout and release. */
}

static void live_pump_detached(void)
{
    const RecompLiveBackend *backend = live_backend();
    if (backend == NULL || backend->poll == NULL) return;
    for (unsigned i = 0u; i < LIVE_DETACHED_REQUESTS; ++i) {
        if (live_detached[i] == 0u) continue;
        uint32_t hresult = 0u;
        if (backend->poll(backend->self, live_detached[i], &hresult) == RECOMP_LIVE_PENDING) continue;
        if (backend->release != NULL) backend->release(backend->self, live_detached[i]);
        live_detached[i] = 0u;
    }
}

/* Every task API in the table returns S_OK and hands the handle to its own
   out argument; the game stores that pointer and polls it later. Returning the
   handle here (as an earlier version did) happens to pass the callers that
   only test "< 0", but it breaks XOnlineStringVerify, whose caller loops while
   the return is zero (online_verify_strings_all_languages 0x2a1860), and it
   violates the HRESULT the rest of the library returns. */
static bool live_task_start(LiveTask *task, RecompLiveRequest request, uint32_t task_out)
{
    const RecompLiveBackend *backend = live_backend();
    if (task == NULL) {
        if (backend != NULL && backend->release != NULL && request != 0u)
            backend->release(backend->self, request);
        return false;
    }
    if (!live_task_add(task, request)) {
        if (backend != NULL && backend->release != NULL && request != 0u)
            backend->release(backend->self, request);
        live_task_release(task);
        return false;
    }
    live_set_u32(task_out, task->handle);
    return true;
}

/* --- lifecycle, users, logon --- */

static void live_xnet_startup(void) { kernel_return(1u, 0u); }
static void live_wsa_startup(void) { kernel_return(2u, 0u); }
static void live_xonline_startup(void) { kernel_return(1u, 0u); }
static void live_xonline_cleanup(void)
{
    live_notification_valid = false;
    /* Logged off: every peer slot and route this account introduced is gone. */
    recomp_live_transport_forget(0u);
    kernel_return(0u, 0u);
}
static void live_xnet_cleanup(void) { kernel_return(0u, 0u); }
static void live_wsa_cleanup(void) { kernel_return(0u, 0u); }
static void live_link_status(void) { kernel_return(0u, 0x0bu); } /* up, 100 Mbit, full duplex */

static void live_get_users(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t array = kernel_arg(1), count = kernel_arg(2);
    RecompLiveUser user;
    uint32_t users = 0u;
    if (backend != NULL && backend->local_user != NULL && backend->local_user(backend->self, &user)) {
        if (array != 0u) live_write_user(array, &user);
        live_bind_profile_xuid(user.xuid);
        users = 1u;
    }
    live_set_u32(count, users);
    kernel_return(2u, 0u);
}

/* XOnlineLogon(users, service ids, count, 0, &task) returns S_OK and hands
   the task to argument 5. The user flags at +8 stay zero: the retail account
   enumerator FUN_0039c80f clears them (param_3[2] = 0) for every record, and
   the game's two flag tests (0x10000 in FUN_002a1340, 0x80000 in
   online_query_service_info 0x2a53a0 and FUN_002a12d0) then take the same
   branches as they do on a real console. */
static void live_logon(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t users = kernel_arg(1), task_out = kernel_arg(5);
    if (users == 0u || task_out == 0u || backend == NULL || backend->logon == NULL) {
        kernel_return(5u, LIVE_E_INVALIDARG);
        return;
    }
    /* The game's four-user array is on its stack (online_logon 0x29f7e0), and the
       retail library copies it: XOnlineGetLogonUsers must return a copy that lasts. */
    static uint32_t logon_copy;
    if (live_scratch(&logon_copy, LIVE_LOGON_USERS * LIVE_USER_RECORD_BYTES) == 0u) {
        kernel_return(5u, LIVE_E_FAIL);
        return;
    }
    uint8_t copy[LIVE_LOGON_USERS * LIVE_USER_RECORD_BYTES];
    live_load(copy, users, sizeof copy);
    live_store(logon_copy, copy, sizeof copy);
    live_logon_users = logon_copy;
    live_logon_slot = 0u;
    for (unsigned i = 0u; i < 4u; ++i) {
        uint64_t xuid = 0u;
        live_load(&xuid, logon_copy + i * LIVE_USER_RECORD_BYTES, sizeof xuid);
        if (xuid != 0u) {
            live_logon_slot = i;
            break;
        }
    }
    live_notification_valid = false;
    LiveTask *task = live_task_new(LIVE_KIND_LOGON);
    if (!live_task_start(task, backend->logon(backend->self), task_out)) {
        kernel_return(5u, LIVE_E_FAIL);
        return;
    }
    live_logon_task = task;
    live_online_changed(true);
    kernel_return(5u, 0u);
}

/* The logon loop (online_update 0x2a5530, cases 1 and 3) polls
   XOnlineTaskContinue once per frame: 0 means pending. The first non-zero
   value makes it call XOnlineLogonTaskGetResults, which must return the
   task's HRESULT. Only 0x1510f0 moves the logon on (it then calls
   XOnlineGetLogonUsers and online_query_service_info 0x2a53a0); every other
   non-negative value leaves the logon page waiting forever, 0x80151006 keeps
   the task open for a retry, and any other negative value is the error page.
   So the task's final value is 0x1510f0 for both Continue and GetResults,
   never the generic 0x1500f0. */
static void live_logon_results(void)
{
    LiveTask *task = live_task_find(kernel_arg(1));
    if (task == NULL || task->kind != LIVE_KIND_LOGON) {
        kernel_return(1u, LIVE_E_INVALIDARG);
        return;
    }
    kernel_return(1u, live_task_poll(task));
}

static void live_logon_users_query(void) { kernel_return(0u, live_logon_users); }

static void live_service_info(void) { kernel_return(2u, 0u); }

static void live_task_continue(void)
{
    live_pump_detached();
    LiveTask *task = live_task_find(kernel_arg(1));
    if (task == NULL) {
        kernel_return(1u, LIVE_E_INVALIDARG);
        return;
    }
    kernel_return(1u, live_task_poll(task));
}

static void live_task_close(void)
{
    LiveTask *task = live_task_find(kernel_arg(1));
    if (task != NULL) {
        /* online_logoff 0x2a4010 ends with XOnlineTaskClose(g_online_logon_task). */
        if (task == live_logon_task) {
            live_logon_task = NULL;
            live_online_changed(false);
        }
        live_task_release(task);
    }
    kernel_return(1u, 0u);
}

static void live_string_verify(void)
{
    /* Logon checks the account string; the service has nothing to reject.
       XOnlineStringVerify returns S_OK and stores the task in argument 5: the
       caller's loop (online_verify_strings_all_languages 0x2a1860) stops on
       any non-zero return, so a handle here would skip the other languages.
       The task must finish with 0x1500f0 (XONLINETASK_S_SUCCESS), not 1:
       FUN_002a1940 0x2a1940 treats 1 as "results ready" and calls
       FUN_0039718f 0x39718f, which dereferences the retail task object. */
    LiveTask *task = live_task_new(LIVE_KIND_OTHER);
    if (task == NULL) {
        kernel_return(5u, LIVE_E_FAIL);
        return;
    }
    live_task_complete_now(task);
    live_set_u32(kernel_arg(5), task->handle);
    kernel_return(5u, 0u);
}

static void live_dsound_create_stream(void)
{
    /* Voice is out of scope: the voice page finds no device to capture from. */
    live_set_u32(kernel_arg(2), 0u);
    kernel_return(2u, 0x80004005u);
}

/* XOnlineNotificationSetState(user, state, kid_lo, kid_hi, cbState, pvState).
   The last two arguments are not an out pair: FUN_002a1340 0x2a1340
   passes cbState = 2 and a pointer to the two bytes at 0x7bfdf0 (01 00 in .data),
   and the retail function FUN_0039f5e3 0x39f5e3 copies them into its kernel
   notification object (FUN_00385816 0x385816, via FUN_0039032a 0x39032a)
   without writing them back. The game later compares the blob's low byte with
   the invite's parsed match type at g_pending_invite+0x35 (0xf1c8b5) in
   session_matches_title 0x2a0f10 and online_get_accepted_game_invite
   0x2a3be0; leaving the buffer untouched is what the retail library does.
   The retail function also suppresses an unchanged state/kid through the
   kernel's change bits, so only report actual changes to the backend; the
   game calls this every frame from online_update 0x2a5530. */
static void live_notification(void)
{
    const RecompLiveBackend *backend = live_backend();
    uint8_t kid[8];
    const uint32_t kid_lo = kernel_arg(3), kid_hi = kernel_arg(4);
    const uint32_t state = kernel_arg(2);
    memcpy(kid, &kid_lo, 4u);
    memcpy(kid + 4, &kid_hi, 4u);
    if (!live_notification_valid || state != live_notification_state ||
        memcmp(kid, live_notification_kid, sizeof kid) != 0) {
        live_notification_valid = true;
        live_notification_state = state;
        memcpy(live_notification_kid, kid, sizeof kid);
        if (backend != NULL && backend->presence != NULL) backend->presence(backend->self, state, kid);
    }
    kernel_return(6u, 0u);
}

/* --- mutelist --- */

static void live_mutelist_startup(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(2);
    if (task_out == 0u || backend == NULL || backend->mutelist == NULL) {
        kernel_return(2u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_MUTELIST_STARTUP);
    if (!live_task_start(task, backend->mutelist(backend->self), task_out)) {
        kernel_return(2u, LIVE_E_FAIL);
        return;
    }
    kernel_return(2u, 0u);
}

static void live_mutelist_get(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(4);
    if (task_out == 0u || backend == NULL || backend->mutelist == NULL) {
        kernel_return(6u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_MUTELIST_GET);
    if (task == NULL) {
        kernel_return(6u, LIVE_E_FAIL);
        return;
    }
    task->out[0] = kernel_arg(5);
    task->out[1] = kernel_arg(6);
    task->capacity = kernel_arg(2);
    if (!live_task_start(task, backend->mutelist(backend->self), task_out)) {
        kernel_return(6u, LIVE_E_FAIL);
        return;
    }
    kernel_return(6u, 0u);
}

/* --- friends --- */

static void live_write_friend(uint32_t address, const RecompLiveFriend *friend)
{
    uint8_t record[LIVE_FRIEND_RECORD_BYTES];
    memset(record, 0, sizeof record);
    memcpy(record + LIVE_FRIEND_XUID, &friend->xuid, sizeof friend->xuid);
    live_copy_tag((char *)record + LIVE_FRIEND_GAMERTAG, friend->gamertag, RECOMP_LIVE_GAMERTAG_BYTES);
    /* RecompLiveFriend.flags already uses the retail XONLINE_FRIENDSTATE_FLAG_*
       values: 1 online, 2 playing, 0x10 joinable, 0x04000000 sent invite,
       0x08000000 received invite, 0x10000000/0x20000000 invite answered,
       0x40000000/0x80000000 request sent/received. friend_ui_state (0x2a0030)
       forms its UI joinable bit from (flags & 0x08000010) == 0x10, and the
       library's GetLatest (xonline_friends_get_latest, 0x39f69e) never exposes
       0x4: when it sees 0x4 it either clears every low flag or clears 0x4
       itself, so 0x4 is not this title's joinable bit. */
    const uint32_t flags = friend->flags & (RECOMP_LIVE_FRIEND_ONLINE | RECOMP_LIVE_FRIEND_PLAYING |
        RECOMP_LIVE_FRIEND_JOINABLE | RECOMP_LIVE_FRIEND_SENT_INVITE |
        RECOMP_LIVE_FRIEND_RECEIVED_INVITE | RECOMP_LIVE_FRIEND_INVITE_ACCEPTED |
        RECOMP_LIVE_FRIEND_INVITE_REJECTED | RECOMP_LIVE_FRIEND_SENT_REQUEST |
        RECOMP_LIVE_FRIEND_RECEIVED_REQUEST);
    memcpy(record + LIVE_FRIEND_FLAGS, &flags, sizeof flags);
    memcpy(record + LIVE_FRIEND_KID, friend->session_kid, sizeof friend->session_kid);
    memcpy(record + LIVE_FRIEND_TITLE, &friend->title_id, sizeof friend->title_id);
    /* An online friend carries the title's presence data. The game sends two
       bytes for itself from g_presence_state_blob (0x7bfdf0), which the data
       section initializes to 1 and nothing writes: the only address reference
       (online_presence_set_state 0x2a13b5) hands it to
       XOnlineNotificationSetState as the source, and
       xonline_notification_kernel_state copies from it. session_matches_title
       (0x2a0f10) answers 1 only when the length is nonzero and presence[0]
       equals that byte (-99 for no data, -1 - byte otherwise), and
       online_join_from_friend_record (0x2a46c0) logs Live off unless the same
       holds. */
    if ((flags & RECOMP_LIVE_FRIEND_ONLINE) != 0u) {
        record[LIVE_FRIEND_PRESENCE_LENGTH] = LIVE_FRIEND_PRESENCE_BYTES;
        record[LIVE_FRIEND_PRESENCE] = LIVE_FRIEND_SELF_PRESENCE;
    }
    live_store(address, record, sizeof record);
}

static uint64_t live_friend_xuid(uint32_t record)
{
    uint64_t xuid = 0u;
    live_load(&xuid, record + LIVE_FRIEND_XUID, sizeof xuid);
    return xuid;
}

static void live_friends_startup(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(2);
    if (task_out == 0u || backend == NULL || backend->friends_enumerate == NULL) {
        kernel_return(2u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_FRIENDS_STARTUP);
    if (!live_task_start(task, backend->friends_enumerate(backend->self), task_out)) {
        kernel_return(2u, LIVE_E_FAIL);
        return;
    }
    /* The library (xonline_friends_startup_task, 0x3a24d9) returns its
       task-create HRESULT and stores the handle through the out-pointer;
       online_friends_poll (0x29f540) keeps the return only to abort on < 0 and
       continues the handle at g_online_friends_startup_task. */
    kernel_return(2u, 0u);
}

static void live_friends_enumerate(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(3);
    if (task_out == 0u || backend == NULL || backend->friends_enumerate == NULL) {
        kernel_return(3u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_FRIENDS_ENUMERATE);
    if (!live_task_start(task, backend->friends_enumerate(backend->self), task_out)) {
        kernel_return(3u, LIVE_E_FAIL);
        return;
    }
    /* xonline_friends_enumerate_task (0x3a250e) also returns the task-create
       HRESULT and writes the handle to its third argument; online_friends_poll
       continues that handle and reads 0x1500F1 from the task, not from this
       return. */
    kernel_return(3u, 0u);
}

static void live_friends_latest(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t array = kernel_arg(3), capacity = kernel_arg(2);
    uint32_t count = 0u;
    if (array != 0u && capacity != 0u && backend != NULL && backend->friends != NULL) {
        const uint32_t available = capacity < RECOMP_LIVE_MAX_FRIENDS ? capacity : RECOMP_LIVE_MAX_FRIENDS;
        count = backend->friends(backend->self, live_friends, available);
        if (count > available) count = available;
        for (uint32_t i = 0u; i < count; ++i)
            live_write_friend(array + i * LIVE_FRIEND_RECORD_BYTES, &live_friends[i]);
    }
    kernel_return(3u, count);
}

static void live_friends_enumerate_finish(void)
{
    LiveTask *task = live_task_find(kernel_arg(1));
    if (task != NULL && task->kind == LIVE_KIND_FRIENDS_ENUMERATE) {
        task->complete = true;
        task->final = LIVE_TASK_SUCCESS;
        task->keep = false;
        /* The game polls TaskContinue once more before closing the task. */
        live_task_drop_requests(task);
    }
    kernel_return(1u, 0u);
}

static RecompLiveAnswer live_answer(uint32_t value)
{
    if (value == 1u) return RECOMP_LIVE_YES;
    if (value == 2u) return RECOMP_LIVE_NEVER;
    return RECOMP_LIVE_NO;
}

static void live_friends_request(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint64_t xuid = (uint64_t)kernel_arg(2) | ((uint64_t)kernel_arg(3) << 32);
    if (backend == NULL || backend->friend_request == NULL) {
        kernel_return(4u, LIVE_E_FAIL);
        return;
    }
    live_detach(backend->friend_request(backend->self, xuid));
    kernel_return(4u, 0u);
}

static void live_friends_answer_request(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint64_t xuid = live_friend_xuid(kernel_arg(2));
    if (backend == NULL || backend->friend_answer == NULL) {
        kernel_return(3u, LIVE_E_FAIL);
        return;
    }
    live_detach(backend->friend_answer(backend->self, xuid, live_answer(kernel_arg(3))));
    kernel_return(3u, 0u);
}

static void live_friends_remove(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint64_t xuid = live_friend_xuid(kernel_arg(2));
    if (backend == NULL || backend->friend_remove == NULL) {
        kernel_return(2u, LIVE_E_FAIL);
        return;
    }
    live_detach(backend->friend_remove(backend->self, xuid));
    kernel_return(2u, 0u);
}

static uint32_t live_friend_xuids(uint32_t records, uint32_t count, uint64_t *out)
{
    uint32_t taken = 0u;
    for (; taken < count && taken < RECOMP_LIVE_MAX_FRIENDS; ++taken)
        out[taken] = live_friend_xuid(records + taken * LIVE_FRIEND_RECORD_BYTES);
    return taken;
}

static void live_friends_game_invite(void)
{
    const RecompLiveBackend *backend = live_backend();
    uint8_t kid[8];
    const uint32_t kid_lo = kernel_arg(2), kid_hi = kernel_arg(3);
    memcpy(kid, &kid_lo, 4u);
    memcpy(kid + 4, &kid_hi, 4u);
    uint64_t xuids[RECOMP_LIVE_MAX_FRIENDS];
    const uint32_t count = live_friend_xuids(kernel_arg(5), kernel_arg(4), xuids);
    if (backend == NULL || backend->game_invite == NULL) {
        kernel_return(5u, LIVE_E_FAIL);
        return;
    }
    live_detach(backend->game_invite(backend->self, kid, xuids, count));
    kernel_return(5u, 0u);
}

static void live_friends_revoke_game_invite(void)
{
    const RecompLiveBackend *backend = live_backend();
    uint8_t kid[8];
    const uint32_t kid_lo = kernel_arg(2), kid_hi = kernel_arg(3);
    memcpy(kid, &kid_lo, 4u);
    memcpy(kid + 4, &kid_hi, 4u);
    uint64_t xuids[RECOMP_LIVE_MAX_FRIENDS];
    const uint32_t count = live_friend_xuids(kernel_arg(5), kernel_arg(4), xuids);
    if (backend == NULL || backend->revoke_invite == NULL) {
        kernel_return(5u, LIVE_E_FAIL);
        return;
    }
    live_detach(backend->revoke_invite(backend->self, kid, xuids, count));
    kernel_return(5u, 0u);
}

static void live_friends_answer_game_invite(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint64_t xuid = live_friend_xuid(kernel_arg(2));
    if (backend == NULL || backend->answer_invite == NULL) {
        kernel_return(3u, LIVE_E_FAIL);
        return;
    }
    live_detach(backend->answer_invite(backend->self, xuid, live_answer(kernel_arg(3))));
    kernel_return(3u, 0u);
}

static void live_friends_join_game(void)
{
    /* (user index, friend record). The game calls this only for a friend in
       another title (online_friend_join_game 0x2a3390); for the same title it
       goes straight to the pending invite. Either way it then copies the
       record into g_pending_invite, and page 3 reaches
       online_join_from_friend_record (0x2a46c0), which calls
       XOnlineMatchSessionFindFromID with the record's session id
       (+0x28) and joins the result through online_session_join. The session
       lookup and the transport's connect carry the join, so no backend
       request belongs here; a cross-title launch is out of scope. */
    kernel_return(2u, 0u);
}

static void live_friends_accepted_invite(void)
{
    /* (friend record). 0 means an invite was accepted from outside the game;
       the game then ages it against its own timestamp and joins it through
       the pending-invite path, XOnlineMatchSessionFindFromID on the record's
       session id. */
    const RecompLiveBackend *backend = live_backend();
    RecompLiveFriend from;
    if (backend != NULL && backend->accepted_invite != NULL && backend->accepted_invite(backend->self, &from)) {
        live_write_friend(kernel_arg(1), &from);
        kernel_return(1u, 0u);
        return;
    }
    kernel_return(1u, 1u); /* S_FALSE: no invite was accepted outside the game */
}

static void live_friends_title_name(void)
{
    /* (title id, language, count, buffer). The friend detail page's join
       spinner (0x2a9d80 case 3) formats this name into the localized
       "joining" text through friend_title_name (0x2a0140), which also rewrites
       it as L"DEAD OR ALIVE %d ULTIMATE" with the friend's presence byte + 1
       for a same-title friend in a session. The certificate gives this title
       id but has an empty name field, so this is the game's own name for it;
       a title the service knows nothing about answers with an empty name. */
    const uint32_t title = kernel_arg(1), count = kernel_arg(3), buffer = kernel_arg(4);
    if (buffer != 0u && count != 0u) {
        uint32_t units = 0u;
        if (title == LIVE_TITLE_ID)
            for (; live_title_name[units] != '\0' && units + 1u < count; ++units) {
                const uint16_t unit = (uint16_t)(unsigned char)live_title_name[units];
                live_store(buffer + units * 2u, &unit, sizeof unit);
            }
        const uint16_t end = 0u;
        live_store(buffer + units * 2u, &end, sizeof end);
    }
    kernel_return(4u, 0u);
}

/* XOnlineTitleIdIsSameTitle (0x393c9d -> 0x39e4f4) answers
   title == *(this + 4), and XOnlineStartup fills that dword from the XBE
   certificate's title id at certificate + 8: 0x54430006 in DOA2.xbe. */
static void live_title_id_is_same(void)
{
    kernel_return(1u, kernel_arg(1) == LIVE_TITLE_ID ? 1u : 0u);
}

/* --- match sessions and search --- */

static void live_session_slots(uint32_t first, uint32_t slots[RECOMP_LIVE_SESSION_SLOTS])
{
    for (unsigned i = 0u; i < RECOMP_LIVE_SESSION_SLOTS; ++i) slots[i] = kernel_arg(first + i);
}

/* The game's create passes all 27 attributes; an update passes the same array
   with only the changed entries marked (record +4). Keep the packed image
   between calls so an update can send the whole blob the matchmaker stores: a
   re-create on a roster change uses create, so the image always belongs to the
   session this process hosts. */
static uint32_t live_pack_attributes(uint32_t array, bool all)
{
    if (array == 0u) return live_attributes_size;
    uint32_t offset = 0u;
    for (unsigned i = 0u; i < LIVE_ATTRIBUTE_COUNT; ++i) {
        const uint32_t entry = array + i * LIVE_ATTRIBUTE_ENTRY_BYTES;
        const uint32_t id = live_u32(entry);
        const uint32_t dirty = live_u32(entry + 4u);
        const uint32_t size = live_attribute_bytes[i];
        if (all || dirty != 0u) {
            uint8_t *out = live_attributes + offset;
            memset(out, 0, size);
            const uint32_t value = live_u32(entry + 8u);
            if ((id & 0x00f00000u) == 0x00100000u) {
                for (uint32_t c = 0u; value != 0u && c + 2u < size; c += 2u) {
                    memcpy(out + c, recomp_memory(value + c, 2u), 2u);
                    if (out[c] == 0u && out[c + 1u] == 0u) break;
                }
            } else if ((id & 0x00f00000u) == 0x00200000u) {
                const uint32_t length = live_u32(entry + 12u);
                const uint32_t taken = length < size ? length : size;
                if (value != 0u && taken != 0u) memcpy(out, recomp_memory(value, taken), taken);
            } else {
                memcpy(out, recomp_memory(entry + 8u, 8u), 8u);
            }
        }
        offset += size;
    }
    live_attributes_size = offset;
    return offset;
}

static void live_session_create(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(8), array = kernel_arg(6);
    if (task_out == 0u || backend == NULL || backend->session_create == NULL) {
        kernel_return(8u, LIVE_E_INVALIDARG);
        return;
    }
    uint32_t slots[RECOMP_LIVE_SESSION_SLOTS];
    live_session_slots(1u, slots);
    memset(live_attributes, 0, sizeof live_attributes);
    const uint32_t size = live_pack_attributes(array, true);
    LiveTask *task = live_task_new(LIVE_KIND_SESSION_CREATE);
    if (!live_task_start(task, backend->session_create(backend->self, slots, live_attributes, size), task_out)) {
        kernel_return(8u, LIVE_E_FAIL);
        return;
    }
    kernel_return(8u, 0u);
}

static void live_session_update(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(10), array = kernel_arg(8);
    if (task_out == 0u || backend == NULL || backend->session_update == NULL) {
        kernel_return(10u, LIVE_E_INVALIDARG);
        return;
    }
    uint8_t kid[8];
    const uint32_t kid_lo = kernel_arg(1), kid_hi = kernel_arg(2);
    memcpy(kid, &kid_lo, 4u);
    memcpy(kid + 4, &kid_hi, 4u);
    uint32_t slots[RECOMP_LIVE_SESSION_SLOTS];
    live_session_slots(3u, slots);
    const uint32_t size = live_pack_attributes(array, false);
    LiveTask *task = live_task_new(LIVE_KIND_SESSION_UPDATE);
    if (!live_task_start(task, backend->session_update(backend->self, kid, slots, live_attributes, size), task_out)) {
        kernel_return(10u, LIVE_E_FAIL);
        return;
    }
    kernel_return(10u, 0u);
}

static void live_session_info(void)
{
    const RecompLiveBackend *backend = live_backend();
    LiveTask *task = live_task_find(kernel_arg(1));
    if (task == NULL || task->request_count == 0u || backend == NULL || backend->session_id == NULL) {
        kernel_return(3u, LIVE_E_INVALIDARG);
        return;
    }
    RecompLiveSessionId id;
    if (!backend->session_id(backend->self, task->requests[0], &id)) {
        live_task_poll(task);
        if (!backend->session_id(backend->self, task->requests[0], &id)) {
            kernel_return(3u, (int32_t)task->final < 0 ? task->final : LIVE_E_FAIL);
            return;
        }
    }
    live_store(kernel_arg(2), id.kid, sizeof id.kid);
    live_store(kernel_arg(3), id.key, sizeof id.key);
    kernel_return(3u, 0u);
}

static void live_session_delete(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(4);
    if (task_out == 0u || backend == NULL || backend->session_delete == NULL) {
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    uint8_t kid[8];
    const uint32_t kid_lo = kernel_arg(1), kid_hi = kernel_arg(2);
    memcpy(kid, &kid_lo, 4u);
    memcpy(kid + 4, &kid_hi, 4u);
    LiveTask *task = live_task_new(LIVE_KIND_SESSION_DELETE);
    if (!live_task_start(task, backend->session_delete(backend->self, kid), task_out)) {
        kernel_return(4u, LIVE_E_FAIL);
        return;
    }
    kernel_return(4u, 0u);
}

static void live_session_find(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(4);
    if (task_out == 0u || backend == NULL || backend->find == NULL) {
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    uint8_t kid[8];
    const uint32_t kid_lo = kernel_arg(1), kid_hi = kernel_arg(2);
    memcpy(kid, &kid_lo, 4u);
    memcpy(kid + 4, &kid_hi, 4u);
    LiveTask *task = live_task_new(LIVE_KIND_FIND);
    if (!live_task_start(task, backend->find(backend->self, kid), task_out)) {
        kernel_return(4u, LIVE_E_FAIL);
        return;
    }
    kernel_return(4u, 0u);
}

/* Quick Match and OptiMatch both enter XOnlineMatchSearch from
   online_match_search_start 0x29f940 as
   (procedure 1, max results 25, attribute count 7, filters, results length,
   0, &task). The filter block is the game's own attribute array, built on
   that function's stack, not the create/update array at 0xf24918. It is seven
   16-byte records, cleared and then filled from seven page dwords:
   [id][flags = 0][value][length = 0]. Ids the game emits:
     0x00000000  integer equality filter; the value is at +8
     0x00F00000  no filter (wildcard); the retail packer writes only this id
     0x00100000  wide string; not emitted by a search
     0x00200000  blob; not emitted by a search
   The slots are positional, in the order the page edits them. Quick Match
   passes NULL and uses the defaults [3, 0, 0, 0x13, 0, 0, 0], which become
   record 0 value 3, records 1/2/6 wildcard, record 3 value 0x13, record 4
   value 0 (a literal zero, not a wildcard) and record 5 wildcard. OptiMatch
   passes page+0xe0, a copy of its edit fields at page+0x100, through the same
   builder. Only slots 0/1/2/5/6 turn a zero into the 0xF00000 wildcard.
   The game filters the returned records itself in
   online_search_filter_results 0x2a4740 using the parsed attributes and the
   gate at record+0x50, so the matchmaker may return every open session; the
   raw 112 bytes only need to be forwarded for a service that wants them. */
static void live_search(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(7);
    if (task_out == 0u || backend == NULL || backend->search == NULL) {
        kernel_return(7u, LIVE_E_INVALIDARG);
        return;
    }
    const uint32_t attributes = kernel_arg(4), attribute_count = kernel_arg(3);
    const uint32_t filters_size = attributes == 0u ? 0u
        : attribute_count < 0x40u ? attribute_count * LIVE_ATTRIBUTE_ENTRY_BYTES : 0x400u;
    const uint8_t *filters = filters_size == 0u ? NULL : recomp_memory(attributes, filters_size);
    LiveTask *task = live_task_new(LIVE_KIND_SEARCH);
    if (!live_task_start(task, backend->search(backend->self, kernel_arg(1), filters, filters_size), task_out)) {
        kernel_return(7u, LIVE_E_FAIL);
        return;
    }
    kernel_return(7u, 0u);
}

/* The backend's four slot counts are XOnlineMatchSessionCreate's own order
   (public filled, public open, private filled, private open), which is what the
   matchmaker protocol carries. The search record the game reads keeps the Live
   service's order instead: public open, private open, public filled, private
   filled. CXo_XOnlineMatchSessionCreate packs its request that way
   (xonline_match_session_put: argument 2 at +0x34, 4 at +0x38, 1 at +0x3c, 3
   at +0x40), and online_session_join adds slot 0 to slot 2 for the public total
   and slot 1 to slot 3 for the private one. Writing the create order here would
   put the open counts where the game reads the filled ones. */
static void live_record_slots(uint8_t header[LIVE_SEARCH_HEADER_BYTES], const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS])
{
    memcpy(header + LIVE_SEARCH_SLOTS, &slots[1], 4u);      /* public open */
    memcpy(header + LIVE_SEARCH_SLOTS + 4u, &slots[3], 4u); /* private open */
    memcpy(header + LIVE_SEARCH_SLOTS + 8u, &slots[0], 4u); /* public filled */
    memcpy(header + LIVE_SEARCH_SLOTS + 12u, &slots[2], 4u); /* private filled */
}

static void live_write_search_record(uint32_t record, const RecompLiveSession *session)
{
    uint8_t header[LIVE_SEARCH_HEADER_BYTES];
    memset(header, 0, sizeof header);
    memcpy(header + LIVE_SEARCH_KID, session->id.kid, sizeof session->id.kid);
    live_fill_xnaddr(header + LIVE_SEARCH_XNADDR, &session->host.address);
    memcpy(header + LIVE_SEARCH_KEY, session->id.key, sizeof session->id.key);
    live_record_slots(header, session->slots);
    /* The gate at +0x50 stays 0: the game filters the records itself. */
    live_store(record, header, sizeof header);
    uint8_t attributes[LIVE_ATTRIBUTE_BYTES];
    memset(attributes, 0, sizeof attributes);
    const uint32_t size = session->attributes_size < LIVE_ATTRIBUTE_BYTES
        ? session->attributes_size
        : LIVE_ATTRIBUTE_BYTES;
    if (size != 0u) memcpy(attributes, session->attributes, size);
    live_terminate_attributes(attributes, sizeof attributes);
    live_store(record + LIVE_SEARCH_ATTRIBUTES, attributes, sizeof attributes);
}

/* XOnlineMatchSearchGetResults(task, &result_array, &count). The second
   argument is the pointer-array out, not the count: online_update 0x2a5530
   pushes &count (0xf24910) first, then a local pointer out, then the task
   (0x2a5a23..0x2a5a34), and CXo_XOnlineMatchSearchGetResults 0x39cf35 writes
   *param_3 = the task's pointer array and *param_4 = the row count. The game
   then indexes that array, copies each 0x54-byte header into its own 25 x 0x2f4
   records at 0xf1d2f0 and calls XOnlineMatchSearchParse 0x393c87. Writing the
   count to the second argument and the pointers to the third corrupts the
   caller's stack and the search count. */
static void live_search_results(void)
{
    const RecompLiveBackend *backend = live_backend();
    LiveTask *task = live_task_find(kernel_arg(1));
    const uint32_t array_out = kernel_arg(2), count_out = kernel_arg(3);
    if (task == NULL || (task->kind != LIVE_KIND_SEARCH && task->kind != LIVE_KIND_FIND) ||
        array_out == 0u || count_out == 0u || backend == NULL || backend->results == NULL) {
        kernel_return(3u, LIVE_E_INVALIDARG);
        return;
    }
    if (!task->complete) live_task_poll(task);
    uint32_t count = 0u;
    uint32_t array = 0u;
    if (task->complete && (int32_t)task->final >= 0 && task->request_count != 0u) {
        count = backend->results(backend->self, task->requests[0], live_sessions, RECOMP_LIVE_MAX_RESULTS);
        if (count > RECOMP_LIVE_MAX_RESULTS) count = RECOMP_LIVE_MAX_RESULTS;
        const uint32_t scratch = live_scratch(&live_search_scratch,
            RECOMP_LIVE_MAX_RESULTS * LIVE_SEARCH_RECORD_BYTES);
        array = live_scratch(&live_search_pointers, RECOMP_LIVE_MAX_RESULTS * 4u);
        if (scratch == 0u || array == 0u) {
            count = 0u;
            array = 0u;
        }
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t record = scratch + i * LIVE_SEARCH_RECORD_BYTES;
            live_write_search_record(record, &live_sessions[i]);
            live_set_u32(array + i * 4u, record);
        }
    }
    live_set_u32(array_out, array);
    live_set_u32(count_out, count);
    kernel_return(3u, 0u);
}

/* XOnlineMatchSearchResultsLen(max results, attribute count, specs) is the
   retail library's response-buffer size, not the game's record array. CXo_
   XOnlineMatchSearchResultsLen 0x39d0b0 starts at 0x54, adds 0x10 for an
   integer spec, 10 + 2 * length for a wide string (id 0x100000), 10 + length
   for a blob (id 0x200000) and 8 otherwise, then multiplies by the result
   count and adds 0x5c. For the game's 27 specs (0x7bfd00) a record is 0x624
   bytes (0x54 header + 0x5d0 of attributes), so 25 results are
   0x624 * 25 + 0x5c = 0x99e0. xonline_match_session_alloc 0x39d479
   allocates exactly dwResultsLen bytes at task+0x40 (minimum 0x118) plus
   maxResults * 4 at task+0x3c; the game allocates nothing itself. The old
   0x4000 is smaller than 25 records of 0x2f4, so a library-side allocation
   would overflow. */
static void live_search_results_len(void)
{
    const uint32_t count = kernel_arg(1), attributes = kernel_arg(2), specs = kernel_arg(3);
    uint64_t record = 0x54u;
    const uint32_t clamped = attributes < 0x100u ? attributes : 0x100u;
    for (uint32_t i = 0u; i < clamped; ++i) {
        const uint32_t id = live_u32(specs + i * 8u);
        const uint32_t size = live_u32(specs + i * 8u + 4u);
        if (id == 0u) record += 0x10u;
        else if (id == 0x00100000u) record += 10u + (uint64_t)size * 2u;
        else if (id == 0x00200000u) record += 10u + size;
        else record += 8u;
    }
    const uint64_t total = record * count + 0x5cu;
    kernel_return(3u, total > 0xffffffffu ? 0xffffffffu : (uint32_t)total);
}

static void live_search_parse(void)
{
    const uint32_t result = kernel_arg(1), out = kernel_arg(4);
    if (result == 0u || out == 0u) {
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    uint8_t attributes[LIVE_ATTRIBUTE_BYTES];
    live_load(attributes, result + LIVE_SEARCH_ATTRIBUTES, sizeof attributes);
    live_terminate_attributes(attributes, sizeof attributes);
    live_store(out, attributes, sizeof attributes);
    kernel_return(4u, 0u);
}

/* FUN_00395bb4 0x395bb4 is the retail notification-context constructor that
   FUN_002a1040 0x2a1040 calls before a friend request or game invite. When it
   succeeds, DAT_00f1d2a8 becomes a retail object and the game switches to the
   library-internal extended paths FUN_00397b64 0x397b64 / FUN_00397b91
   0x397b91. Those create real retail task objects at 0xf1ca64 and later read
   their property bags through FUN_00395b8b 0x395b8b / 9a / a9; the task
   handle would be polled through the replaced XOnlineTaskContinue, which
   cannot run a retail task. Refusing the context keeps the game on the
   public XOnlineFriendsGameInvite / XOnlineFriendsRequest thunks that this
   file and the backend own. Temporary seam: remove it when the extended
   notification path is implemented. */
static void live_notification_context(void) { kernel_return(8u, LIVE_E_FAIL); }

/* --- feedback, stats and rankings --- */

static void live_feedback(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(8);
    const uint64_t xuid = (uint64_t)kernel_arg(2) | ((uint64_t)kernel_arg(3) << 32);
    if (task_out == 0u || backend == NULL || backend->feedback == NULL) {
        kernel_return(8u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_FEEDBACK);
    if (!live_task_start(task, backend->feedback(backend->self, xuid, kernel_arg(5)), task_out)) {
        kernel_return(8u, LIVE_E_FAIL);
        return;
    }
    kernel_return(8u, 0u);
}

static RecompLiveStat live_read_stat(uint32_t entry)
{
    RecompLiveStat stat;
    memset(&stat, 0, sizeof stat);
    uint8_t bytes[LIVE_STAT_ENTRY_BYTES];
    live_load(bytes, entry, sizeof bytes);
    memcpy(&stat.id, bytes + LIVE_STAT_ID, sizeof stat.id);
    uint32_t type = 0u;
    memcpy(&type, bytes + LIVE_STAT_TYPE, sizeof type);
    stat.type = (uint8_t)type;
    if (stat.type == LIVE_STAT_TYPE_INT32) {
        int32_t value = 0;
        memcpy(&value, bytes + LIVE_STAT_VALUE, sizeof value);
        stat.value = value;
    } else {
        memcpy(&stat.value, bytes + LIVE_STAT_VALUE, sizeof stat.value);
    }
    return stat;
}

/* XOnlineStatWrite returns an HRESULT and hands the library task to the fourth
   argument (retail xonline_stat_write_request 0x3a4d86 returns the allocation result and stores the
   task in *param_5); the game checks the return for < 0 and polls the
   out-pointer. */
static void live_stat_write(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t count = kernel_arg(1), records = kernel_arg(2), task_out = kernel_arg(4);
    if (count == 0u || records == 0u || task_out == 0u || backend == NULL || backend->stat_write == NULL) {
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_STAT_WRITE);
    if (task == NULL) {
        kernel_return(4u, LIVE_E_FAIL);
        return;
    }
    for (uint32_t i = 0u; i < count && task->request_count < LIVE_TASK_REQUESTS; ++i) {
        const uint32_t record = records + i * LIVE_STAT_RECORD_BYTES;
        uint64_t xuid = 0u;
        live_load(&xuid, record + LIVE_STAT_XUID, sizeof xuid);
        const uint32_t board = live_u32(record + LIVE_STAT_BOARD);
        const uint32_t stat_count = live_u32(record + LIVE_STAT_COUNT);
        const uint32_t array = live_u32(record + LIVE_STAT_ARRAY);
        RecompLiveStat stats[RECOMP_LIVE_MAX_STAT_ATTRS];
        uint32_t wanted = stat_count;
        if (wanted > RECOMP_LIVE_MAX_STAT_ATTRS) {
            /* No RANKINGS group is this large (group 7 writes 61); a larger
               record would mean a changed group table, so say it instead of
               dropping the tail silently. */
            fprintf(stderr, "recomp live: stat write has %u stats, keeping %u\n", wanted,
                (unsigned)RECOMP_LIVE_MAX_STAT_ATTRS);
            wanted = RECOMP_LIVE_MAX_STAT_ATTRS;
        }
        uint32_t n = 0u;
        if (array != 0u)
            for (uint32_t k = 0u; k < wanted; ++k) {
                const RecompLiveStat stat = live_read_stat(array + k * LIVE_STAT_ENTRY_BYTES);
                /* -3 (gamertag) carries a guest wide-string pointer, not a
                   number the backend can keep. -1 (rank) is never sent: the
                   write record holds the group spec minus its last id. -2
                   (score) is a plain int64 and travels like any other stat. */
                if (stat.type == LIVE_STAT_TYPE_STRING) continue;
                stats[n++] = stat;
            }
        if (!live_task_add(task, backend->stat_write(backend->self, board, xuid, stats, n))) {
            live_task_release(task);
            kernel_return(4u, LIVE_E_FAIL);
            return;
        }
    }
    if (task->request_count == 0u) {
        live_task_release(task);
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    live_set_u32(task_out, task->handle);
    kernel_return(4u, 0u);
}

/* XOnlineStatRead: same HRESULT contract (online_start_stat_read 0x2a36d0
   succeeds only on a zero return) and the same out-pointer task. */
static void live_stat_read(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t count = kernel_arg(1), records = kernel_arg(2), task_out = kernel_arg(4);
    if (count == 0u || records == 0u || task_out == 0u || backend == NULL || backend->stat_read == NULL) {
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    LiveTask *task = live_task_new(LIVE_KIND_STAT_READ);
    if (task == NULL) {
        kernel_return(4u, LIVE_E_FAIL);
        return;
    }
    /* The library puts every record of the call in one request, each with its
       own board; the backend takes the xuids of one board per call. The
       RANKINGS friend page reads up to 100 records (ranking_read_query_start 0x29d9d0 with the
       friend count + 1), so batch them instead of one request per record. */
    uint64_t batch[RECOMP_LIVE_MAX_FRIENDS];
    uint32_t batch_count = 0u, batch_board = 0u;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t record = records + i * LIVE_STAT_RECORD_BYTES;
        uint64_t xuid = 0u;
        live_load(&xuid, record + LIVE_STAT_XUID, sizeof xuid);
        const uint32_t board = live_u32(record + LIVE_STAT_BOARD);
        if (batch_count != 0u && (board != batch_board || batch_count >= RECOMP_LIVE_MAX_FRIENDS)) {
            if (!live_task_add(task, backend->stat_read(backend->self, batch_board, batch, batch_count))) {
                live_task_release(task);
                kernel_return(4u, LIVE_E_FAIL);
                return;
            }
            batch_count = 0u;
        }
        if (batch_count == 0u) batch_board = board;
        batch[batch_count++] = xuid;
    }
    if (batch_count != 0u &&
        !live_task_add(task, backend->stat_read(backend->self, batch_board, batch, batch_count))) {
        live_task_release(task);
        kernel_return(4u, LIVE_E_FAIL);
        return;
    }
    if (task->request_count == 0u) {
        live_task_release(task);
        kernel_return(4u, LIVE_E_INVALIDARG);
        return;
    }
    live_set_u32(task_out, task->handle);
    kernel_return(4u, 0u);
}

static void live_wide_tag(const char *text, uint16_t *out, unsigned units)
{
    unsigned length = 0u;
    for (; text[length] != '\0' && length + 1u < units; ++length) {
        const unsigned char c = (unsigned char)text[length];
        out[length] = c < 0x80u ? c : '?';
    }
    out[length] = 0u;
}

/* The caller's text buffer, where the library puts the wide gamertags the -3
   entries point at. It holds XONLINE_GAMERTAG_SIZE units per row. */
typedef struct LiveStatText {
    uint32_t buffer;
    uint32_t size;
    uint32_t offset;
    uint32_t row;     /* this row's gamertag, 0 until written */
    bool row_written;
} LiveStatText;

static void live_stat_text_begin(LiveStatText *text, uint32_t buffer, uint32_t size)
{
    text->buffer = buffer;
    text->size = size;
    text->offset = 0u;
    text->row = 0u;
    text->row_written = false;
}

static void live_stat_text_row(LiveStatText *text, const char *tag)
{
    if (text->row_written) return;
    text->row_written = true;
    text->row = 0u;
    const uint32_t bytes = LIVE_LEADER_GAMERTAG_UNITS * 2u;
    if (text->buffer == 0u || text->offset > text->size || text->size - text->offset < bytes) return;
    /* Zeroed: all of it goes to guest memory, past the tag's end too. */
    uint16_t wide[LIVE_LEADER_GAMERTAG_UNITS] = {0};
    live_wide_tag(tag, wide, LIVE_LEADER_GAMERTAG_UNITS);
    live_store(text->buffer + text->offset, wide, sizeof wide);
    text->row = text->buffer + text->offset;
    text->offset += bytes;
}

/* Writes one requested stat entry the way the retail parser does: the type as
   a dword at +4 (the caller pre-fills +8..+0xf with zero), an int32 in the low
   dword at +8, an int64/double at +8, or a wide-string pointer at +8. */
static void live_store_stat_value(uint32_t entry, uint8_t type, int64_t value,
    uint32_t string_pointer)
{
    uint8_t bytes[LIVE_STAT_ENTRY_BYTES];
    uint32_t type_word = type;
    live_load(bytes, entry, sizeof bytes);
    memcpy(bytes + LIVE_STAT_TYPE, &type_word, sizeof type_word);
    memset(bytes + LIVE_STAT_VALUE, 0, 8u);
    switch (type) {
    case LIVE_STAT_TYPE_INT32: {
        const int32_t narrow = (int32_t)value;
        memcpy(bytes + LIVE_STAT_VALUE, &narrow, sizeof narrow);
        break;
    }
    case LIVE_STAT_TYPE_STRING:
        memcpy(bytes + LIVE_STAT_VALUE, &string_pointer, sizeof string_pointer);
        break;
    default:
        memcpy(bytes + LIVE_STAT_VALUE, &value, sizeof value);
        break;
    }
    live_store(entry, bytes, sizeof bytes);
}

/* Fills one entry the record asked for: a stored stat by id, or one of the
   pseudo ids the library derives from the row and board. */
static void live_fill_stat_entry(uint32_t entry, uint16_t id, const RecompLiveLeaderRow *row,
    uint32_t board, LiveStatText *text)
{
    /* The library writes the id it was asked for (the service echoes it). */
    live_store(entry + LIVE_STAT_ID, &id, sizeof id);
    switch (id) {
    case LIVE_STAT_PSEUDO_RANK:
        live_store_stat_value(entry, LIVE_STAT_TYPE_INT32, (int64_t)(int32_t)row->rank, 0u);
        return;
    case LIVE_STAT_PSEUDO_SCORE: {
        int64_t score = 0;
        for (uint32_t i = 0u; i < row->stat_count; ++i)
            if (row->stats[i].id == id) {
                score = row->stats[i].value;
                break;
            }
        live_store_stat_value(entry, LIVE_STAT_TYPE_INT64, score, 0u);
        return;
    }
    case LIVE_STAT_PSEUDO_GAMERTAG:
        live_stat_text_row(text, row->gamertag);
        live_store_stat_value(entry, LIVE_STAT_TYPE_STRING, 0, text->row);
        return;
    case LIVE_STAT_PSEUDO_BOARD:
        live_store_stat_value(entry, LIVE_STAT_TYPE_INT32, (int64_t)(int32_t)board, 0u);
        return;
    default:
        break;
    }
    for (uint32_t i = 0u; i < row->stat_count; ++i) {
        if (row->stats[i].id != id) continue;
        const uint8_t type = row->stats[i].type;
        /* Only the library's three numeric types reach a guest entry: a string
           row would otherwise become a NULL (or foreign) pointer. */
        if (type < LIVE_STAT_TYPE_INT32 || type > LIVE_STAT_TYPE_DOUBLE)
            live_store_stat_value(entry, 0u, 0, 0u);
        else
            live_store_stat_value(entry, type, row->stats[i].value, 0u);
        return;
    }
    /* A stat the row has no value for: type 0, as the retail service sent it, and
       the game reads that as no data (ranking_row_to_profile_counters 0x29b590
       takes a type-0 first entry for a player with no record). The record was
       pre-filled with the group's type by FUN_00294510 0x294510 before the read,
       so leaving the entry alone would make an absent stat a real zero. */
    live_store_stat_value(entry, 0u, 0, 0u);
}

/* Fills the entries a stat-read record requested (the record's own id list,
   filled by the game from the same RANKINGS group spec). */
static void live_fill_stat_result(uint32_t record, const RecompLiveLeaderRow *row,
    LiveStatText *text)
{
    const uint32_t array = live_u32(record + LIVE_STAT_ARRAY);
    const uint32_t count = live_u32(record + LIVE_STAT_COUNT);
    const uint32_t board = live_u32(record + LIVE_STAT_BOARD);
    if (array == 0u) return;
    text->row = 0u;
    text->row_written = false;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t entry = array + i * LIVE_STAT_ENTRY_BYTES;
        live_fill_stat_entry(entry, live_u16(entry + LIVE_STAT_ID), row, board, text);
    }
}

static void live_stat_read_results(void)
{
    const RecompLiveBackend *backend = live_backend();
    LiveTask *task = live_task_find(kernel_arg(1));
    /* (task, count, records, text size, text buffer): the game passes the same
       count and array it gave XOnlineStatRead, and the text buffer it
       allocated for the wide gamertags (query+0xc, count * 32 bytes). */
    const uint32_t count = kernel_arg(2), records = kernel_arg(3);
    const uint32_t text_size = kernel_arg(4), text_buffer = kernel_arg(5);
    if (task == NULL || task->kind != LIVE_KIND_STAT_READ || backend == NULL || backend->stat_results == NULL) {
        kernel_return(5u, LIVE_E_INVALIDARG);
        return;
    }
    if (!task->complete) live_task_poll(task);
    if (!task->complete) {
        kernel_return(5u, LIVE_TASK_RUNNING);
        return;
    }
    if ((int32_t)task->final < 0) {
        kernel_return(5u, task->final);
        return;
    }
    LiveStatText text;
    live_stat_text_begin(&text, text_buffer, text_size);
    for (unsigned r = 0u; r < task->request_count; ++r) {
        uint32_t rows = backend->stat_results(backend->self, task->requests[r], live_rows,
            RECOMP_LIVE_MAX_LEADER_ROWS);
        if (rows > RECOMP_LIVE_MAX_LEADER_ROWS) rows = RECOMP_LIVE_MAX_LEADER_ROWS;
        if (rows == 0u) continue;
        for (uint32_t i = 0u; i < count && records != 0u; ++i) {
            const uint32_t record = records + i * LIVE_STAT_RECORD_BYTES;
            uint64_t xuid = 0u;
            live_load(&xuid, record + LIVE_STAT_XUID, sizeof xuid);
            for (uint32_t k = 0u; k < rows; ++k) {
                if (live_rows[k].xuid != xuid) continue;
                live_fill_stat_result(record, &live_rows[k], &text);
                break;
            }
        }
    }
    kernel_return(5u, 0u);
}

/* XOnlineStatLeaderEnumerate (retail xonline_stat_leader_enumerate_request
   0x3a50ba, RET 32 = 8 args):
   (pivot XUID pointer or NULL, rank pivot, row capacity, board, stat count,
   stat ids, allocator selector, task). online_start_stat_leaderboard_enumerate
   0x2a3620 pushes the last two first (0 and &DAT_00f21cec) and fills the first
   six around the ranking_group_stat_ids 0x294500 lookup, so the task really is
   argument 8 and the selector is a constant 0. The return is the HRESULT; the
   game only starts on zero. */
static void live_stat_leader_enumerate(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t task_out = kernel_arg(8), pivot_pointer = kernel_arg(1);
    if (task_out == 0u || backend == NULL || backend->leader_enumerate == NULL) {
        kernel_return(8u, LIVE_E_INVALIDARG);
        return;
    }
    uint64_t pivot = 0u;
    if (pivot_pointer != 0u) live_load(&pivot, pivot_pointer, sizeof pivot);
    const uint32_t first_rank = kernel_arg(2);
    LiveTask *task = live_task_new(LIVE_KIND_LEADER);
    if (task == NULL) {
        kernel_return(8u, LIVE_E_FAIL);
        return;
    }
    task->board = kernel_arg(4);
    task->capacity = kernel_arg(3);
    task->stat_count = kernel_arg(5);
    task->stat_ids = kernel_arg(6);
    if (!live_task_start(task,
            backend->leader_enumerate(backend->self, task->board, pivot, first_rank, task->capacity), task_out)) {
        kernel_return(8u, LIVE_E_FAIL);
        return;
    }
    kernel_return(8u, 0u);
}

/* XOnlineStatLeaderEnumerateGetResults (retail xonline_stat_leader_results_parse
   0x3a46aa, RET 36 = 9
   args): (task, row capacity, rows, stat entry capacity, stat entries, board
   out, row count out, text size, text buffer). A row is 0x2c bytes; its stat
   entries are row-major with the enumerate's stat count as the stride.

   The guest sizes both buffers from its own group spec: the entries from
   g_ranking_stat_count 0xf1c944 (62 for group 7) times the row capacity, and
   the text buffer as row capacity * 32 bytes (one wide gamertag per row).
   This writes at most stat_count entries per row, never past stat_capacity,
   and one 16-unit tag per row, never past text_size, so a 62-entry group
   stays inside what the game allocated. */
static void live_write_leader_rows(uint32_t row_array, uint32_t row_capacity, uint32_t stat_array,
    uint32_t stat_capacity, uint32_t stat_count, uint32_t stat_ids, uint32_t board,
    LiveStatText *text, const RecompLiveLeaderRow *rows, uint32_t count)
{
    for (uint32_t i = 0u; i < count && i < row_capacity; ++i) {
        uint8_t row[LIVE_LEADER_ROW_BYTES];
        memset(row, 0, sizeof row);
        memcpy(row + LIVE_LEADER_ROW_XUID, &rows[i].xuid, sizeof rows[i].xuid);
        /* +8 is the struct's dwRank slot; the retail parser clears it and the
           game reads the rank from the pseudo -1 entry instead. */
        /* Zeroed: all of it goes to guest memory, past the tag's end too. */
        uint16_t tag[LIVE_LEADER_GAMERTAG_UNITS] = {0};
        live_wide_tag(rows[i].gamertag, tag, LIVE_LEADER_GAMERTAG_UNITS);
        memcpy(row + LIVE_LEADER_ROW_GAMERTAG, tag, sizeof tag);
        live_store(row_array + i * LIVE_LEADER_ROW_BYTES, row, sizeof row);
        text->row = 0u;
        text->row_written = false;
        if (stat_array == 0u || stat_ids == 0u) continue;
        for (uint32_t j = 0u; j < stat_count; ++j) {
            const uint32_t index = i * stat_count + j;
            if (index >= stat_capacity) break;
            const uint16_t id = live_u16(stat_ids + j * 2u);
            live_fill_stat_entry(stat_array + index * LIVE_STAT_ENTRY_BYTES, id, &rows[i], board, text);
        }
    }
}

static void live_stat_leader_results(void)
{
    const RecompLiveBackend *backend = live_backend();
    LiveTask *task = live_task_find(kernel_arg(1));
    if (task == NULL || task->kind != LIVE_KIND_LEADER || backend == NULL || backend->stat_results == NULL) {
        kernel_return(9u, LIVE_E_INVALIDARG);
        return;
    }
    if (!task->complete) live_task_poll(task);
    if (!task->complete) {
        kernel_return(9u, LIVE_TASK_RUNNING);
        return;
    }
    if ((int32_t)task->final < 0) {
        kernel_return(9u, task->final);
        return;
    }
    const uint32_t capacity = kernel_arg(2);
    uint32_t count = 0u;
    if (task->request_count != 0u) {
        const uint32_t available = capacity < RECOMP_LIVE_MAX_LEADER_ROWS ? capacity : RECOMP_LIVE_MAX_LEADER_ROWS;
        count = backend->stat_results(backend->self, task->requests[0], live_rows, available);
        if (count > available) count = available;
        LiveStatText text;
        live_stat_text_begin(&text, kernel_arg(9), kernel_arg(8));
        live_write_leader_rows(kernel_arg(3), capacity, kernel_arg(5), kernel_arg(4),
            task->stat_count, task->stat_ids, task->board, &text, live_rows, count);
    }
    live_set_u32(kernel_arg(6), task->board);
    live_set_u32(kernel_arg(7), count);
    kernel_return(9u, 0u);
}

/* --- keys, QoS and randomness --- */

static void live_register_key(void) { kernel_return(2u, 0u); }
static void live_unregister_key(void) { kernel_return(1u, 0u); }
static void live_qos_listen(void) { kernel_return(5u, 0u); }

/* XNQOS layout the game reads (XNetQosLookup, retail CXo 0x39d4xx):
     +0x00  number of addresses queried
     +0x04  number of probes still pending; online_update case 0xb waits for 0
     +0x08  entry 0, then one 0x18-byte entry per queried address:
       +0x00  BYTE flags: bit 0 = complete, bit 2 = error
       +0x0e  WORD median RTT in ms
   DOA2U's readers, all found in Ghidra:
   - online_update 0x2a5530 waits on handle+4 before sorting the search.
   - online_search_filter_results 0x2a4740 stores handle+8+i*0x18 in the
     search record at +0x2f0, so all 25 result slots need an entry.
   - FUN_002a2420 sorts by entry+0 bit 0 and entry+0x0e (missing = 0xffff).
   - FUN_002ab0c0 (search result bars) and FUN_002a8850 (session bars) map
     the RTT through FUN_002946e0; bit 2 means retry / no quality.
   - FUN_00294660 0x294660 and FUN_002946c0 0x2946c0 read handle+8 and
     handle+0x16 (entry 0) for the local slot's peer.
   - FUN_000d1570 0xd1570 and FUN_000d2070 0xd2070 read the peer's RTT,
     compute (rtt*3+199)/100 (FUN_002946e0 0x2946e0) and set the lockstep
     delay batches (FUN_00293a60 clamps to 2..7). With 50 ms the value is 3,
     the shipped default; a measured RTT between about 1 and 200 ms moves it
     from 2 to 7. No game code reads the per-probe sent/received counters or a
     bandwidth field, so zero is correct for them.
   The matchmaker has no QoS probe, so every queried entry is complete at
   the documented 50 ms constant. A real RTT needs a seam the backend or the
   transport does not have today: ppXnAddr (argument 2) holds the virtual
   addresses, so a future backend->qos_rtt(self, virtual_ina) could answer
   per entry without touching this layout. */
static uint32_t live_qos_handle(unsigned slot)
{
    return live_qos_pool + slot * LIVE_QOS_BYTES;
}

static int live_qos_slot(uint32_t handle)
{
    if (live_qos_pool == 0u || handle < live_qos_pool) return -1;
    const uint32_t offset = handle - live_qos_pool;
    if (offset % LIVE_QOS_BYTES != 0u) return -1;
    const uint32_t slot = offset / LIVE_QOS_BYTES;
    return slot < LIVE_QOS_HANDLES ? (int)slot : -1;
}

static void live_qos_lookup(void)
{
    const uint32_t handle_out = kernel_arg(12);
    if (handle_out == 0u) {
        kernel_return(12u, LIVE_E_INVALIDARG);
        return;
    }
    if (live_qos_pool == 0u)
        live_qos_pool = recomp_kernel_allocate_pool(LIVE_QOS_HANDLES * LIVE_QOS_BYTES);
    unsigned slot = LIVE_QOS_HANDLES;
    for (unsigned i = 0u; live_qos_pool != 0u && i < LIVE_QOS_HANDLES; ++i)
        if (!live_qos_used[i]) {
            slot = i;
            break;
        }
    if (slot == LIVE_QOS_HANDLES) {
        kernel_return(12u, LIVE_E_FAIL);
        return;
    }
    uint32_t queried = kernel_arg(1);
    if (queried > LIVE_QOS_ENTRIES) queried = LIVE_QOS_ENTRIES;
    uint8_t buffer[LIVE_QOS_BYTES];
    memset(buffer, 0, sizeof buffer);
    memcpy(buffer, &queried, sizeof queried);
    /* +4 stays zero: the service has no outstanding probe. */
    for (uint32_t i = 0u; i < queried; ++i) {
        const uint32_t entry = 8u + i * LIVE_QOS_ENTRY_BYTES;
        buffer[entry + LIVE_QOS_FLAGS] = 1u; /* complete, no error */
        const uint16_t rtt = LIVE_QOS_DEFAULT_RTT_MS;
        memcpy(buffer + entry + LIVE_QOS_MEDIAN_RTT, &rtt, sizeof rtt);
    }
    const uint32_t qos = live_qos_handle(slot);
    live_store(qos, buffer, sizeof buffer);
    live_qos_used[slot] = true;
    live_set_u32(handle_out, qos);
    kernel_return(12u, 0u);
}

/* XNetQosRelease(handle): the game holds one handle per search or session
   peer and clears its own pointer afterwards. Free the slot so the next
   lookup gets a fresh, independent buffer instead of overwriting a live
   peer's result (a single shared buffer aliases every session player). */
static void live_qos_release(void)
{
    const uint32_t qos = kernel_arg(1);
    const int slot = live_qos_slot(qos);
    if (slot >= 0) {
        uint8_t zero[LIVE_QOS_BYTES];
        memset(zero, 0, sizeof zero);
        live_store(qos, zero, sizeof zero);
        live_qos_used[slot] = false;
    }
    kernel_return(1u, 0u);
}

static void live_title_xnaddr(void)
{
    const RecompLiveBackend *backend = live_backend();
    const uint32_t out = kernel_arg(1);
    uint8_t xnaddr[0x24];
    memset(xnaddr, 0, sizeof xnaddr);
    RecompLiveUser user;
    if (backend != NULL && backend->local_user != NULL && backend->local_user(backend->self, &user))
        live_fill_xnaddr(xnaddr, &user.address);
    live_store(out, xnaddr, sizeof xnaddr);
    kernel_return(1u, 0xe6u); /* ethernet, static, gateway, dns, online */
}

/* XNetRandom: the host's secure random source. The retail game cannot make
   lockstep state depend on this (two consoles' values differ), so a frame run
   again after a rewind may call it without changing what is simulated. A
   source that fails leaves zeros rather than a fixed, predictable sequence. */
static void live_xnet_random(void)
{
    const uint32_t buffer = kernel_arg(1), size = kernel_arg(2);
    if (size != 0u) {
        uint8_t *out = recomp_memory(buffer, size);
#ifdef _WIN32
        if (BCryptGenRandom(NULL, out, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) memset(out, 0, size);
#elif defined(__APPLE__)
        arc4random_buf(out, size);
#else
        size_t got = 0u;
        while (got < size) {
            const ssize_t count = getrandom(out + got, size - got, 0);
            if (count <= 0) {
                memset(out + got, 0, size - got);
                break;
            }
            got += (size_t)count;
        }
#endif
    }
    kernel_return(2u, 0u);
}

/* --- lookup --- */

void recomp_live_pump(uint64_t now_ms)
{
    const RecompLiveBackend *backend = live_backend();
    if (backend == NULL) return;
    live_pump_detached();
    if (backend->tick != NULL) backend->tick(backend->self, now_ms);
}

/* The transport work package's live_transport.c defines the real lookup. Until
   it is linked, this keeps the guest API linkable: weak for the compilers the
   runner uses, a linker alternate name for MSVC. A strong definition (the
   transport, or the guest API test's stub) replaces either one. */
#if defined(_MSC_VER)
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_recomp_live_transport_lookup=_recomp_live_transport_unbound")
#else
#pragma comment(linker, "/alternatename:recomp_live_transport_lookup=recomp_live_transport_unbound")
#endif
RecompFunction recomp_live_transport_unbound(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}
#else
__attribute__((weak)) RecompFunction recomp_live_transport_lookup(uint32_t guest_address)
{
    (void)guest_address;
    return NULL;
}
#endif

/* The same seam for the peer table's logoff, which lives in the transport. */
#if defined(_MSC_VER)
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_recomp_live_transport_forget=_recomp_live_transport_forget_unbound")
#else
#pragma comment(linker, "/alternatename:recomp_live_transport_forget=recomp_live_transport_forget_unbound")
#endif
void recomp_live_transport_forget_unbound(uint32_t virtual_ina)
{
    (void)virtual_ina;
}
#else
__attribute__((weak)) void recomp_live_transport_forget(uint32_t virtual_ina)
{
    (void)virtual_ina;
}
#endif

RecompFunction recomp_live_lookup(uint32_t guest_address)
{
    /* Live is on only while a backend is installed; without one the game keeps
       its own library and no network, as before. */
    if (live_backend() == NULL) return NULL;
    switch (guest_address) {
    /* The guest winsock/XNet transport group: one UDP socket carries the
       matchmaker, every peer and the rollback session (live_transport.c). */
    case 0x00381019u: /* socket */
    case 0x00381054u: /* bind */
    case 0x0038103au: /* ioctlsocket */
    case 0x00381024u: /* closesocket */
    case 0x0038102fu: /* shutdown */
    case 0x003810a2u: /* recvfrom */
    case 0x003810c0u: /* sendto */
    case 0x003814d3u: /* WSAGetLastError */
    case 0x00380f9fu: /* XNetXnAddrToInAddr */
    case 0x00381109u: /* XNetInAddrToXnAddr */
        return recomp_live_transport_lookup(guest_address);

    case 0x00381331u: return live_xnet_startup;
    case 0x00381348u: return live_wsa_startup;
    case 0x003941ceu: return live_xonline_startup;
    case 0x003942cbu: return live_xonline_cleanup;
    case 0x00381436u: return live_xnet_cleanup;
    case 0x00381443u: return live_wsa_cleanup;
    case 0x002f9e3du: return live_link_status;
    case 0x0038100eu: return live_title_xnaddr;
    case 0x00380f7eu: return live_xnet_random;

    case 0x00393b96u: return live_get_users;
    case 0x00393ba1u: return live_logon;
    case 0x00393bb0u: return live_logon_results;
    case 0x00393bbbu: return live_logon_users_query;
    case 0x00393bc6u: return live_service_info;
    case 0x00393b80u: return live_task_continue;
    case 0x00393b8bu: return live_task_close;
    case 0x00397394u: return live_string_verify;

    case 0x00393da9u: return live_mutelist_get;
    case 0x00393db8u: return live_mutelist_startup;
    case 0x00393ca8u: return live_notification;
    /* Not an export: the retail notification context FUN_002a1040 starts
       before it hands friend invites/requests to the library internals. */
    case 0x00395bb4u: return live_notification_context;

    case 0x00393cd7u: return live_friends_startup;
    case 0x00393ce2u: return live_friends_enumerate;
    case 0x00393cf8u: return live_friends_latest;
    case 0x00393cedu: return live_friends_enumerate_finish;
    case 0x00393d03u: return live_friends_title_name;
    case 0x00393d0eu: return live_friends_remove;
    case 0x00393d19u: return live_friends_request;
    case 0x00393d3bu: return live_friends_game_invite;
    case 0x00393d5cu: return live_friends_revoke_game_invite;
    case 0x00393d7du: return live_friends_answer_request;
    case 0x00393d88u: return live_friends_answer_game_invite;
    case 0x00393d93u: return live_friends_join_game;
    case 0x00393d9eu: return live_friends_accepted_invite;
    case 0x00393c9du: return live_title_id_is_same;

    case 0x00393be7u: return live_session_create;
    case 0x00393bf6u: return live_session_update;
    case 0x00393c26u: return live_session_info;
    case 0x00393c31u: return live_session_delete;
    case 0x00393c4fu: return live_session_find;
    case 0x00393c6du: return live_search;
    case 0x00393c7cu: return live_search_results;
    case 0x00393c87u: return live_search_parse;
    case 0x00393c92u: return live_search_results_len;

    case 0x00393e07u: return live_feedback;
    case 0x00393e37u: return live_stat_write;
    case 0x00393e42u: return live_stat_read;
    case 0x00393e4du: return live_stat_read_results;
    case 0x00393e5cu: return live_stat_leader_enumerate;
    case 0x00393e6bu: return live_stat_leader_results;

    case 0x00380f89u: return live_register_key;
    case 0x00380f94u: return live_unregister_key;
    case 0x00380fd8u: return live_qos_listen;
    case 0x00380fe7u: return live_qos_lookup;
    case 0x00381003u: return live_qos_release;

    case 0x00366257u: return live_dsound_create_stream; /* voice: no streams */
    default: return NULL;
    }
}
