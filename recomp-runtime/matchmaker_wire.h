#ifndef DOA2U_RECOMP_MATCHMAKER_WIRE_H
#define DOA2U_RECOMP_MATCHMAKER_WIRE_H

/* What netplay.c, live_transport.c and live_client.c share: the matchmaker's
   packet framing and kind space (tools/matchmaker.py, docs/matchmaking.md).
   Socket-free, so the Live transport can frame a request without the rollback
   socket's types. Callers still use netplay.h and netplay_wire.h for the
   transport. */
#include <string.h>

#include "netplay.h"

/* The matchmaker's packets: "D2MM", version, kind, the client socket's
   nonce, then for HOST, JOIN, LEAVE, RELAY and LIVE_RELAY the server's
   cookie, then a body whose numbers are big endian. The kinds sit in their
   number order: 1..10 are the room matchmaker's, 12..33 are the Live
   client's (live_client.c, whose LIVE_ names they keep), 34 relays for Live. */
static const uint8_t MATCH_MAGIC[4] = {'D', '2', 'M', 'M'};
enum {
    MATCH_VERSION = 2u,
    MATCH_HOST = 1u, MATCH_HOSTED, MATCH_LIST, MATCH_ROOMS, MATCH_JOIN, MATCH_PEER, MATCH_GONE, MATCH_LEAVE,
    MATCH_RELAY, MATCH_COOKIE, MATCH_ADDR6,
    LIVE_LOGON = 12u, LIVE_LOGGED, LIVE_SESSION_PUT, LIVE_SESSION_OK, LIVE_SESSION_DEL, LIVE_SEARCH,
    LIVE_RESULT, LIVE_FIND, LIVE_CONNECT, LIVE_LPEER, LIVE_FRIENDS, LIVE_FRIEND, LIVE_FRIEND_OP, LIVE_DONE,
    LIVE_PUSH, LIVE_MUTES, LIVE_MUTELIST, LIVE_FEEDBACK, LIVE_STAT_WRITE, LIVE_STAT_READ, LIVE_STATROW,
    LIVE_LEADERS,
    MATCH_LIVE_RELAY = 34u, /* the Live client asks for one addressee */
    MATCH_HEADER = 6u,
    MATCH_BODY = MATCH_HEADER + RECOMP_NETPLAY_TOKEN_BYTES, /* after the nonce */
    MATCH_ASKED = MATCH_BODY + RECOMP_NETPLAY_TOKEN_BYTES,  /* after the cookie */
};
_Static_assert(LIVE_LEADERS + 1 == MATCH_LIVE_RELAY, "a Live kind added to the list takes LIVE_RELAY's number");

/* Writes a packet's start and returns where its body goes. */
static inline uint8_t *match_header(uint8_t *out, uint8_t kind, const uint8_t *nonce, const uint8_t *cookie)
{
    memcpy(out, MATCH_MAGIC, sizeof MATCH_MAGIC);
    out[4] = MATCH_VERSION;
    out[5] = kind;
    memcpy(out + MATCH_HEADER, nonce, RECOMP_NETPLAY_TOKEN_BYTES);
    if (kind == MATCH_LIST) return out + MATCH_BODY;
    memcpy(out + MATCH_BODY, cookie, RECOMP_NETPLAY_TOKEN_BYTES);
    return out + MATCH_ASKED;
}

/* A body's numbers are big endian. */
static inline void put32be(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) out[i] = (uint8_t)(value >> (24u - 8u * i));
}

static inline uint32_t get32be(const uint8_t *in)
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

#endif
