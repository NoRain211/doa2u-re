/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* In-memory Xbox Live: accounts, sessions, matchmaking, friends, mutes and
   rankings, all on the host's heap. Tests install it to drive the guest XOnline
   API without a matchmaker; two runners in one process can also share it later.
   Every request completes on its first poll, so the guest's per-frame task loop
   sees the result on the next frame. */

enum {
    LOCAL_USERS = 4,
    LOCAL_SESSIONS = 8,
    LOCAL_REQUESTS = 32,
    LOCAL_STAT_ROWS = 64,
    LOCAL_STAT_VALUE_BYTES = 8,
    /* The title this build is, from the certificate in the executable; the
       seeded friends play it so the same-title invite and join paths apply. */
    LOCAL_TITLE_ID = 0x54430006,
};

enum {
    LOCAL_REQUEST_SEARCH = 1,
    LOCAL_REQUEST_FIND,
    LOCAL_REQUEST_SESSION,
    LOCAL_REQUEST_STATS,
    LOCAL_REQUEST_LEADER,
    LOCAL_REQUEST_OTHER,
};

typedef struct LocalRequest {
    uint32_t id;
    uint32_t kind;
    uint32_t hresult;
    uint8_t kid[8];
    uint32_t board;
    uint64_t xuid;
    uint32_t first_rank;
    uint32_t count;
    uint64_t xuids[RECOMP_LIVE_MAX_FRIENDS];
    uint32_t xuid_count;
} LocalRequest;

typedef struct LocalStatRow {
    bool used;
    uint32_t board;
    uint64_t xuid;
    char gamertag[RECOMP_LIVE_GAMERTAG_BYTES];
    RecompLiveStat stats[RECOMP_LIVE_MAX_STAT_ATTRS];
    uint32_t stat_count;
} LocalStatRow;

typedef struct LocalState {
    bool logged_on;
    unsigned local_index;
    unsigned user_count;
    RecompLiveUser users[LOCAL_USERS];
    bool session_used[LOCAL_SESSIONS];
    RecompLiveSession sessions[LOCAL_SESSIONS];
    RecompLiveFriend friends[RECOMP_LIVE_MAX_FRIENDS];
    unsigned friend_count;
    uint64_t mutes[RECOMP_LIVE_MAX_MUTES];
    unsigned mute_count;
    bool invite_pending;
    RecompLiveFriend invite_from;
    uint32_t presence_state;
    uint8_t presence_kid[8];
    LocalStatRow stats[LOCAL_STAT_ROWS];
    unsigned stat_count;
    uint32_t connects;
} LocalState;

static LocalState local;
static LocalRequest local_requests[LOCAL_REQUESTS];
static uint32_t local_request_serial;
static uint32_t local_session_serial;

/* 10.x.y.z as the four guest bytes in order (network order, as the matchmaker
   carries it). */
static uint32_t local_ina(unsigned index)
{
    const uint8_t bytes[4] = {10u, 0u, 0u, (uint8_t)index};
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static void local_mac(uint64_t xuid, uint8_t mac[6])
{
    mac[0] = 0x00u;
    mac[1] = 0x50u;
    mac[2] = 0xf2u;
    mac[3] = (uint8_t)(xuid & 0xffu);
    mac[4] = (uint8_t)((xuid >> 8) & 0xffu);
    mac[5] = (uint8_t)((xuid >> 16) & 0xffu);
}

static void local_set_address(RecompLiveUser *user, unsigned index)
{
    memset(&user->address, 0, sizeof user->address);
    user->address.virtual_ina = local_ina(index);
    user->address.port = 1000u;
    local_mac(user->xuid, user->address.mac);
}

static void local_copy_tag(char *destination, const char *source)
{
    unsigned length = 0u;
    for (; source != NULL && source[length] != '\0' && length + 1u < RECOMP_LIVE_GAMERTAG_BYTES; ++length)
        destination[length] = source[length];
    destination[length] = '\0';
}

static unsigned local_add_user(uint64_t xuid, const char *gamertag)
{
    if (local.user_count >= LOCAL_USERS) return LOCAL_USERS;
    RecompLiveUser *user = &local.users[local.user_count];
    memset(user, 0, sizeof *user);
    user->xuid = xuid;
    local_copy_tag(user->gamertag, gamertag);
    local_set_address(user, local.user_count + 1u);
    return local.user_count++;
}

static int local_user_at(uint64_t xuid)
{
    for (unsigned i = 0u; i < local.user_count; ++i)
        if (local.users[i].xuid == xuid) return (int)i;
    return -1;
}

static int local_session_at(const uint8_t kid[8])
{
    for (unsigned i = 0u; i < LOCAL_SESSIONS; ++i)
        if (local.session_used[i] && memcmp(local.sessions[i].id.kid, kid, 8u) == 0) return (int)i;
    return -1;
}

static void local_session_id(uint8_t kid[8], uint8_t key[16], unsigned host_index)
{
    ++local_session_serial;
    memset(kid, 0, 8u);
    kid[0] = 0x44u; /* "D2" */
    kid[1] = 0x32u;
    kid[2] = 0x4cu; /* "L" */
    kid[3] = 0x56u; /* "V" */
    kid[4] = (uint8_t)(local_session_serial & 0xffu);
    kid[5] = (uint8_t)((local_session_serial >> 8) & 0xffu);
    kid[6] = (uint8_t)host_index;
    kid[7] = 0x01u;
    for (unsigned i = 0u; i < 16u; ++i) key[i] = (uint8_t)(0x5au ^ kid[i & 7u] ^ (i * 0x11u));
}

static void local_fill_attributes(uint8_t *attributes, uint32_t *size, const uint8_t *source, uint32_t source_size)
{
    memset(attributes, 0, RECOMP_LIVE_ATTRIBUTES_BYTES);
    uint32_t taken = source_size < RECOMP_LIVE_ATTRIBUTES_BYTES ? source_size : RECOMP_LIVE_ATTRIBUTES_BYTES;
    if (source != NULL && taken != 0u) memcpy(attributes, source, taken);
    *size = taken;
}

static int local_seed_session(unsigned host_index, const char tag[3], const uint32_t slots[4])
{
    unsigned index = 0u;
    while (index < LOCAL_SESSIONS && local.session_used[index]) ++index;
    if (index == LOCAL_SESSIONS) return -1;
    RecompLiveSession *session = &local.sessions[index];
    memset(session, 0, sizeof *session);
    local_session_id(session->id.kid, session->id.key, host_index);
    session->id.kid[7] = tag[0];
    session->id.kid[2] = tag[1];
    session->id.kid[3] = tag[2];
    session->host = local.users[host_index];
    memcpy(session->slots, slots, sizeof session->slots);
    uint8_t attributes[RECOMP_LIVE_ATTRIBUTES_BYTES];
    memset(attributes, 0, sizeof attributes);
    uint32_t match_type = 1u; /* winner-stays, same-title: the game's filter */
    memcpy(attributes, &match_type, sizeof match_type);
    local_fill_attributes(session->attributes, &session->attributes_size, attributes, sizeof attributes);
    local.session_used[index] = true;
    return (int)index;
}

static int local_seed_friend(uint64_t xuid, const char *gamertag, uint32_t flags)
{
    if (local.friend_count >= RECOMP_LIVE_MAX_FRIENDS) return -1;
    RecompLiveFriend *friend = &local.friends[local.friend_count];
    memset(friend, 0, sizeof *friend);
    friend->xuid = xuid;
    local_copy_tag(friend->gamertag, gamertag);
    friend->flags = flags;
    return (int)local.friend_count++;
}

static int local_friend_at(uint64_t xuid)
{
    for (unsigned i = 0u; i < local.friend_count; ++i)
        if (local.friends[i].xuid == xuid) return (int)i;
    return -1;
}

static void local_add_friend_if_missing(uint64_t xuid)
{
    if (xuid == 0u || local_friend_at(xuid) >= 0) return;
    const int user = local_user_at(xuid);
    local_seed_friend(xuid, user >= 0 ? local.users[user].gamertag : "Player", 0u);
}

static LocalStatRow *local_stat_row(uint32_t board, uint64_t xuid, bool create)
{
    for (unsigned i = 0u; i < local.stat_count; ++i)
        if (local.stats[i].used && local.stats[i].board == board && local.stats[i].xuid == xuid) return &local.stats[i];
    if (!create || local.stat_count >= LOCAL_STAT_ROWS) return NULL;
    LocalStatRow *row = &local.stats[local.stat_count++];
    memset(row, 0, sizeof *row);
    row->used = true;
    row->board = board;
    row->xuid = xuid;
    const int user = local_user_at(xuid);
    local_copy_tag(row->gamertag, user >= 0 ? local.users[user].gamertag : "Player");
    return row;
}

static int64_t local_stat_value(const LocalStatRow *row)
{
    return row != NULL && row->stat_count != 0u ? row->stats[0].value : 0;
}

static int local_rank_compare(const void *left, const void *right)
{
    const LocalStatRow *a = *(const LocalStatRow *const *)left;
    const LocalStatRow *b = *(const LocalStatRow *const *)right;
    const int64_t av = local_stat_value(a), bv = local_stat_value(b);
    if (av != bv) return av < bv ? 1 : -1; /* higher scores first */
    if (a->xuid != b->xuid) return a->xuid < b->xuid ? -1 : 1;
    return 0;
}

/* Board order by the first stat's value, as RANKINGS shows it. */
static unsigned local_rank_board(uint32_t board, const LocalStatRow **rows, unsigned capacity)
{
    unsigned count = 0u;
    for (unsigned i = 0u; i < local.stat_count && count < capacity; ++i)
        if (local.stats[i].used && local.stats[i].board == board) rows[count++] = &local.stats[i];
    qsort(rows, count, sizeof rows[0], local_rank_compare);
    return count;
}

static uint32_t local_rank_of(uint32_t board, uint64_t xuid)
{
    const LocalStatRow *rows[LOCAL_STAT_ROWS];
    const unsigned count = local_rank_board(board, rows, LOCAL_STAT_ROWS);
    for (unsigned i = 0u; i < count; ++i)
        if (rows[i]->xuid == xuid) return i + 1u;
    return 0u;
}

static LocalRequest *local_request_new(uint32_t kind)
{
    for (unsigned i = 0u; i < LOCAL_REQUESTS; ++i) {
        LocalRequest *request = &local_requests[i];
        if (request->id != 0u) continue;
        memset(request, 0, sizeof *request);
        request->id = ++local_request_serial;
        if (request->id == 0u) request->id = ++local_request_serial;
        request->kind = kind;
        request->hresult = 0u;
        return request;
    }
    return NULL;
}

static LocalRequest *local_request_at(RecompLiveRequest id)
{
    for (unsigned i = 0u; i < LOCAL_REQUESTS; ++i)
        if (local_requests[i].id == id && id != 0u) return &local_requests[i];
    return NULL;
}

static RecompLiveRequest local_request_finish(uint32_t kind)
{
    LocalRequest *request = local_request_new(kind);
    return request != NULL ? request->id : 0u;
}

static RecompLiveRequest local_other_request(uint32_t hresult)
{
    LocalRequest *request = local_request_new(LOCAL_REQUEST_OTHER);
    if (request == NULL) return 0u;
    request->hresult = hresult;
    return request->id;
}

void recomp_live_backend_local_reset(void)
{
    memset(&local, 0, sizeof local);
    memset(local_requests, 0, sizeof local_requests);
    local_request_serial = 0u;
    local_session_serial = 0u;
    local.local_index = local_add_user(0x0009000000a1b2c2ull, "LocalPlayer");
    local_add_user(0x0009000000a1b2c3ull, "Ann");
    local_add_user(0x0009000000a1b2c4ull, "Bob");
    local_add_user(0x0009000000a1b2c5ull, "Cara");

    const uint32_t open_slots[RECOMP_LIVE_SESSION_SLOTS] = {1u, 3u, 0u, 0u};
    const uint32_t bob_slots[RECOMP_LIVE_SESSION_SLOTS] = {2u, 2u, 0u, 0u};
    const char ann_tag[3] = {0x01u, 0x41u, 0x4eu};  /* "AN" */
    const char bob_tag[3] = {0x02u, 0x42u, 0x4fu};  /* "BO" */
    local_seed_session(1u, ann_tag, open_slots);
    local_seed_session(2u, bob_tag, bob_slots);

    const uint8_t invite_kid[8] = {0x44u, 0x32u, 0x49u, 0x4eu, 0x56u, 0x01u, 0x03u, 0x02u};
    local_seed_friend(local.users[1].xuid, local.users[1].gamertag,
        RECOMP_LIVE_FRIEND_ONLINE | RECOMP_LIVE_FRIEND_PLAYING | RECOMP_LIVE_FRIEND_JOINABLE);
    local_seed_friend(local.users[2].xuid, local.users[2].gamertag, RECOMP_LIVE_FRIEND_RECEIVED_REQUEST);
    local_seed_friend(local.users[3].xuid, local.users[3].gamertag,
        RECOMP_LIVE_FRIEND_ONLINE | RECOMP_LIVE_FRIEND_RECEIVED_INVITE);
    memcpy(local.friends[2].session_kid, invite_kid, 8u);
    local.friends[0].title_id = LOCAL_TITLE_ID;
    local.friends[1].title_id = LOCAL_TITLE_ID;
    local.friends[2].title_id = LOCAL_TITLE_ID;

    local.mutes[local.mute_count++] = local.users[3].xuid;

    /* A starting RANKINGS board: wins as stat 1, losses as stat 2. */
    const struct { uint64_t xuid; int64_t wins, losses; } seed_stats[LOCAL_USERS] = {
        {local.users[0].xuid, 10, 3},
        {local.users[1].xuid, 25, 1},
        {local.users[2].xuid, 18, 8},
        {local.users[3].xuid, 4, 12},
    };
    for (unsigned i = 0u; i < LOCAL_USERS; ++i) {
        LocalStatRow *row = local_stat_row(1u, seed_stats[i].xuid, true);
        if (row == NULL) continue;
        row->stats[0].id = 1u;
        row->stats[0].type = 2u; /* int64 */
        row->stats[0].value = seed_stats[i].wins;
        row->stats[1].id = 2u;
        row->stats[1].type = 2u;
        row->stats[1].value = seed_stats[i].losses;
        row->stat_count = 2u;
    }
    /* A second board so a read names a board and stays separate. */
    LocalStatRow *second = local_stat_row(2u, local.users[0].xuid, true);
    if (second != NULL) {
        second->stats[0].id = 0x10u;
        second->stats[0].type = 1u; /* int32 */
        second->stats[0].value = 7;
        second->stat_count = 1u;
    }
}

/* --- users and logon --- */

static bool local_local_user(void *self, RecompLiveUser *user)
{
    (void)self;
    if (local.user_count == 0u) return false;
    *user = local.users[local.local_index];
    local_set_address(user, local.local_index + 1u);
    return true;
}

static RecompLiveRequest local_logon(void *self)
{
    (void)self;
    local.logged_on = true;
    return local_other_request(0u);
}

static void local_logoff(void *self)
{
    (void)self;
    local.logged_on = false;
    local.invite_pending = false;
}

/* --- sessions and search --- */

static RecompLiveRequest local_session_create(void *self, const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS],
    const uint8_t *attributes, uint32_t attributes_size)
{
    (void)self;
    unsigned index = 0u;
    while (index < LOCAL_SESSIONS && local.session_used[index]) ++index;
    if (index == LOCAL_SESSIONS) return local_other_request(0x80004005u);
    RecompLiveSession *session = &local.sessions[index];
    memset(session, 0, sizeof *session);
    local_session_id(session->id.kid, session->id.key, local.local_index);
    session->host = local.users[local.local_index];
    local_set_address(&session->host, local.local_index + 1u);
    memcpy(session->slots, slots, sizeof session->slots);
    local_fill_attributes(session->attributes, &session->attributes_size, attributes, attributes_size);
    local.session_used[index] = true;
    LocalRequest *request = local_request_new(LOCAL_REQUEST_SESSION);
    if (request == NULL) return 0u;
    memcpy(request->kid, session->id.kid, 8u);
    return request->id;
}

static RecompLiveRequest local_session_update(void *self, const uint8_t kid[8],
    const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS], const uint8_t *attributes, uint32_t attributes_size)
{
    (void)self;
    const int index = local_session_at(kid);
    if (index < 0) return local_other_request(0x80004005u);
    RecompLiveSession *session = &local.sessions[index];
    memcpy(session->slots, slots, sizeof session->slots);
    local_fill_attributes(session->attributes, &session->attributes_size, attributes, attributes_size);
    LocalRequest *request = local_request_new(LOCAL_REQUEST_SESSION);
    if (request == NULL) return 0u;
    memcpy(request->kid, kid, 8u);
    return request->id;
}

static RecompLiveRequest local_session_delete(void *self, const uint8_t kid[8])
{
    (void)self;
    const int index = local_session_at(kid);
    if (index < 0) return local_other_request(0x80004005u);
    local.session_used[index] = false;
    LocalRequest *request = local_request_new(LOCAL_REQUEST_SESSION);
    if (request == NULL) return 0u;
    memcpy(request->kid, kid, 8u);
    return request->id;
}

static bool local_session_id_of(void *self, RecompLiveRequest id, RecompLiveSessionId *out)
{
    (void)self;
    LocalRequest *request = local_request_at(id);
    if (request == NULL || request->kind != LOCAL_REQUEST_SESSION) return false;
    const int index = local_session_at(request->kid);
    if (index < 0 || !local.session_used[index]) return false;
    *out = local.sessions[index].id;
    return true;
}

static RecompLiveRequest local_search(void *self, uint32_t procedure, const uint8_t *filters, uint32_t filters_size)
{
    (void)self;
    (void)procedure;
    (void)filters;
    (void)filters_size;
    return local_request_finish(LOCAL_REQUEST_SEARCH);
}

static RecompLiveRequest local_find(void *self, const uint8_t kid[8])
{
    (void)self;
    LocalRequest *request = local_request_new(LOCAL_REQUEST_FIND);
    if (request == NULL) return 0u;
    memcpy(request->kid, kid, 8u);
    if (local_session_at(kid) < 0) request->hresult = 0x80004005u;
    return request->id;
}

static uint32_t local_results(void *self, RecompLiveRequest id, RecompLiveSession *out, uint32_t capacity)
{
    (void)self;
    LocalRequest *request = local_request_at(id);
    if (request == NULL || capacity == 0u) return 0u;
    unsigned count = 0u;
    if (request->kind == LOCAL_REQUEST_SEARCH) {
        for (unsigned i = 0u; i < LOCAL_SESSIONS && count < capacity; ++i) {
            if (!local.session_used[i]) continue;
            const RecompLiveSession *session = &local.sessions[i];
            if (session->host.xuid == local.users[local.local_index].xuid) continue;
            if (session->slots[1] + session->slots[3] == 0u) continue; /* open slots only */
            out[count++] = *session;
        }
    } else if (request->kind == LOCAL_REQUEST_FIND) {
        const int index = local_session_at(request->kid);
        if (index >= 0 && local.session_used[index]) out[count++] = local.sessions[index];
    }
    return count;
}

static void local_connect(void *self, uint32_t virtual_ina)
{
    (void)self;
    (void)virtual_ina;
    ++local.connects;
}

/* --- friends --- */

static RecompLiveRequest local_friends_enumerate(void *self)
{
    (void)self;
    return local_request_finish(LOCAL_REQUEST_OTHER);
}

static uint32_t local_friends(void *self, RecompLiveFriend *out, uint32_t capacity)
{
    (void)self;
    unsigned count = 0u;
    for (; count < local.friend_count && count < capacity; ++count) out[count] = local.friends[count];
    return count;
}

static RecompLiveRequest local_friend_request(void *self, uint64_t xuid)
{
    (void)self;
    if (xuid == 0u) return local_other_request(0x80004005u);
    local_add_friend_if_missing(xuid);
    const int index = local_friend_at(xuid);
    if (index < 0) return local_other_request(0x80004005u);
    local.friends[index].flags |= RECOMP_LIVE_FRIEND_SENT_REQUEST;
    return local_other_request(0u);
}

static RecompLiveRequest local_friend_answer(void *self, uint64_t xuid, RecompLiveAnswer answer)
{
    (void)self;
    const int index = local_friend_at(xuid);
    if (index < 0) return local_other_request(0x80004005u);
    RecompLiveFriend *friend = &local.friends[index];
    friend->flags &= ~(RECOMP_LIVE_FRIEND_SENT_REQUEST | RECOMP_LIVE_FRIEND_RECEIVED_REQUEST);
    if (answer == RECOMP_LIVE_YES) friend->flags |= RECOMP_LIVE_FRIEND_ONLINE;
    return local_other_request(0u);
}

static RecompLiveRequest local_friend_remove(void *self, uint64_t xuid)
{
    (void)self;
    const int index = local_friend_at(xuid);
    if (index < 0) return local_other_request(0x80004005u);
    for (unsigned i = (unsigned)index; i + 1u < local.friend_count; ++i) local.friends[i] = local.friends[i + 1u];
    --local.friend_count;
    return local_other_request(0u);
}

static RecompLiveRequest local_game_invite(void *self, const uint8_t kid[8], const uint64_t *xuids, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0u; i < count; ++i) {
        const int index = local_friend_at(xuids[i]);
        if (index < 0) continue;
        local.friends[index].flags |= RECOMP_LIVE_FRIEND_SENT_INVITE;
        memcpy(local.friends[index].session_kid, kid, 8u);
    }
    return local_other_request(0u);
}

static RecompLiveRequest local_revoke_invite(void *self, const uint8_t kid[8], const uint64_t *xuids, uint32_t count)
{
    (void)self;
    for (uint32_t i = 0u; i < count; ++i) {
        const int index = local_friend_at(xuids[i]);
        if (index >= 0) local.friends[index].flags &= ~RECOMP_LIVE_FRIEND_SENT_INVITE;
    }
    (void)kid;
    return local_other_request(0u);
}

static RecompLiveRequest local_answer_invite(void *self, uint64_t xuid, RecompLiveAnswer answer)
{
    (void)self;
    const int index = local_friend_at(xuid);
    if (index < 0) return local_other_request(0x80004005u);
    RecompLiveFriend *friend = &local.friends[index];
    friend->flags &= ~RECOMP_LIVE_FRIEND_RECEIVED_INVITE;
    if (answer == RECOMP_LIVE_YES) {
        friend->flags |= RECOMP_LIVE_FRIEND_INVITE_ACCEPTED;
        local.invite_pending = true;
        local.invite_from = *friend;
    } else {
        friend->flags |= RECOMP_LIVE_FRIEND_INVITE_REJECTED;
    }
    return local_other_request(0u);
}

static bool local_accepted_invite(void *self, RecompLiveFriend *from)
{
    (void)self;
    if (!local.invite_pending) return false;
    *from = local.invite_from;
    return true;
}

static void local_presence(void *self, uint32_t state, const uint8_t kid[8])
{
    (void)self;
    local.presence_state = state;
    if (kid != NULL) memcpy(local.presence_kid, kid, 8u);
}

/* --- mutelist and feedback --- */

static RecompLiveRequest local_mutelist(void *self)
{
    (void)self;
    return local_request_finish(LOCAL_REQUEST_OTHER);
}

static uint32_t local_mutes(void *self, uint64_t *out, uint32_t capacity)
{
    (void)self;
    unsigned count = 0u;
    for (; count < local.mute_count && count < capacity; ++count) out[count] = local.mutes[count];
    return count;
}

static RecompLiveRequest local_feedback(void *self, uint64_t xuid, uint32_t type)
{
    (void)self;
    (void)type;
    if (xuid == 0u) return local_other_request(0x80004005u);
    return local_other_request(0u);
}

/* --- stats and rankings --- */

static RecompLiveRequest local_stat_write(void *self, uint32_t board, uint64_t xuid,
    const RecompLiveStat *stats, uint32_t count)
{
    (void)self;
    if (xuid == 0u || stats == NULL) return local_other_request(0x80004005u);
    LocalStatRow *row = local_stat_row(board, xuid, true);
    if (row == NULL) return local_other_request(0x80004005u);
    for (uint32_t i = 0u; i < count; ++i) {
        /* Id 0 is a real stat (group 7's first); count bounds the list. */
        unsigned slot = row->stat_count;
        for (unsigned existing = 0u; existing < row->stat_count; ++existing)
            if (row->stats[existing].id == stats[i].id) { slot = existing; break; }
        if (slot >= RECOMP_LIVE_MAX_STAT_ATTRS) {
            /* Only a changed group table could get here (group 7 has 61). */
            fprintf(stderr, "recomp live: local stat row %u is full, dropping stat %u\n", board, stats[i].id);
            continue;
        }
        row->stats[slot] = stats[i];
        if (slot == row->stat_count) ++row->stat_count;
    }
    return local_other_request(0u);
}

static RecompLiveRequest local_stat_read(void *self, uint32_t board, const uint64_t *xuids, uint32_t count)
{
    (void)self;
    LocalRequest *request = local_request_new(LOCAL_REQUEST_STATS);
    if (request == NULL) return 0u;
    request->board = board;
    request->xuid_count = 0u;
    for (uint32_t i = 0u; i < count && i < RECOMP_LIVE_MAX_FRIENDS; ++i) request->xuids[request->xuid_count++] = xuids[i];
    return request->id;
}

static uint32_t local_stat_results(void *self, RecompLiveRequest id, RecompLiveLeaderRow *out, uint32_t capacity)
{
    (void)self;
    LocalRequest *request = local_request_at(id);
    if (request == NULL || out == NULL || capacity == 0u) return 0u;
    if (request->kind == LOCAL_REQUEST_STATS) {
        unsigned count = 0u;
        for (uint32_t i = 0u; i < request->xuid_count && count < capacity; ++i) {
            LocalStatRow *row = local_stat_row(request->board, request->xuids[i], false);
            if (row == NULL) continue;
            RecompLiveLeaderRow *result = &out[count++];
            memset(result, 0, sizeof *result);
            result->xuid = row->xuid;
            local_copy_tag(result->gamertag, row->gamertag);
            result->rank = local_rank_of(request->board, row->xuid);
            result->stat_count = row->stat_count;
            memcpy(result->stats, row->stats, sizeof row->stats);
        }
        return count;
    }
    if (request->kind == LOCAL_REQUEST_LEADER) {
        const LocalStatRow *rows[LOCAL_STAT_ROWS];
        unsigned total = local_rank_board(request->board, rows, LOCAL_STAT_ROWS);
        unsigned start = request->first_rank == 0u ? 0u : request->first_rank - 1u;
        unsigned count = 0u;
        for (unsigned i = start; i < total && count < capacity; ++i, ++count) {
            RecompLiveLeaderRow *result = &out[count];
            memset(result, 0, sizeof *result);
            result->xuid = rows[i]->xuid;
            local_copy_tag(result->gamertag, rows[i]->gamertag);
            result->rank = i + 1u;
            result->stat_count = rows[i]->stat_count;
            memcpy(result->stats, rows[i]->stats, sizeof rows[i]->stats);
        }
        return count;
    }
    return 0u;
}

static RecompLiveRequest local_leader_enumerate(void *self, uint32_t board, uint64_t pivot_xuid,
    uint32_t first_rank, uint32_t count)
{
    (void)self;
    LocalRequest *request = local_request_new(LOCAL_REQUEST_LEADER);
    if (request == NULL) return 0u;
    request->board = board;
    request->xuid = pivot_xuid;
    request->first_rank = first_rank;
    request->count = count;
    return request->id;
}

static uint32_t local_leader_total(void *self, RecompLiveRequest id)
{
    (void)self;
    LocalRequest *request = local_request_at(id);
    if (request == NULL || request->kind != LOCAL_REQUEST_LEADER) return 0u;
    const LocalStatRow *rows[LOCAL_STAT_ROWS];
    return local_rank_board(request->board, rows, LOCAL_STAT_ROWS);
}

/* --- polling --- */

static RecompLiveStatus local_poll(void *self, RecompLiveRequest id, uint32_t *hresult)
{
    (void)self;
    LocalRequest *request = local_request_at(id);
    if (request == NULL) {
        if (hresult != NULL) *hresult = 0x80004005u;
        return RECOMP_LIVE_FAILED;
    }
    if (hresult != NULL) *hresult = request->hresult;
    return request->hresult == 0u ? RECOMP_LIVE_DONE : RECOMP_LIVE_FAILED;
}

static void local_release(void *self, RecompLiveRequest id)
{
    (void)self;
    LocalRequest *request = local_request_at(id);
    if (request != NULL) memset(request, 0, sizeof *request);
}

static void local_tick(void *self, uint64_t now_ms)
{
    (void)self;
    (void)now_ms;
}

static const RecompLiveBackend local_backend = {
    NULL,
    local_local_user,
    local_logon,
    local_logoff,
    local_session_create,
    local_session_update,
    local_session_delete,
    local_session_id_of,
    local_search,
    local_find,
    local_results,
    local_connect,
    local_friends_enumerate,
    local_friends,
    local_friend_request,
    local_friend_answer,
    local_friend_remove,
    local_game_invite,
    local_revoke_invite,
    local_answer_invite,
    local_accepted_invite,
    local_presence,
    local_mutelist,
    local_mutes,
    local_feedback,
    local_stat_write,
    local_stat_read,
    local_stat_results,
    local_leader_enumerate,
    local_leader_total,
    local_poll,
    local_release,
    local_tick,
};

const RecompLiveBackend *recomp_live_backend_local(void)
{
    return &local_backend;
}
