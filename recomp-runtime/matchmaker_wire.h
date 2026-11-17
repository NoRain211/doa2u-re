#ifndef DOA2U_RECOMP_MATCHMAKER_WIRE_H
#define DOA2U_RECOMP_MATCHMAKER_WIRE_H

/* What netplay.c and live_transport.c share: the matchmaker's packet framing
   (tools/matchmaker.py, docs/matchmaking.md). Socket-free, so the Live
   transport can frame a request without the rollback socket's types. Callers
   still use netplay.h and netplay_wire.h for the transport. */
#include <string.h>

#include "netplay.h"

/* The matchmaker's packets: "D2MM", version, kind, the client socket's
   nonce, then for HOST, JOIN, LEAVE, RELAY and LIVE_RELAY the server's
   cookie, then a body whose numbers are big endian. */
static const uint8_t MATCH_MAGIC[4] = {'D', '2', 'M', 'M'};
enum {
    MATCH_VERSION = 2u,
    MATCH_HOST = 1u, MATCH_HOSTED, MATCH_LIST, MATCH_ROOMS, MATCH_JOIN, MATCH_PEER, MATCH_GONE, MATCH_LEAVE,
    MATCH_RELAY, MATCH_COOKIE,
    MATCH_LIVE_RELAY = 34u, /* the Live client asks for one addressee */
    MATCH_HEADER = 6u,
    MATCH_BODY = MATCH_HEADER + RECOMP_NETPLAY_TOKEN_BYTES, /* after the nonce */
    MATCH_ASKED = MATCH_BODY + RECOMP_NETPLAY_TOKEN_BYTES,  /* after the cookie */
};

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

#endif
