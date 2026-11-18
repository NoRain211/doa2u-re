/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_client.h"

#include "live_transport.h"
#include "matchmaker_wire.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* tools/matchmaker.py and docs/matchmaking.md: matchmaker_wire.h frames a
   packet as "D2MM", a version, a kind, this socket's nonce, then for a request
   the server's cookie, then a body whose numbers are big endian. An answer
   starts its body after the nonce. The transport owns the nonce, the cookie and
   the retry the server's COOKIE answer asks for, and fills those bytes in. */

enum {
    LIVE_DIGEST_BYTES = 64u,

    /* Request sizes. SESSION_PUT, SEARCH, STAT_WRITE and STAT_READ add their list. */
    LIVE_CHALLENGE_BYTES = MATCH_ASKED + 4u + 8u,
    LIVE_LOGON_BYTES = MATCH_ASKED + 4u + 8u + 32u + 64u + 4u + 4u + 8u + 1u + 15u,
    LIVE_LOGON_ANSWER = MATCH_ASKED + 4u + 8u, /* where a LOGON's 32-byte answer is */
    LIVE_NONCE_BYTES = 16u,                    /* the server's nonce, which that answer is made over */
    LIVE_SESSION_PUT_BYTES = MATCH_ASKED + 4u + 8u + 16u + 2u,
    LIVE_SESSION_DEL_BYTES = MATCH_ASKED + 4u + 8u,
    LIVE_SEARCH_BYTES = MATCH_ASKED + 4u + 4u + 2u,
    LIVE_FIND_BYTES = MATCH_ASKED + 4u + 8u,
    LIVE_CONNECT_BYTES = MATCH_ASKED + 4u + 4u,
    LIVE_QUERY_BYTES = MATCH_ASKED + 4u, /* FRIENDS and MUTES */
    LIVE_FRIEND_OP_BYTES = MATCH_ASKED + 4u + 1u + 8u + 1u + 8u,
    LIVE_FEEDBACK_BYTES = MATCH_ASKED + 4u + 8u + 4u,
    LIVE_STAT_WRITE_BYTES = MATCH_ASKED + 4u + 4u + 8u + 1u,
    LIVE_STAT_ITEM_BYTES = 11u,
    LIVE_STAT_READ_BYTES = MATCH_ASKED + 4u + 4u + 1u,
    LIVE_LEADERS_BYTES = MATCH_ASKED + 4u + 4u + 8u + 4u + 1u,
    LIVE_GROUP_BYTES = LIVE_FRIEND_OP_BYTES,

    /* Answer body sizes, after the header and nonce. */
    LIVE_CHALLENGED_BYTES = 4u + 1u + LIVE_NONCE_BYTES, /* a NONCE */
    LIVE_LOGGED_BYTES = 4u + 4u + 4u + 1u + 15u,
    LIVE_SESSION_OK_BYTES = 4u + 4u + 8u + 16u,
    LIVE_RESULT_BYTES = 4u + 4u + 1u + 1u,
    LIVE_RESULT_ROW_BYTES = 4u + 4u + 1u + 1u + 8u + 16u + 8u + 16u + 4u + 16u + 2u,
    LIVE_FRIEND_BYTES = 4u + 1u + 1u + 8u + 16u + 4u + 4u + 8u,
    LIVE_DONE_BYTES = 4u + 4u,
    LIVE_PUSH_BYTES = 1u + 8u + 8u,
    LIVE_MUTELIST_BYTES = 4u + 4u + 1u,
    LIVE_STATROW_BYTES = 4u + 4u + 1u + 1u + 4u + 8u + 16u + 4u + 1u,
    LIVE_STAT_BYTES = 11u, /* a stat in an answer: id, type, value */
    LIVE_LPEER_BYTES = 4u + 6u + 6u + 8u + 1u,

    /* Timing. */
    LIVE_RETRY_MS = 500u,
    LIVE_TIMEOUT_MS = 5000u,
    LIVE_HEARTBEAT_MS = 5000u,
    /* A CONNECT repeats this often while its LPEER is missing, for as long as
       the matchmaker keeps a repeated request's answer (60 s). */
    LIVE_CONNECT_MS = 1000u,
    LIVE_CONNECT_GIVE_UP_MS = 60000u,

    LIVE_REQUEST_SLOTS = 16u,
    LIVE_CONNECT_SLOTS = 8u, /* one per peer slot in live_transport.c */
    LIVE_GROUP_CHILDREN = 8u,
    LIVE_REMEMBERED_INVITES = 8u,
    LIVE_ROWS_SEEN_BYTES = (RECOMP_LIVE_MAX_LEADER_ROWS + 7u) / 8u,
    LIVE_PORT = 1000u, /* the game's socket port, which the XNADDR carries */
    /* DOA2U's title id, the XBE certificate's at VA 0x10184 + 8, which the
       library copies into its context and XOnlineTitleIdIsSameTitle compares
       against. A friend row carries the title the friend logged on with. */
    LIVE_TITLE = 0x54430006u,
};

/* RANKINGS packing: a stat is 11 bytes on the wire (id u16, type u8, value
   i64), and a STATROW answer carries one row, as the game reads one record
   per row anyway. A group-7 write (61 stats) is 22 + 17 + 61 * 11 = 710 bytes
   and an answer row of 64 stats is 14 + 43 + 64 * 11 = 761, both inside the
   1,200-byte packet cap, so neither is split. */
_Static_assert(LIVE_STAT_WRITE_BYTES + RECOMP_LIVE_MAX_STAT_ATTRS * LIVE_STAT_ITEM_BYTES <= RECOMP_LIVE_PACKET_BYTES,
    "a full STAT_WRITE must fit one packet");
_Static_assert(MATCH_BODY + LIVE_STATROW_BYTES + RECOMP_LIVE_MAX_STAT_ATTRS * LIVE_STAT_BYTES <=
    RECOMP_LIVE_PACKET_BYTES, "a full STATROW must fit one packet");

/* What the client itself decides, as the HRESULT its poll reports. */
enum {
    LIVE_HRESULT_FAIL = 0x80004005u,
    LIVE_HRESULT_INVALIDARG = 0x80070057u,
    LIVE_HRESULT_ACCESSDENIED = 0x80070005u,
    LIVE_HRESULT_TIMEOUT = 0x800705B4u,
};

typedef struct LiveRequestState {
    uint32_t id;   /* the request id on the wire; 0 when the slot is free */
    uint8_t kind;  /* the kind that was sent */
    uint8_t op;    /* FRIEND_OP's op */
    int8_t status; /* RecompLiveStatus */
    uint32_t hresult;
    uint64_t sent_ms, last_ms; /* the 5 s window, and the last send */
    uint16_t size;
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    uint32_t total, count; /* rows the answer holds, and those that arrived */
    uint8_t seen[LIVE_ROWS_SEEN_BYTES];
    uint32_t board_total;
    uint32_t virtual_ina;
    char gamertag[RECOMP_LIVE_GAMERTAG_BYTES];
    RecompLiveSessionId session;
    /* A group request (a game invite to several friends) and its children. */
    uint32_t head; /* 0 outside a group, else the head slot's index plus one */
    uint32_t subs[LIVE_GROUP_CHILDREN]; /* the head's own id first, then its children */
    uint32_t sub_count;
    bool own_done;
    uint32_t own_hresult;
    uint64_t xuid; /* FRIEND_OP's target, also an accepted invite's sender */
    uint8_t kid[8];
    uint8_t answer;
    bool owned; /* the client started it (a push), so the client frees it too */
    union {
        RecompLiveSession sessions[RECOMP_LIVE_MAX_RESULTS];
        RecompLiveFriend friends[RECOMP_LIVE_MAX_FRIENDS];
        uint64_t mutes[RECOMP_LIVE_MAX_MUTES];
        RecompLiveLeaderRow rows[RECOMP_LIVE_MAX_LEADER_ROWS];
    } answer_at;
} LiveRequestState;

/* A CONNECT this side sent for one peer, kept so the pairing's LPEER can stop
   its repeats and so an introduction this side did not ask for is answered
   once, not on every repeat. */
typedef struct LiveConnect {
    uint32_t virtual_ina; /* 0 when the slot is free */
    bool repeating;       /* the peer's LPEER has not come yet */
    uint64_t last_ms;     /* the last send */
    uint64_t until_ms;    /* the repeats stop here, outlasting a lost packet */
    uint8_t token[8];     /* the pairing its last LPEER named */
    uint16_t size;
    uint8_t packet[LIVE_CONNECT_BYTES];
} LiveConnect;

typedef struct LiveClient {
    RecompLiveBackend backend;
    bool attached;
    RecompLiveIdentity identity;
    char server[256]; /* the matchmaker as "host:port" in lowercase, which the identity's list names it by */
    bool server_known; /* logged on there in this run, whatever came of writing the list */
    char digest[LIVE_DIGEST_BYTES];
    RecompLiveUser user;
    bool logged_on;
    uint32_t virtual_ina;
    uint64_t now_ms;
    uint64_t heartbeat_ms;
    bool heartbeat_due;
    uint32_t presence;
    uint8_t presence_kid[8];
    LiveRequestState requests[LIVE_REQUEST_SLOTS];
    uint32_t next_id;
    /* The pairings this side asked for and whose LPEER has not come. */
    LiveConnect connects[LIVE_CONNECT_SLOTS];
    /* The latest friend list, which friends() hands out. */
    RecompLiveFriend friends[RECOMP_LIVE_MAX_FRIENDS];
    uint32_t friend_count;
    /* The invite this player accepted, which accepted_invite() hands out. */
    RecompLiveFriend accepted;
    bool has_accepted;
    /* The latest mute list, which mutes() hands out. */
    uint64_t mutes[RECOMP_LIVE_MAX_MUTES];
    uint32_t mute_count;
    /* Invites pushed to this player, so answer_invite knows the kid. */
    struct { uint64_t xuid; uint8_t kid[8]; } invites[LIVE_REMEMBERED_INVITES];
    uint32_t invite_count;
    /* Set while a push's re-enumeration is in flight, so one push is one ask. */
    bool friends_asking;
} LiveClient;

/* One player per process: Live runs with the game, not beside it. */
static LiveClient live;

static void put16be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put64be(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0u; i < 8u; ++i) out[i] = (uint8_t)(value >> (56u - 8u * i));
}

static uint16_t get16be(const uint8_t *in)
{
    return (uint16_t)((uint16_t)in[0] << 8 | in[1]);
}

static uint64_t get64be(const uint8_t *in)
{
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i) value = value << 8 | in[i];
    return value;
}

/* A gamertag field: the ASCII before its first zero, at most 15 bytes. */
static void copy_tag(char out[RECOMP_LIVE_GAMERTAG_BYTES], const uint8_t *field)
{
    size_t length = 0u;
    memset(out, 0, RECOMP_LIVE_GAMERTAG_BYTES);
    while (length + 1u < RECOMP_LIVE_GAMERTAG_BYTES && field[length] != 0u) {
        out[length] = (char)field[length];
        ++length;
    }
}

static uint32_t next_request_id(void)
{
    do {
        ++live.next_id;
    } while (live.next_id == 0u);
    return live.next_id;
}

/* The 00:50:F2 MAC plus the low three bytes of the XUID, as the XNADDR has it. */
static void fill_address(RecompLiveUser *user)
{
    user->address.virtual_ina = live.virtual_ina;
    user->address.port = LIVE_PORT;
    user->address.mac[0] = 0x00u;
    user->address.mac[1] = 0x50u;
    user->address.mac[2] = 0xF2u;
    user->address.mac[3] = (uint8_t)live.identity.xuid;
    user->address.mac[4] = (uint8_t)(live.identity.xuid >> 8);
    user->address.mac[5] = (uint8_t)(live.identity.xuid >> 16);
}

static void send_packet(const uint8_t *packet, size_t size)
{
    recomp_live_transport_send_server(packet, size);
}

/* Starts a request packet and returns where its body goes, or NULL when it does not fit. */
static uint8_t *begin_request(uint8_t kind, uint32_t id, uint8_t *packet, size_t capacity, size_t size)
{
    static const uint8_t zero_token[RECOMP_NETPLAY_TOKEN_BYTES] = {0u};
    if (packet == NULL || capacity < size) return NULL;
    /* The nonce and the cookie are the transport's: it writes them when this
       packet goes out, and writes them again on a resend. */
    uint8_t *at = match_header(packet, kind, zero_token, zero_token);
    put32be(at, id);
    memset(at + 4u, 0, size - MATCH_ASKED - 4u);
    return at + 4u;
}

static LiveRequestState *new_request(uint8_t kind, uint32_t *id)
{
    for (uint32_t i = 0u; i < LIVE_REQUEST_SLOTS; ++i) {
        LiveRequestState *slot = &live.requests[i];
        if (slot->id != 0u) continue;
        memset(slot, 0, sizeof *slot);
        slot->id = next_request_id();
        slot->kind = kind;
        slot->status = RECOMP_LIVE_PENDING;
        *id = slot->id;
        return slot;
    }
    *id = 0u;
    return NULL;
}

static LiveRequestState *find_request(uint32_t id)
{
    if (id == 0u) return NULL;
    for (uint32_t i = 0u; i < LIVE_REQUEST_SLOTS; ++i)
        if (live.requests[i].id == id) return &live.requests[i];
    return NULL;
}

static LiveRequestState *slot_at(uint32_t index)
{
    return index < LIVE_REQUEST_SLOTS ? &live.requests[index] : NULL;
}

static uint32_t slot_index(const LiveRequestState *slot)
{
    return (uint32_t)(slot - live.requests);
}

/* Every terminal transition of a request ends here, so the flag a push's friend
   ask set is freed whichever way it ends: an answer, a refusal or a timeout. */
static void finish_request(LiveRequestState *slot, uint32_t hresult)
{
    slot->status = hresult == 0u ? RECOMP_LIVE_DONE : RECOMP_LIVE_FAILED;
    slot->hresult = hresult;
    if (slot->kind == LIVE_FRIENDS && slot->sub_count == 0u) live.friends_asking = false;
}

/* A request the client started for itself (a push's friend list) has no guest
   caller to release it: free the slot once its result is stored. */
static void drop_owned(LiveRequestState *slot)
{
    if (slot == NULL || !slot->owned || slot->status == RECOMP_LIVE_PENDING) return;
    memset(slot, 0, sizeof *slot);
}

/* Sends a built request, keeping it for the resends inside the 5 s window. */
static bool send_request(LiveRequestState *slot, const uint8_t *packet, size_t size)
{
    if (slot == NULL || size > sizeof slot->packet) return false;
    memcpy(slot->packet, packet, size);
    slot->size = (uint16_t)size;
    slot->sent_ms = slot->last_ms = live.now_ms;
    send_packet(slot->packet, size);
    return true;
}

/* The pairing this side sent a CONNECT for, and whether its LPEER is missing. */
static LiveConnect *connect_find(uint32_t virtual_ina)
{
    for (uint32_t i = 0u; i < LIVE_CONNECT_SLOTS; ++i)
        if (live.connects[i].virtual_ina == virtual_ina) return &live.connects[i];
    return NULL;
}

/* A slot for one pairing, the least recently used when every one is held. */
static LiveConnect *connect_claim(uint32_t virtual_ina)
{
    LiveConnect *oldest = &live.connects[0];
    for (uint32_t i = 0u; i < LIVE_CONNECT_SLOTS; ++i) {
        LiveConnect *slot = &live.connects[i];
        if (slot->virtual_ina == 0u) {
            memset(slot, 0, sizeof *slot);
            slot->virtual_ina = virtual_ina;
            return slot;
        }
        if (slot->last_ms < oldest->last_ms) oldest = slot;
    }
    memset(oldest, 0, sizeof *oldest);
    oldest->virtual_ina = virtual_ina;
    return oldest;
}

/* CONNECT names the player to pair with, under the request id its repeats keep. */
static bool build_connect(uint8_t *packet, size_t capacity, uint32_t id, uint32_t virtual_ina)
{
    uint8_t *at = begin_request(LIVE_CONNECT, id, packet, capacity, LIVE_CONNECT_BYTES);
    if (at == NULL) return false;
    put32be(at, virtual_ina);
    return true;
}

/* Asks the matchmaker to pair this player with another, and keeps the ask so it
   can repeat with the same request id until the other player is heard: its
   repeats also re-send the other side's LPEER, which may be the one lost. A
   joiner asks when its first datagram needs a route, and again when the
   transport hears nothing back; a host asks after the joiner's LPEER, which is
   how the matchmaker comes to grant each side the other's stats. An ask still
   repeating is left alone, so LPEERs never start a ping-pong of CONNECTs. */
static void ask_connect(uint32_t virtual_ina)
{
    uint8_t packet[LIVE_CONNECT_BYTES];
    if (!live.logged_on || virtual_ina == 0u) return;
    LiveConnect *ask = connect_find(virtual_ina);
    if (ask != NULL && ask->repeating) return;
    if (ask == NULL) ask = connect_claim(virtual_ina);
    if (ask == NULL) return;
    const uint32_t id = next_request_id();
    if (!build_connect(packet, sizeof packet, id, virtual_ina)) {
        ask->virtual_ina = 0u;
        return;
    }
    memcpy(ask->packet, packet, sizeof packet);
    ask->size = LIVE_CONNECT_BYTES;
    ask->last_ms = live.now_ms;
    ask->until_ms = live.now_ms + LIVE_CONNECT_GIVE_UP_MS;
    ask->repeating = true;
    send_packet(ask->packet, ask->size);
}

/* A group request is done when its own invite and each child have answered. */
static void settle_group(uint32_t head_index)
{
    LiveRequestState *head = slot_at(head_index);
    if (head == NULL || head->id == 0u || head->sub_count == 0u) return;
    uint32_t failed = 0u, hresult = 0u;
    for (uint32_t i = 0u; i < head->sub_count; ++i) {
        const uint32_t id = head->subs[i];
        if (id == head->id) {
            if (!head->own_done) return;
            if (head->own_hresult != 0u) {
                ++failed;
                hresult = head->own_hresult;
            }
            continue;
        }
        LiveRequestState *child = find_request(id);
        if (child == NULL || child->status == RECOMP_LIVE_PENDING) return;
        if (child->status == RECOMP_LIVE_FAILED) {
            ++failed;
            if (hresult == 0u) hresult = child->hresult;
        }
    }
    if (failed == 0u) {
        finish_request(head, 0u);
        return;
    }
    finish_request(head, hresult != 0u ? hresult : LIVE_HRESULT_FAIL);
}

static bool mark_row(LiveRequestState *slot, uint32_t index)
{
    if (index >= RECOMP_LIVE_MAX_LEADER_ROWS) return false;
    const uint8_t bit = (uint8_t)(1u << (index & 7u));
    if (slot->seen[index >> 3] & bit) return false;
    slot->seen[index >> 3] |= bit;
    ++slot->count;
    return true;
}

static void remember_invite(uint64_t xuid, const uint8_t kid[8])
{
    for (uint32_t i = 0u; i < live.invite_count; ++i) {
        if (live.invites[i].xuid != xuid) continue;
        memcpy(live.invites[i].kid, kid, 8u);
        return;
    }
    if (live.invite_count == LIVE_REMEMBERED_INVITES) {
        memmove(&live.invites[0], &live.invites[1], sizeof live.invites[0] * (LIVE_REMEMBERED_INVITES - 1u));
        --live.invite_count;
    }
    live.invites[live.invite_count].xuid = xuid;
    memcpy(live.invites[live.invite_count].kid, kid, 8u);
    ++live.invite_count;
}

static const uint8_t *remembered_invite(uint64_t xuid)
{
    for (uint32_t i = 0u; i < live.invite_count; ++i)
        if (live.invites[i].xuid == xuid) return live.invites[i].kid;
    return NULL;
}

static size_t tag_length(const char *tag)
{
    size_t length = 0u;
    while (tag[length] != '\0' && length < RECOMP_LIVE_GAMERTAG_BYTES - 1u) ++length;
    return length;
}

/* A logon, and every heartbeat, is two requests under one id: CHALLENGE asks
   the matchmaker for a nonce, and LOGON answers it. The slot holds whichever
   is out, so each goes again every 500 ms until its answer comes. A heartbeat
   is the client's own (owned), which the game never polls. */
static uint32_t start_logon(bool owned)
{
    uint8_t packet[LIVE_CHALLENGE_BYTES];
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_LOGON, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_CHALLENGE, id, packet, sizeof packet, sizeof packet) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    put64be(at, live.identity.xuid);
    slot->owned = owned;
    if (!send_request(slot, packet, sizeof packet)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

/* The LOGON that answers a NONCE. An XUID the matchmaker has no account for
   registers with its proof, the one LOGON that carries it. Any other carries
   an HMAC, under the account's key, of the nonce, the kind and every field but
   the answer itself, which nobody can use for another logon. */
static bool build_logon(uint8_t packet[LIVE_LOGON_BYTES], uint32_t id, bool registered,
    const uint8_t nonce[LIVE_NONCE_BYTES])
{
    enum { ANSWERED = LIVE_LOGON_ANSWER + RECOMP_LIVE_HMAC_BYTES }; /* the fields after the answer */
    uint8_t message[LIVE_NONCE_BYTES + 1u + LIVE_LOGON_BYTES - MATCH_ASKED - RECOMP_LIVE_HMAC_BYTES];
    uint8_t *at = begin_request(LIVE_LOGON, id, packet, LIVE_LOGON_BYTES, LIVE_LOGON_BYTES);
    if (at == NULL) return false;
    const size_t length = tag_length(live.user.gamertag);
    put64be(at, live.identity.xuid);
    memcpy(at + 40u, live.digest, sizeof live.digest);
    put32be(at + 104u, LIVE_TITLE);
    put32be(at + 108u, live.presence);
    memcpy(at + 112u, live.presence_kid, 8u);
    at[120] = (uint8_t)length;
    memcpy(at + 121u, live.user.gamertag, length);
    if (!registered) {
        memcpy(packet + LIVE_LOGON_ANSWER, live.identity.proof, RECOMP_LIVE_IDENTITY_PROOF_BYTES);
        return true;
    }
    memcpy(message, nonce, LIVE_NONCE_BYTES);
    message[LIVE_NONCE_BYTES] = LIVE_LOGON;
    memcpy(message + LIVE_NONCE_BYTES + 1u, packet + MATCH_ASKED, LIVE_LOGON_ANSWER - MATCH_ASKED);
    memcpy(message + LIVE_NONCE_BYTES + 1u + LIVE_LOGON_ANSWER - MATCH_ASKED, packet + ANSWERED,
        LIVE_LOGON_BYTES - ANSWERED);
    return recomp_live_hmac(live.identity.key, message, sizeof message, packet + LIVE_LOGON_ANSWER);
}

/* A heartbeat is a whole logon, so no LOGON goes out that the matchmaker did
   not just ask for. With no slot free, the next tick tries again. */
static void send_heartbeat(void)
{
    if (start_logon(true) == 0u) return;
    live.heartbeat_ms = live.now_ms;
    live.heartbeat_due = false;
}

static uint32_t build_session_put(uint8_t *packet, size_t capacity, const uint8_t kid[8],
    const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS], const uint8_t *attributes, uint32_t attributes_size)
{
    if (attributes_size > RECOMP_LIVE_ATTRIBUTES_BYTES) return 0u;
    if (attributes_size != 0u && attributes == NULL) return 0u;
    const size_t size = LIVE_SESSION_PUT_BYTES + attributes_size;
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_SESSION_PUT, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_SESSION_PUT, id, packet, capacity, size) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    memcpy(at, kid, 8u);
    for (unsigned i = 0u; i < RECOMP_LIVE_SESSION_SLOTS; ++i) put32be(at + 8u + 4u * i, slots[i]);
    put16be(at + 24u, (uint16_t)attributes_size);
    if (attributes_size != 0u) memcpy(at + 26u, attributes, attributes_size);
    if (!send_request(slot, packet, size)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_session_del(uint8_t *packet, size_t capacity, const uint8_t kid[8])
{
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_SESSION_DEL, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_SESSION_DEL, id, packet, capacity, LIVE_SESSION_DEL_BYTES) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    memcpy(at, kid, 8u);
    if (!send_request(slot, packet, LIVE_SESSION_DEL_BYTES)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_search(uint8_t *packet, size_t capacity, uint32_t procedure, const uint8_t *filters,
    uint32_t filters_size)
{
    if (filters_size > RECOMP_LIVE_PACKET_BYTES - LIVE_SEARCH_BYTES) return 0u;
    const size_t size = LIVE_SEARCH_BYTES + filters_size;
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_SEARCH, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_SEARCH, id, packet, capacity, size) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    put32be(at, procedure);
    put16be(at + 4u, (uint16_t)filters_size);
    if (filters_size != 0u) memcpy(at + 6u, filters, filters_size);
    if (!send_request(slot, packet, size)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_find(uint8_t *packet, size_t capacity, const uint8_t kid[8])
{
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_FIND, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_FIND, id, packet, capacity, LIVE_FIND_BYTES) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    memcpy(at, kid, 8u);
    if (!send_request(slot, packet, LIVE_FIND_BYTES)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_query(uint8_t kind, uint8_t *packet, size_t capacity)
{
    uint32_t id;
    LiveRequestState *slot = new_request(kind, &id);
    uint8_t *at = slot != NULL ? begin_request(kind, id, packet, capacity, LIVE_QUERY_BYTES) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    if (!send_request(slot, packet, LIVE_QUERY_BYTES)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_friend_op(uint8_t *packet, size_t capacity, uint8_t op, uint64_t xuid, uint8_t answer,
    const uint8_t kid[8], uint32_t head)
{
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_FRIEND_OP, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_FRIEND_OP, id, packet, capacity, LIVE_FRIEND_OP_BYTES) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    slot->op = op;
    slot->xuid = xuid;
    slot->answer = answer;
    memcpy(slot->kid, kid, 8u);
    slot->head = head;
    at[0] = op;
    put64be(at + 1u, xuid);
    at[9] = answer;
    memcpy(at + 10u, kid, 8u);
    if (!send_request(slot, packet, LIVE_FRIEND_OP_BYTES)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_feedback(uint8_t *packet, size_t capacity, uint64_t xuid, uint32_t type)
{
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_FEEDBACK, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_FEEDBACK, id, packet, capacity, LIVE_FEEDBACK_BYTES) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    put64be(at, xuid);
    put32be(at + 8u, type);
    if (!send_request(slot, packet, LIVE_FEEDBACK_BYTES)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_stat_write(uint8_t *packet, size_t capacity, uint32_t board, uint64_t xuid,
    const RecompLiveStat *stats, uint32_t count)
{
    const size_t size = LIVE_STAT_WRITE_BYTES + count * LIVE_STAT_ITEM_BYTES;
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_STAT_WRITE, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_STAT_WRITE, id, packet, capacity, size) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    put32be(at, board);
    put64be(at + 4u, xuid);
    at[12] = (uint8_t)count;
    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t *item = at + 13u + i * LIVE_STAT_ITEM_BYTES;
        put16be(item, stats[i].id);
        item[2] = stats[i].type;
        put64be(item + 3u, (uint64_t)stats[i].value);
    }
    if (!send_request(slot, packet, size)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_stat_read(uint8_t *packet, size_t capacity, uint32_t board, const uint64_t *xuids,
    uint32_t count)
{
    const size_t size = LIVE_STAT_READ_BYTES + count * 8u;
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_STAT_READ, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_STAT_READ, id, packet, capacity, size) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    put32be(at, board);
    at[4] = (uint8_t)count;
    for (uint32_t i = 0u; i < count; ++i) put64be(at + 5u + i * 8u, xuids[i]);
    if (!send_request(slot, packet, size)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

static uint32_t build_leaders(uint8_t *packet, size_t capacity, uint32_t board, uint64_t pivot, uint32_t first,
    uint32_t count)
{
    uint32_t id;
    LiveRequestState *slot = new_request(LIVE_LEADERS, &id);
    uint8_t *at = slot != NULL ? begin_request(LIVE_LEADERS, id, packet, capacity, LIVE_LEADERS_BYTES) : NULL;
    if (at == NULL) {
        if (slot != NULL) slot->id = 0u;
        return 0u;
    }
    put32be(at, board);
    put64be(at + 4u, pivot);
    put32be(at + 12u, first);
    at[16] = (uint8_t)count;
    if (!send_request(slot, packet, LIVE_LEADERS_BYTES)) {
        slot->id = 0u;
        return 0u;
    }
    return id;
}

/* A request the client refuses before sending: its poll says why. */
static uint32_t refused_request(uint8_t kind, uint32_t hresult)
{
    uint32_t id;
    LiveRequestState *slot = new_request(kind, &id);
    if (slot != NULL) finish_request(slot, hresult);
    return id;
}

/* --- answers --- */

static void sign_out(void);

/* The matchmaker's nonce for a logon: its LOGON takes the CHALLENGE's place in
   the slot, inside the same 5 s. */
static void handle_nonce(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    uint8_t packet[LIVE_LOGON_BYTES];
    if (size < LIVE_CHALLENGED_BYTES || get32be(body) != slot->id) return;
    /* A NONCE said again, or one for a logon that has ended: its LOGON is out or not wanted. */
    if (slot->kind != LIVE_LOGON || slot->status != RECOMP_LIVE_PENDING || slot->packet[5] != LIVE_CHALLENGE) return;
    if (slot->owned && !live.logged_on) { /* a heartbeat the player's logoff overtook */
        finish_request(slot, LIVE_HRESULT_FAIL);
        return;
    }
    const bool registered = body[4] != 0u;
    if (!registered && (live.server_known || recomp_live_identity_knows_server(&live.identity, live.server))) {
        /* This account registered with this matchmaker before, so the proof
           stays here: a NONCE forged from the server's address says "no
           account" to be sent it. The logon ends as a refused one does, and a
           heartbeat signs the account out as its refusal would. */
        fprintf(stderr, "recomp live: %s says this account does not exist, but it has logged on there before "
            "(or %s cannot be read). Its proof was withheld to protect the account. If that server lost its "
            "accounts, remove the line \"%s\" from that file to register again\n",
            live.server, live.identity.servers, live.server);
        if (slot->owned) sign_out();
        finish_request(slot, LIVE_HRESULT_ACCESSDENIED);
        return;
    }
    if (!build_logon(packet, slot->id, registered, body + 5u)) {
        finish_request(slot, LIVE_HRESULT_FAIL);
        return;
    }
    memcpy(slot->packet, packet, sizeof packet);
    slot->size = LIVE_LOGON_BYTES;
    slot->last_ms = live.now_ms;
    send_packet(slot->packet, slot->size);
}

static void handle_heartbeat(const uint8_t *body);

static void handle_logged(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_LOGGED_BYTES || get32be(body) != slot->id) return;
    const uint32_t hresult = get32be(body + 4u);
    if (slot->owned) {
        handle_heartbeat(body);
        finish_request(slot, hresult);
        return;
    }
    slot->virtual_ina = get32be(body + 8u);
    if (body[12] > 15u) return;
    copy_tag(slot->gamertag, body + 13u);
    if (hresult == 0u) {
        live.virtual_ina = slot->virtual_ina;
        live.logged_on = true;
        live.heartbeat_ms = live.now_ms;
        live.heartbeat_due = false;
        if (slot->gamertag[0] != '\0') {
            memcpy(live.user.gamertag, slot->gamertag, RECOMP_LIVE_GAMERTAG_BYTES);
            fill_address(&live.user);
        }
        recomp_live_transport_set_local(live.virtual_ina);
        /* From here on this matchmaker has the account, and is never sent the proof again. */
        live.server_known = true;
        if (!recomp_live_identity_remember_server(&live.identity, live.server))
            fprintf(stderr, "recomp live: cannot write %s, so %s is remembered only until the game closes: after "
                "that an answer forged as its own can still ask for this account's proof\n",
                live.identity.servers, live.server);
    }
    finish_request(slot, hresult);
}

static void handle_session_ok(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_SESSION_OK_BYTES || get32be(body) != slot->id) return;
    const uint32_t hresult = get32be(body + 4u);
    if (hresult == 0u) {
        memcpy(slot->session.kid, body + 8u, 8u);
        memcpy(slot->session.key, body + 16u, 16u);
    }
    finish_request(slot, hresult);
}

static void handle_result(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_RESULT_BYTES || get32be(body) != slot->id) return;
    const uint32_t hresult = get32be(body + 4u);
    const uint32_t index = body[8], total = body[9];
    slot->total = total;
    if (hresult != 0u || total == 0u) {
        finish_request(slot, hresult);
        return;
    }
    if (size < LIVE_RESULT_ROW_BYTES || index >= RECOMP_LIVE_MAX_RESULTS) return;
    const uint8_t *row = body + 10u;
    const uint32_t attributes_size = get16be(row + 68u);
    if (attributes_size > RECOMP_LIVE_ATTRIBUTES_BYTES || size < LIVE_RESULT_ROW_BYTES + attributes_size) return;
    RecompLiveSession *session = &slot->answer_at.sessions[index];
    memset(session, 0, sizeof *session);
    memcpy(session->id.kid, row, 8u);
    memcpy(session->id.key, row + 8u, 16u);
    session->host.xuid = get64be(row + 24u);
    copy_tag(session->host.gamertag, row + 32u);
    session->host.address.virtual_ina = get32be(row + 48u);
    session->host.address.port = LIVE_PORT;
    for (unsigned i = 0u; i < RECOMP_LIVE_SESSION_SLOTS; ++i) session->slots[i] = get32be(row + 52u + 4u * i);
    session->attributes_size = attributes_size;
    memcpy(session->attributes, row + 70u, attributes_size);
    if (mark_row(slot, index) && slot->count == slot->total) finish_request(slot, 0u);
}

static void refresh_friend_list(const LiveRequestState *slot)
{
    live.friend_count = slot->count < RECOMP_LIVE_MAX_FRIENDS ? slot->count : RECOMP_LIVE_MAX_FRIENDS;
    for (uint32_t i = 0u; i < live.friend_count; ++i) live.friends[i] = slot->answer_at.friends[i];
}

static void handle_friend(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_FRIEND_BYTES || get32be(body) != slot->id) return;
    const uint32_t index = body[4], total = body[5];
    slot->total = total;
    if (total == 0u) {
        live.friend_count = 0u;
        finish_request(slot, 0u);
        return;
    }
    if (index >= RECOMP_LIVE_MAX_FRIENDS) return;
    RecompLiveFriend *friend_row = &slot->answer_at.friends[index];
    memset(friend_row, 0, sizeof *friend_row);
    friend_row->xuid = get64be(body + 6u);
    copy_tag(friend_row->gamertag, body + 14u);
    friend_row->flags = get32be(body + 30u);
    friend_row->title_id = get32be(body + 34u);
    memcpy(friend_row->session_kid, body + 38u, 8u);
    if ((friend_row->flags & RECOMP_LIVE_FRIEND_RECEIVED_INVITE) != 0u)
        remember_invite(friend_row->xuid, friend_row->session_kid);
    if (mark_row(slot, index) && slot->count == slot->total) {
        refresh_friend_list(slot);
        finish_request(slot, 0u);
    }
}

static void handle_done(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_DONE_BYTES || get32be(body) != slot->id) return;
    const uint32_t hresult = get32be(body + 4u);
    if (slot->sub_count != 0u) { /* a group's own invite: its children decide the rest */
        slot->own_done = true;
        slot->own_hresult = hresult;
        settle_group(slot_index(slot));
        return;
    }
    if (hresult == 0u && slot->kind == LIVE_FRIEND_OP && slot->op == 6u && slot->answer == 1u) {
        /* The invite this player accepted, which the game asks for once outside the room. */
        memset(&live.accepted, 0, sizeof live.accepted);
        live.accepted.xuid = slot->xuid;
        live.accepted.flags = RECOMP_LIVE_FRIEND_INVITE_ACCEPTED;
        memcpy(live.accepted.session_kid, slot->kid, 8u);
        for (uint32_t i = 0u; i < live.friend_count; ++i)
            if (live.friends[i].xuid == slot->xuid) live.accepted = live.friends[i];
        live.accepted.flags |= RECOMP_LIVE_FRIEND_INVITE_ACCEPTED;
        memcpy(live.accepted.session_kid, slot->kid, 8u);
        live.has_accepted = true;
    }
    finish_request(slot, hresult);
    if (slot->head != 0u) settle_group(slot->head - 1u); /* a group child answered */
}

static void handle_mutelist(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_MUTELIST_BYTES || get32be(body) != slot->id) return;
    const uint32_t hresult = get32be(body + 4u);
    const uint32_t count = body[8];
    if (hresult != 0u || size < LIVE_MUTELIST_BYTES + count * 8u) {
        finish_request(slot, hresult != 0u ? hresult : LIVE_HRESULT_FAIL);
        return;
    }
    live.mute_count = count < RECOMP_LIVE_MAX_MUTES ? count : RECOMP_LIVE_MAX_MUTES;
    for (uint32_t i = 0u; i < live.mute_count; ++i) live.mutes[i] = get64be(body + 9u + i * 8u);
    finish_request(slot, 0u);
}

static void handle_statrow(LiveRequestState *slot, const uint8_t *body, size_t size)
{
    if (size < LIVE_STATROW_BYTES || get32be(body) != slot->id) return;
    const uint32_t hresult = get32be(body + 4u);
    const uint32_t index = body[8], total = body[9];
    slot->total = total;
    slot->board_total = get32be(body + 10u);
    if (hresult != 0u || total == 0u) {
        finish_request(slot, hresult);
        return;
    }
    const uint32_t count = body[42];
    if (index >= RECOMP_LIVE_MAX_LEADER_ROWS || size < LIVE_STATROW_BYTES + count * LIVE_STAT_BYTES) return;
    RecompLiveLeaderRow *row = &slot->answer_at.rows[index];
    memset(row, 0, sizeof *row);
    row->xuid = get64be(body + 14u);
    copy_tag(row->gamertag, body + 22u);
    row->rank = get32be(body + 38u);
    row->stat_count = count;
    if (count > RECOMP_LIVE_MAX_STAT_ATTRS) {
        /* The server holds at most 64 a row, RECOMP_LIVE_MAX_STAT_ATTRS. */
        fprintf(stderr, "recomp live: STATROW has %u stats, keeping %u\n", count,
            (unsigned)RECOMP_LIVE_MAX_STAT_ATTRS);
        row->stat_count = RECOMP_LIVE_MAX_STAT_ATTRS;
    }
    for (uint32_t i = 0u; i < row->stat_count; ++i) {
        const uint8_t *stat = body + 43u + i * LIVE_STAT_BYTES;
        const uint8_t type = stat[2];
        row->stats[i].id = get16be(stat);
        /* Only the library's numeric types: a string stat would carry a
           pointer that is meaningless across hosts, so zero it instead. */
        if (type >= 1u && type <= 3u) {
            row->stats[i].type = type;
            row->stats[i].value = (int64_t)get64be(stat + 3u);
        } else {
            row->stats[i].type = 0u;
            row->stats[i].value = 0;
        }
    }
    if (mark_row(slot, index) && slot->count == slot->total) finish_request(slot, 0u);
}

static void handle_push(const uint8_t *body, size_t size)
{
    if (size < LIVE_PUSH_BYTES) return;
    const uint8_t type = body[0];
    const uint64_t xuid = get64be(body + 1u);
    if (type == 2u) remember_invite(xuid, body + 9u); /* an invite arrived: keep its kid to answer */
    if (live.friends_asking) return; /* this push's re-enumeration is already on its way */
    uint8_t packet[LIVE_QUERY_BYTES];
    const uint32_t id = build_query(LIVE_FRIENDS, packet, sizeof packet);
    live.friends_asking = id != 0u;
    LiveRequestState *slot = find_request(id);
    if (slot != NULL) slot->owned = true;
}

static void handle_lpeer(const uint8_t *body, size_t size)
{
    if (size < LIVE_LPEER_BYTES) return;
    RecompLivePeerIntro intro;
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = get32be(body);
    memcpy(&intro.public_address, body + 4u, 4u);
    memcpy(&intro.public_port, body + 8u, 2u);
    memcpy(&intro.lan_address, body + 10u, 4u);
    memcpy(&intro.lan_port, body + 14u, 2u);
    memcpy(intro.token, body + 16u, 8u);
    intro.relay = body[24] != 0u;
    if (intro.virtual_ina == 0u || intro.virtual_ina == live.virtual_ina) return;
    recomp_live_transport_peer(&intro);
    /* The joiner's LPEER reaches the host, which did not ask for it: one
       CONNECT back makes the pairing two-sided and grants each side the
       other's stats. So does a new pairing with a known player, a rematch
       after the old one lapsed. This side's own ask, or the same pairing
       again, is left alone, so LPEERs never start a ping-pong of CONNECTs. */
    LiveConnect *ask = connect_find(intro.virtual_ina);
    if (ask == NULL || (!ask->repeating && memcmp(ask->token, intro.token, sizeof ask->token) != 0)) {
        ask_connect(intro.virtual_ina);
        ask = connect_find(intro.virtual_ina);
    }
    if (ask != NULL) memcpy(ask->token, intro.token, sizeof ask->token);
}

/* Drops the account and every route it introduced, as a logoff does. */
static void sign_out(void)
{
    live.logged_on = false;
    live.virtual_ina = 0u;
    fill_address(&live.user);
    recomp_live_transport_set_local(0u);
    /* No peer or route outlives the account that introduced it. */
    recomp_live_transport_forget(0u);
    memset(live.connects, 0, sizeof live.connects);
}

/* A heartbeat's LOGGED: a new virtual address is adopted here and the
   transport told. One that a logoff overtook changes nothing. */
static void handle_heartbeat(const uint8_t *body)
{
    if (!live.logged_on) return;
    const uint32_t hresult = get32be(body + 4u);
    if (hresult != 0u) {
        /* The matchmaker no longer accepts this account, as a logon it refused
           would have said: the game's Live pages report no account again. */
        fprintf(stderr, "recomp live: the matchmaker ended this account's session (0x%08x)\n",
            (unsigned)hresult);
        sign_out();
        return;
    }
    const uint32_t virtual_ina = get32be(body + 8u);
    if (virtual_ina == 0u || virtual_ina == live.virtual_ina) return;
    live.virtual_ina = virtual_ina;
    fill_address(&live.user);
    recomp_live_transport_set_local(virtual_ina);
    fprintf(stderr, "recomp live: the matchmaker moved this player to a new address\n");
}

static void handle_answer(uint8_t kind, const uint8_t *body, size_t size)
{
    if (kind == LIVE_PUSH) {
        handle_push(body, size);
        return;
    }
    if (kind == LIVE_LPEER) {
        handle_lpeer(body, size);
        return;
    }
    if (size < 4u) return;
    LiveRequestState *slot = find_request(get32be(body));
    if (slot == NULL) return; /* a late answer for a request the game already dropped */
    switch (kind) {
    case LIVE_NONCE: handle_nonce(slot, body, size); break;
    case LIVE_LOGGED: handle_logged(slot, body, size); break;
    case LIVE_SESSION_OK: handle_session_ok(slot, body, size); break;
    case LIVE_RESULT: handle_result(slot, body, size); break;
    case LIVE_FRIEND: handle_friend(slot, body, size); break;
    case LIVE_DONE: handle_done(slot, body, size); break;
    case LIVE_MUTELIST: handle_mutelist(slot, body, size); break;
    case LIVE_STATROW: handle_statrow(slot, body, size); break;
    default: break;
    }
    drop_owned(slot);
}

/* Every matchmaker packet the transport hands over. It has already dropped a
   packet without this socket's nonce, and has answered the server's COOKIE
   itself, so only the Live kinds are left. */
static void on_server_packet(const uint8_t *packet, size_t size)
{
    if (!live.attached || packet == NULL || size < MATCH_BODY) return;
    if (memcmp(packet, MATCH_MAGIC, sizeof MATCH_MAGIC) != 0 || packet[4] != MATCH_VERSION) return;
    handle_answer(packet[5], packet + MATCH_BODY, size - MATCH_BODY);
}

/* --- the backend --- */

static bool client_local_user(void *self, RecompLiveUser *user)
{
    (void)self;
    if (user == NULL) return false;
    fill_address(&live.user);
    *user = live.user;
    return true;
}

static RecompLiveRequest client_logon(void *self)
{
    (void)self;
    return start_logon(false);
}

static void client_logoff(void *self)
{
    (void)self;
    sign_out();
}

static RecompLiveRequest client_session_create(void *self, const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS],
    const uint8_t *attributes, uint32_t attributes_size)
{
    static const uint8_t zero_kid[8] = {0u};
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    (void)self;
    return build_session_put(packet, sizeof packet, zero_kid, slots, attributes, attributes_size);
}

static RecompLiveRequest client_session_update(void *self, const uint8_t kid[8],
    const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS], const uint8_t *attributes, uint32_t attributes_size)
{
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    (void)self;
    if (kid == NULL) return refused_request(LIVE_SESSION_PUT, LIVE_HRESULT_INVALIDARG);
    return build_session_put(packet, sizeof packet, kid, slots, attributes, attributes_size);
}

static RecompLiveRequest client_session_delete(void *self, const uint8_t kid[8])
{
    uint8_t packet[LIVE_SESSION_DEL_BYTES];
    (void)self;
    if (kid == NULL) return refused_request(LIVE_SESSION_DEL, LIVE_HRESULT_INVALIDARG);
    return build_session_del(packet, sizeof packet, kid);
}

static bool client_session_id(void *self, RecompLiveRequest request, RecompLiveSessionId *id)
{
    LiveRequestState *slot = find_request(request);
    (void)self;
    if (slot == NULL || id == NULL || slot->status != RECOMP_LIVE_DONE || slot->kind != LIVE_SESSION_PUT) return false;
    *id = slot->session;
    return true;
}

static RecompLiveRequest client_search(void *self, uint32_t procedure, const uint8_t *filters,
    uint32_t filters_size)
{
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    (void)self;
    if (filters == NULL && filters_size != 0u) return refused_request(LIVE_SEARCH, LIVE_HRESULT_INVALIDARG);
    if (filters_size > RECOMP_LIVE_PACKET_BYTES - LIVE_SEARCH_BYTES)
        return refused_request(LIVE_SEARCH, LIVE_HRESULT_INVALIDARG);
    return build_search(packet, sizeof packet, procedure, filters, filters_size);
}

static RecompLiveRequest client_find(void *self, const uint8_t kid[8])
{
    uint8_t packet[LIVE_FIND_BYTES];
    (void)self;
    if (kid == NULL) return refused_request(LIVE_FIND, LIVE_HRESULT_INVALIDARG);
    return build_find(packet, sizeof packet, kid);
}

static uint32_t client_results(void *self, RecompLiveRequest request, RecompLiveSession *out, uint32_t capacity)
{
    LiveRequestState *slot = find_request(request);
    (void)self;
    if (slot == NULL || slot->status != RECOMP_LIVE_DONE || out == NULL) return 0u;
    uint32_t count = slot->count < capacity ? slot->count : capacity;
    if (count > RECOMP_LIVE_MAX_RESULTS) count = RECOMP_LIVE_MAX_RESULTS;
    if (count != 0u) memcpy(out, slot->answer_at.sessions, count * sizeof *out);
    return count;
}

/* The first datagram to a player asks the matchmaker to pair this socket with
   it; the ask repeats until its LPEER arrives. */
static void client_connect(void *self, uint32_t virtual_ina)
{
    (void)self;
    ask_connect(virtual_ina);
}

static RecompLiveRequest client_friends_enumerate(void *self)
{
    uint8_t packet[LIVE_QUERY_BYTES];
    (void)self;
    const uint32_t id = build_query(LIVE_FRIENDS, packet, sizeof packet);
    live.friends_asking = id != 0u;
    return id;
}

static uint32_t client_friends(void *self, RecompLiveFriend *out, uint32_t capacity)
{
    (void)self;
    if (out == NULL) return 0u;
    const uint32_t count = live.friend_count < capacity ? live.friend_count : capacity;
    if (count != 0u) memcpy(out, live.friends, count * sizeof *out);
    return count;
}

static RecompLiveRequest client_friend_op(void *self, uint8_t op, uint64_t xuid, uint8_t answer, const uint8_t *kid)
{
    static const uint8_t zero_kid[8] = {0u};
    uint8_t packet[LIVE_FRIEND_OP_BYTES];
    (void)self;
    return build_friend_op(packet, sizeof packet, op, xuid, answer, kid != NULL ? kid : zero_kid, 0u);
}

static RecompLiveRequest client_friend_request(void *self, uint64_t xuid)
{
    return client_friend_op(self, 1u, xuid, 0u, NULL);
}

static RecompLiveRequest client_friend_answer(void *self, uint64_t xuid, RecompLiveAnswer answer)
{
    if (answer > RECOMP_LIVE_NEVER) return refused_request(LIVE_FRIEND_OP, LIVE_HRESULT_INVALIDARG);
    return client_friend_op(self, 2u, xuid, (uint8_t)answer, NULL);
}

static RecompLiveRequest client_friend_remove(void *self, uint64_t xuid)
{
    return client_friend_op(self, 3u, xuid, 0u, NULL);
}

/* One FRIEND_OP per friend; the head request is done when each one answered. */
static RecompLiveRequest client_invite_group(void *self, uint8_t op, const uint8_t kid[8], const uint64_t *xuids,
    uint32_t count)
{
    uint8_t packet[LIVE_GROUP_BYTES];
    (void)self;
    if (kid == NULL || xuids == NULL || count == 0u)
        return refused_request(LIVE_FRIEND_OP, LIVE_HRESULT_INVALIDARG);
    if (count > LIVE_GROUP_CHILDREN) return refused_request(LIVE_FRIEND_OP, LIVE_HRESULT_INVALIDARG);
    /* The invite is all or nothing: with a slot short, the friends that fit
       would be invited while the game is told the whole group went. */
    uint32_t free_slots = 0u;
    for (uint32_t i = 0u; i < LIVE_REQUEST_SLOTS; ++i)
        if (live.requests[i].id == 0u) ++free_slots;
    if (free_slots < count) return 0u;
    LiveRequestState *head = NULL;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t id = build_friend_op(packet, sizeof packet, op, xuids[i], 0u, kid, 0u);
        LiveRequestState *slot = find_request(id);
        if (slot == NULL) break;
        if (head == NULL) {
            head = slot;
            head->subs[head->sub_count++] = head->id; /* its own answer counts too */
            continue;
        }
        slot->head = slot_index(head) + 1u;
        head->subs[head->sub_count++] = slot->id;
    }
    return head != NULL ? head->id : 0u;
}

static RecompLiveRequest client_game_invite(void *self, const uint8_t kid[8], const uint64_t *xuids, uint32_t count)
{
    return client_invite_group(self, 4u, kid, xuids, count);
}

static RecompLiveRequest client_revoke_invite(void *self, const uint8_t kid[8], const uint64_t *xuids,
    uint32_t count)
{
    return client_invite_group(self, 5u, kid, xuids, count);
}

static RecompLiveRequest client_answer_invite(void *self, uint64_t xuid, RecompLiveAnswer answer)
{
    static const uint8_t zero_kid[8] = {0u};
    if (answer > RECOMP_LIVE_NEVER) return refused_request(LIVE_FRIEND_OP, LIVE_HRESULT_INVALIDARG);
    const uint8_t *kid = remembered_invite(xuid);
    return client_friend_op(self, 6u, xuid, (uint8_t)answer, kid != NULL ? kid : zero_kid);
}

static bool client_accepted_invite(void *self, RecompLiveFriend *from)
{
    (void)self;
    if (from == NULL || !live.has_accepted) return false;
    *from = live.accepted;
    return true;
}

static void client_presence(void *self, uint32_t state, const uint8_t kid[8])
{
    (void)self;
    live.presence = state;
    if (kid != NULL) memcpy(live.presence_kid, kid, 8u);
    else memset(live.presence_kid, 0, 8u);
    live.heartbeat_due = true;
}

static RecompLiveRequest client_mutelist(void *self)
{
    uint8_t packet[LIVE_QUERY_BYTES];
    (void)self;
    return build_query(LIVE_MUTES, packet, sizeof packet);
}

static uint32_t client_mutes(void *self, uint64_t *out, uint32_t capacity)
{
    (void)self;
    if (out == NULL) return 0u;
    const uint32_t count = live.mute_count < capacity ? live.mute_count : capacity;
    if (count != 0u) memcpy(out, live.mutes, count * sizeof *out);
    return count;
}

static RecompLiveRequest client_feedback(void *self, uint64_t xuid, uint32_t type)
{
    uint8_t packet[LIVE_FEEDBACK_BYTES];
    (void)self;
    if (xuid == 0u) return refused_request(LIVE_FEEDBACK, LIVE_HRESULT_INVALIDARG);
    return build_feedback(packet, sizeof packet, xuid, type);
}

static RecompLiveRequest client_stat_write(void *self, uint32_t board, uint64_t xuid, const RecompLiveStat *stats,
    uint32_t count)
{
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    (void)self;
    if (count > RECOMP_LIVE_MAX_STAT_ATTRS || (stats == NULL && count != 0u))
        return refused_request(LIVE_STAT_WRITE, LIVE_HRESULT_INVALIDARG);
    return build_stat_write(packet, sizeof packet, board, xuid, stats, count);
}

static RecompLiveRequest client_stat_read(void *self, uint32_t board, const uint64_t *xuids, uint32_t count)
{
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    (void)self;
    if (count > RECOMP_LIVE_MAX_LEADER_ROWS || (xuids == NULL && count != 0u))
        return refused_request(LIVE_STAT_READ, LIVE_HRESULT_INVALIDARG);
    return build_stat_read(packet, sizeof packet, board, xuids, count);
}

static uint32_t client_stat_results(void *self, RecompLiveRequest request, RecompLiveLeaderRow *out,
    uint32_t capacity)
{
    LiveRequestState *slot = find_request(request);
    (void)self;
    if (slot == NULL || slot->status != RECOMP_LIVE_DONE || out == NULL) return 0u;
    uint32_t count = slot->count < capacity ? slot->count : capacity;
    if (count > RECOMP_LIVE_MAX_LEADER_ROWS) count = RECOMP_LIVE_MAX_LEADER_ROWS;
    if (count != 0u) memcpy(out, slot->answer_at.rows, count * sizeof *out);
    return count;
}

static RecompLiveRequest client_leader_enumerate(void *self, uint32_t board, uint64_t pivot_xuid,
    uint32_t first_rank, uint32_t count)
{
    uint8_t packet[LIVE_LEADERS_BYTES];
    (void)self;
    if (count > RECOMP_LIVE_MAX_LEADER_ROWS) count = RECOMP_LIVE_MAX_LEADER_ROWS;
    return build_leaders(packet, sizeof packet, board, pivot_xuid, first_rank, count);
}

static uint32_t client_leader_total(void *self, RecompLiveRequest request)
{
    LiveRequestState *slot = find_request(request);
    (void)self;
    return slot != NULL ? slot->board_total : 0u;
}

static RecompLiveStatus client_poll(void *self, RecompLiveRequest request, uint32_t *hresult)
{
    LiveRequestState *slot = find_request(request);
    (void)self;
    if (slot == NULL) {
        if (hresult != NULL) *hresult = LIVE_HRESULT_FAIL;
        return RECOMP_LIVE_FAILED;
    }
    if (hresult != NULL) *hresult = slot->hresult;
    return (RecompLiveStatus)slot->status;
}

static void client_release(void *self, RecompLiveRequest request)
{
    LiveRequestState *slot = find_request(request);
    (void)self;
    if (slot == NULL) return;
    for (uint32_t i = 0u; i < LIVE_REQUEST_SLOTS; ++i) {
        LiveRequestState *other = &live.requests[i];
        if (other->head != 0u && other->head == slot_index(slot) + 1u) other->head = 0u;
    }
    /* The guest only holds the head of a group; its children are the client's
       to free once the head is released. */
    if (slot->sub_count != 0u) {
        for (uint32_t i = 0u; i < slot->sub_count; ++i) {
            if (slot->subs[i] == slot->id) continue;
            LiveRequestState *child = find_request(slot->subs[i]);
            if (child != NULL) memset(child, 0, sizeof *child);
        }
    }
    if (slot->kind == LIVE_FRIENDS && slot->sub_count == 0u) live.friends_asking = false;
    memset(slot, 0, sizeof *slot);
}

static void client_tick(void *self, uint64_t now_ms)
{
    (void)self;
    recomp_live_client_tick(now_ms);
}

void recomp_live_client_tick(uint64_t now_ms)
{
    if (!live.attached) return;
    live.now_ms = now_ms;
    if (live.logged_on && (live.heartbeat_due || now_ms - live.heartbeat_ms >= LIVE_HEARTBEAT_MS))
        send_heartbeat();
    for (uint32_t i = 0u; i < LIVE_REQUEST_SLOTS; ++i) {
        LiveRequestState *slot = &live.requests[i];
        if (slot->id == 0u || slot->status != RECOMP_LIVE_PENDING) continue;
        if (slot->sent_ms == 0u) slot->sent_ms = slot->last_ms = now_ms; /* before the first tick */
        if (now_ms - slot->sent_ms >= LIVE_TIMEOUT_MS) {
            if (slot->sub_count != 0u) {
                slot->own_done = true;
                slot->own_hresult = LIVE_HRESULT_TIMEOUT;
                settle_group(i);
            } else {
                finish_request(slot, LIVE_HRESULT_TIMEOUT);
            }
            if (slot->head != 0u) settle_group(slot->head - 1u);
            drop_owned(slot);
            continue;
        }
        if (now_ms - slot->last_ms < LIVE_RETRY_MS || slot->size < MATCH_ASKED) continue;
        send_packet(slot->packet, slot->size);
        slot->last_ms = now_ms;
    }
    /* A pairing whose other player is not heard yet: say its CONNECT again,
       with the id the matchmaker cached, so a lost LPEER on either side does
       not hang the join. */
    for (uint32_t i = 0u; i < LIVE_CONNECT_SLOTS; ++i) {
        LiveConnect *ask = &live.connects[i];
        if (ask->virtual_ina == 0u || !ask->repeating) continue;
        if (recomp_live_transport_heard(ask->virtual_ina)) {
            ask->repeating = false;
            continue;
        }
        if (ask->last_ms == 0u) { /* asked before the first tick: start the clock */
            ask->last_ms = now_ms;
            ask->until_ms = now_ms + LIVE_CONNECT_GIVE_UP_MS;
        }
        if (now_ms >= ask->until_ms) {
            ask->repeating = false;
            continue;
        }
        if (now_ms - ask->last_ms < LIVE_CONNECT_MS) continue;
        send_packet(ask->packet, ask->size);
        ask->last_ms = now_ms;
    }
}

bool recomp_live_client_attached(void)
{
    return live.attached;
}

const RecompLiveBackend *recomp_live_client_attach(const RecompLiveIdentity *identity, const char *gamertag,
    const char *digest, const char *server)
{
    if (identity == NULL || identity->xuid == 0u) return NULL;
    /* The server list names a server by this string, so an identity that
       keeps one never logs on without it, or with it cut short: the guard
       against a forged "no account" NONCE would not find the server. */
    const size_t named = server != NULL ? strlen(server) : 0u;
    if (named >= sizeof live.server || (named == 0u && identity->servers[0] != '\0')) {
        fprintf(stderr, "recomp live: the matchmaker's name is %s, so this account's server list cannot "
            "name it\n", named == 0u ? "missing" : "too long");
        return NULL;
    }
    memset(&live, 0, sizeof live);
    live.identity = *identity;
    for (size_t i = 0u; server != NULL && server[i] != '\0' && i + 1u < sizeof live.server; ++i)
        live.server[i] = (char)tolower((unsigned char)server[i]);
    memset(live.digest, '0', sizeof live.digest);
    if (digest != NULL) {
        size_t length = 0u;
        while (digest[length] != '\0' && length < sizeof live.digest) ++length;
        memcpy(live.digest, digest, length);
    }
    live.user.xuid = identity->xuid;
    if (gamertag != NULL) {
        const size_t length = tag_length(gamertag);
        memcpy(live.user.gamertag, gamertag, length);
    }
    fill_address(&live.user);
    live.next_id = 0u;
    recomp_live_transport_set_server_handler(on_server_packet);
    live.backend.self = &live;
    live.backend.local_user = client_local_user;
    live.backend.logon = client_logon;
    live.backend.logoff = client_logoff;
    live.backend.session_create = client_session_create;
    live.backend.session_update = client_session_update;
    live.backend.session_delete = client_session_delete;
    live.backend.session_id = client_session_id;
    live.backend.search = client_search;
    live.backend.find = client_find;
    live.backend.results = client_results;
    live.backend.connect = client_connect;
    live.backend.friends_enumerate = client_friends_enumerate;
    live.backend.friends = client_friends;
    live.backend.friend_request = client_friend_request;
    live.backend.friend_answer = client_friend_answer;
    live.backend.friend_remove = client_friend_remove;
    live.backend.game_invite = client_game_invite;
    live.backend.revoke_invite = client_revoke_invite;
    live.backend.answer_invite = client_answer_invite;
    live.backend.accepted_invite = client_accepted_invite;
    live.backend.presence = client_presence;
    live.backend.mutelist = client_mutelist;
    live.backend.mutes = client_mutes;
    live.backend.feedback = client_feedback;
    live.backend.stat_write = client_stat_write;
    live.backend.stat_read = client_stat_read;
    live.backend.stat_results = client_stat_results;
    live.backend.leader_enumerate = client_leader_enumerate;
    live.backend.leader_total = client_leader_total;
    live.backend.poll = client_poll;
    live.backend.release = client_release;
    live.backend.tick = client_tick;
    live.attached = true;
    return &live.backend;
}

void recomp_live_client_detach(void)
{
    if (!live.attached) return;
    if (live.logged_on) {
        /* A zero session id logs off, so a quitting host leaves search results
           and friends' lists now instead of when its heartbeat lapses. */
        static const uint8_t logoff[8] = {0u};
        uint8_t packet[LIVE_SESSION_DEL_BYTES];
        (void)build_session_del(packet, sizeof packet, logoff);
    }
    recomp_live_transport_set_server_handler(NULL);
    memset(&live, 0, sizeof live);
}
