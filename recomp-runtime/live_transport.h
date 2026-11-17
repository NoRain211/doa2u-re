/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_TRANSPORT_H
#define DOA2U_RECOMP_LIVE_TRANSPORT_H

/* One UDP socket for everything Live: the matchmaker's packets, each peer's
   game datagrams (the guest's socket on port 1000), and the rollback session
   with the current opponent. The matchmaker must see the same socket the peers
   punch to, so nothing else opens one. live_transport.c owns it and demuxes by
   magic: "D2MM" to the server handler, "D2GD" to the guest queues, the
   rollback magics ("D2RB", "D2BY", ...) to the rollback queue. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompNetplay RecompNetplay;

/* What the matchmaker's PEER told both players of a pairing. */
typedef struct RecompLivePeerIntro {
    uint32_t virtual_ina;      /* network order */
    uint32_t public_address;   /* network order */
    uint16_t public_port;
    uint32_t lan_address;
    uint16_t lan_port;
    bool relay;                /* --relay-only or punching failed: through the server */
    uint8_t token[8];          /* the pair's token, which both sides' punches carry */
} RecompLivePeerIntro;

/* Keeps the server; the socket opens and the server resolves on the first
   request (recomp_live_transport_send_server). False for a bad server. */
bool recomp_live_transport_open(const char *server_host, uint16_t server_port);
void recomp_live_transport_close(void);
/* Reads every queued packet and demuxes it; resends punches. Called once per
   frame and from the guest's recvfrom. */
void recomp_live_transport_pump(uint64_t now_ms);

/* Matchmaker. */
typedef void (*RecompLiveServerHandler)(const uint8_t *packet, size_t size);
void recomp_live_transport_set_server_handler(RecompLiveServerHandler handler);
void recomp_live_transport_send_server(const uint8_t *packet, size_t size);
/* The socket's nonce, as the matchmaker's other packets carry it. */
void recomp_live_transport_nonce(uint8_t nonce[8]);

/* Peers, from the matchmaker's PEER. */
void recomp_live_transport_peer(const RecompLivePeerIntro *intro);
/* Drops one player's slot and route, or every one of them for address 0 (a
   logoff). The table also evicts its stalest peer when a new one needs a slot. */
void recomp_live_transport_forget(uint32_t virtual_ina);
bool recomp_live_transport_reachable(uint32_t virtual_ina);
/* True while this peer's datagrams go through the matchmaker's relay, so a
   caller can slow its repeats to the relay's packet budget. */
bool recomp_live_transport_relay(uint32_t virtual_ina);
/* Whether a packet from that player passed the token or path check since its
   introduction, which tells the client its CONNECT has done its work. */
bool recomp_live_transport_heard(uint32_t virtual_ina);
/* The local player's virtual address once logged on. */
void recomp_live_transport_set_local(uint32_t virtual_ina);

/* The guest's datagrams. Send returns the bytes taken (queued while the peer
   connects); receive returns the size, or -1 when none is queued. */
int recomp_live_transport_guest_send(uint32_t virtual_ina, uint16_t port, const uint8_t *data, size_t size);
int recomp_live_transport_guest_receive(uint8_t *data, size_t capacity, uint32_t *virtual_ina, uint16_t *port);

/* The rollback session's channel to the opponent: fills net so netplay.c's
   send goes to the opponent's route and its receive reads the rollback queue. */
bool recomp_live_transport_bind_rollback(uint32_t opponent_ina, RecompNetplay *net);
void recomp_live_transport_unbind_rollback(void);

/* Session frames (see live_session.h): drop sends in hidden frames, log what
   each frame received the first time and give the same in a frame run again,
   drop the game's own lockstep packets (type 9) while a session runs. */
void recomp_live_transport_session_begin(void);
void recomp_live_transport_session_end(void);

#ifdef __cplusplus
}
#endif

#endif
