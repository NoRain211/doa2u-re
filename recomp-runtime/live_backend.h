/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_BACKEND_H
#define DOA2U_RECOMP_LIVE_BACKEND_H

/* The Xbox Live service the game's XOnline calls talk to (see docs/xbox-live.md).
   live_xapi.c translates the guest API to these calls; live_client.c implements
   them against the matchmaker; live_backend_local.c implements them in memory
   for tests. Every call returns at once. A long operation returns a request id
   that live_xapi.c polls once per XOnlineTaskContinue, as the game polls its
   task handles. All strings are UTF-8, all integers host order. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_LIVE_GAMERTAG_BYTES = 16,   /* XONLINE_GAMERTAG_SIZE, NUL included */
    RECOMP_LIVE_SESSION_SLOTS = 4,     /* public filled/open, private filled/open */
    RECOMP_LIVE_ATTRIBUTES_BYTES = 0x2a0, /* XOnlineMatchSearchParse output: search record 0x54..0x2f4 */
    RECOMP_LIVE_MAX_RESULTS = 25,
    RECOMP_LIVE_MAX_FRIENDS = 100,
    RECOMP_LIVE_MAX_MUTES = 250,
    /* The largest RANKINGS group is 62 entries (group 7: ids 0..59, -2, -1);
       a write carries the group minus -1, so 61. */
    RECOMP_LIVE_MAX_STAT_ATTRS = 64,
    RECOMP_LIVE_MAX_LEADER_ROWS = 100,
};

typedef uint32_t RecompLiveRequest; /* 0 is none */

typedef enum RecompLiveStatus {
    RECOMP_LIVE_PENDING = 0,
    RECOMP_LIVE_DONE = 1,
    RECOMP_LIVE_FAILED = -1, /* the request's hresult says why */
} RecompLiveStatus;

/* An XNADDR as the game sees it: the virtual address the matchmaker gave the
   player at logon stands in for both ina and inaOnline. */
typedef struct RecompLiveAddress {
    uint32_t virtual_ina; /* 10.x.y.z, network order, never loopback */
    uint16_t port;        /* the game's socket port, 1000 */
    uint8_t mac[6];       /* derived from the XUID */
} RecompLiveAddress;

typedef struct RecompLiveUser {
    uint64_t xuid;
    char gamertag[RECOMP_LIVE_GAMERTAG_BYTES];
    RecompLiveAddress address;
} RecompLiveUser;

typedef struct RecompLiveSessionId {
    uint8_t kid[8];
    uint8_t key[16];
} RecompLiveSessionId;

typedef struct RecompLiveSession {
    RecompLiveSessionId id;
    RecompLiveUser host;
    uint32_t slots[RECOMP_LIVE_SESSION_SLOTS];
    uint32_t attributes_size;
    uint8_t attributes[RECOMP_LIVE_ATTRIBUTES_BYTES]; /* in XOnlineMatchSearchParse's format */
} RecompLiveSession;

enum { /* RecompLiveFriend.flags, the XONLINE_FRIENDSTATE_FLAG_* bits the game reads */
    RECOMP_LIVE_FRIEND_ONLINE = 0x00000001u,
    RECOMP_LIVE_FRIEND_PLAYING = 0x00000002u,
    RECOMP_LIVE_FRIEND_JOINABLE = 0x00000010u,
    RECOMP_LIVE_FRIEND_SENT_INVITE = 0x04000000u,
    RECOMP_LIVE_FRIEND_RECEIVED_INVITE = 0x08000000u,
    RECOMP_LIVE_FRIEND_INVITE_ACCEPTED = 0x10000000u,
    RECOMP_LIVE_FRIEND_INVITE_REJECTED = 0x20000000u,
    RECOMP_LIVE_FRIEND_SENT_REQUEST = 0x40000000u,
    RECOMP_LIVE_FRIEND_RECEIVED_REQUEST = 0x80000000u,
};

typedef struct RecompLiveFriend {
    uint64_t xuid;
    char gamertag[RECOMP_LIVE_GAMERTAG_BYTES];
    uint32_t flags;
    uint32_t title_id;
    uint8_t session_kid[8]; /* the friend's session, zero when none */
} RecompLiveFriend;

typedef enum RecompLiveAnswer { RECOMP_LIVE_NO = 0, RECOMP_LIVE_YES = 1, RECOMP_LIVE_NEVER = 2 } RecompLiveAnswer;

typedef struct RecompLiveStat {
    uint16_t id;
    uint8_t type; /* 1 = int32, 2 = int64, 3 = double, as XONLINE_STAT */
    int64_t value;
} RecompLiveStat;

typedef struct RecompLiveLeaderRow {
    uint64_t xuid;
    char gamertag[RECOMP_LIVE_GAMERTAG_BYTES];
    uint32_t rank;
    uint32_t stat_count;
    /* Stored stats, the score pseudo id -2 (0xfffe) among them: the game
       writes -2 as an ordinary write-record entry and reads it back by id,
       so it stays a stat here too. Rank (-1) and gamertag (-3) come from the
       row's own fields instead. */
    RecompLiveStat stats[RECOMP_LIVE_MAX_STAT_ATTRS];
} RecompLiveLeaderRow;

typedef struct RecompLiveBackend RecompLiveBackend;

struct RecompLiveBackend {
    void *self;
    /* The signed-in player, filled once at startup from the netplay profile. */
    bool (*local_user)(void *self, RecompLiveUser *user);

    /* Logon: done once the matchmaker accepted the XUID and gave the virtual address. */
    RecompLiveRequest (*logon)(void *self);
    void (*logoff)(void *self);

    /* Sessions. Create fills the id; update and delete name it. */
    RecompLiveRequest (*session_create)(void *self, const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS],
        const uint8_t *attributes, uint32_t attributes_size);
    RecompLiveRequest (*session_update)(void *self, const uint8_t kid[8],
        const uint32_t slots[RECOMP_LIVE_SESSION_SLOTS], const uint8_t *attributes, uint32_t attributes_size);
    RecompLiveRequest (*session_delete)(void *self, const uint8_t kid[8]);
    /* The id of the session a finished create made. */
    bool (*session_id)(void *self, RecompLiveRequest request, RecompLiveSessionId *id);

    /* Search: Quick Match and OptiMatch; find: a session by id (invites, joins). */
    RecompLiveRequest (*search)(void *self, uint32_t procedure, const uint8_t *filters, uint32_t filters_size);
    RecompLiveRequest (*find)(void *self, const uint8_t kid[8]);
    uint32_t (*results)(void *self, RecompLiveRequest request, RecompLiveSession *out, uint32_t capacity);

    /* Ask the matchmaker to pair this player with another for punching or relay.
       live_transport.c calls it on the first datagram to an address it has no route for. */
    void (*connect)(void *self, uint32_t virtual_ina);

    /* Friends and presence. */
    RecompLiveRequest (*friends_enumerate)(void *self);
    uint32_t (*friends)(void *self, RecompLiveFriend *out, uint32_t capacity); /* the latest list */
    RecompLiveRequest (*friend_request)(void *self, uint64_t xuid);
    RecompLiveRequest (*friend_answer)(void *self, uint64_t xuid, RecompLiveAnswer answer);
    RecompLiveRequest (*friend_remove)(void *self, uint64_t xuid);
    RecompLiveRequest (*game_invite)(void *self, const uint8_t kid[8], const uint64_t *xuids, uint32_t count);
    RecompLiveRequest (*revoke_invite)(void *self, const uint8_t kid[8], const uint64_t *xuids, uint32_t count);
    RecompLiveRequest (*answer_invite)(void *self, uint64_t xuid, RecompLiveAnswer answer);
    /* The invite this player accepted from outside the game, false when none. */
    bool (*accepted_invite)(void *self, RecompLiveFriend *from);
    /* State: 1 idle, 0x13 in a session (XONLINE_NOTIFICATION state bits); kid zero when none. */
    void (*presence)(void *self, uint32_t state, const uint8_t kid[8]);

    /* Mutelist and feedback. */
    RecompLiveRequest (*mutelist)(void *self);
    uint32_t (*mutes)(void *self, uint64_t *out, uint32_t capacity);
    RecompLiveRequest (*feedback)(void *self, uint64_t xuid, uint32_t type);

    /* Stats and leaderboards (RANKINGS). */
    RecompLiveRequest (*stat_write)(void *self, uint32_t board, uint64_t xuid,
        const RecompLiveStat *stats, uint32_t count);
    RecompLiveRequest (*stat_read)(void *self, uint32_t board, const uint64_t *xuids, uint32_t count);
    uint32_t (*stat_results)(void *self, RecompLiveRequest request, RecompLiveLeaderRow *out, uint32_t capacity);
    RecompLiveRequest (*leader_enumerate)(void *self, uint32_t board, uint64_t pivot_xuid,
        uint32_t first_rank, uint32_t count);
    /* Total rows on the board, for the game's page count. */
    uint32_t (*leader_total)(void *self, RecompLiveRequest request);

    /* Polling. */
    RecompLiveStatus (*poll)(void *self, RecompLiveRequest request, uint32_t *hresult);
    void (*release)(void *self, RecompLiveRequest request);
    /* Once per frame: resend, time out, read pushed invites and presence. */
    void (*tick)(void *self, uint64_t now_ms);
};

/* The backend live_xapi.c uses; NULL until recomp_live_backend_set. */
const RecompLiveBackend *recomp_live_backend(void);
void recomp_live_backend_set(const RecompLiveBackend *backend);

/* In memory, for tests and two runners in one process. */
const RecompLiveBackend *recomp_live_backend_local(void);
void recomp_live_backend_local_reset(void);

#ifdef __cplusplus
}
#endif

#endif
