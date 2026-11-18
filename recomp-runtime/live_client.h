/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_CLIENT_H
#define DOA2U_RECOMP_LIVE_CLIENT_H

/* RecompLiveBackend (live_backend.h) over tools/matchmaker.py's Live kinds
   (docs/matchmaking.md), through the one socket live_transport.c owns. Every
   request carries an id, goes out again every 500 ms until its answer, and
   fails after 5 s. A logon is a CHALLENGE and then the LOGON that answers the
   server's nonce, under one id; after LOGGED the client logs on again that
   way every 5 s as the heartbeat, with the player's presence. An answer that
   arrives late is dropped, and a repeated request id gets the server's first
   answer again. */
#include "live_backend.h"
#include "live_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_LIVE_PACKET_BYTES = 1200, /* the matchmaker's limit, both ways */
};

/* The backend over a transport that is already open. gamertag is the name
   the game shows for this player (the netplay profile's), digest the
   runner build's 64 hex characters; the server answers the gamertag it
   actually gave, which may carry a digit. server is the matchmaker's
   "host:port" as the runner has it, kept in lowercase: after a logon there
   the identity's list names it, and it is never sent the proof again. NULL
   remembers no server. The test calls this directly. */
const RecompLiveBackend *recomp_live_client_attach(const RecompLiveIdentity *identity, const char *gamertag,
    const char *digest, const char *server);
/* Drops the backend and the transport's server handler; the transport stays open. */
void recomp_live_client_detach(void);
bool recomp_live_client_attached(void);
/* Once per frame: resends, timeouts and the heartbeat. */
void recomp_live_client_tick(uint64_t now_ms);

/* The runner's version (live_bridge.cpp, which reads the netplay side): the
   identity in <storage>/live-identity.bin and its server list beside it, the
   gamertag from the player's profile, and the matchmaker from NETPLAY
   SETTINGS, or the built-in one.
   RECOMP_LIVE_SERVER=host:port overrides both; a bare host keeps the port
   MATCHMAKER names (or the built-in server's). It opens the transport and
   returns the backend to give recomp_live_backend_set, or NULL. */
const RecompLiveBackend *recomp_live_client_start(const char *storage, const char *digest);
void recomp_live_client_stop(void);

#ifdef __cplusplus
}
#endif

#endif
