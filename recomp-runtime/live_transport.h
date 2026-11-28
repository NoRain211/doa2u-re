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

/* Keeps the server; the first request (recomp_live_transport_send_server)
   starts the server's name lookup on a thread of its own, and the socket opens
   once the name is found, so a request goes out from the first one sent after
   that. No request waits for the lookup. False for a bad server. */
bool recomp_live_transport_open(const char *server_host, uint16_t server_port);
/* How a name is looked up: recomp_netplay_resolve unless the test makes it
   slow. NULL restores it. */
typedef bool (*RecompLiveResolver)(const char *host, uint32_t *address);
void recomp_live_transport_set_resolver(RecompLiveResolver resolver);
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

/* QoS: the game's XNetQosLookup on the same socket. live_xapi.c starts one
   request per guest handle (slot 0..RECOMP_LIVE_QOS_SLOTS-1); the transport
   probes each address's peer on the route its game datagrams use, so a
   direct pairing measures the direct path and a relayed one the relay. A
   peer's runner that predates the probes ignores them, and the entry falls
   back to the shipped 50 ms, as does an address never paired (a search
   result). A peer forgotten mid-request is the console's no-reply entry
   (complete, wRtt 0xffff). */
enum { RECOMP_LIVE_QOS_SLOTS = 8, RECOMP_LIVE_QOS_PROBES = 5 };
typedef enum RecompLiveQosState {
    RECOMP_LIVE_QOS_PENDING = 0,
    RECOMP_LIVE_QOS_ANSWERED = 1, /* the peer's runner answered the probes */
    RECOMP_LIVE_QOS_NO_REPLY = 2, /* the peer was forgotten mid-request */
    RECOMP_LIVE_QOS_FALLBACK = 3, /* no answer, or never paired: the 50 ms default */
} RecompLiveQosState;
typedef struct RecompLiveQosEntry {
    uint16_t rtt_min_ms;      /* 0 until the entry settles */
    uint16_t rtt_med_ms;
    uint16_t probes_sent;
    uint16_t probes_received;
    uint8_t state;            /* RecompLiveQosState */
} RecompLiveQosEntry;
/* Starts (or restarts) one request; false when the slot or count is bad. */
bool recomp_live_transport_qos_start(unsigned slot, const uint32_t *virtual_inas, uint32_t count);
/* Copies what is known so far; returns the entries written and reports
   whether every entry has settled. */
uint32_t recomp_live_transport_qos_results(unsigned slot, RecompLiveQosEntry *out, uint32_t capacity,
    bool *done);
void recomp_live_transport_qos_stop(unsigned slot);
/* Whether a datagram is shaped as the game's own senders build it; the
   guest's recvfrom drops any other, since the game's handlers trust them. */
bool recomp_live_game_datagram_ok(const uint8_t *datagram, size_t size);
/* Whether a dropped datagram is one of the game's own empty type-15 replies
   (not chosen yet, handlers 0-3; not ready, handler 5), which it resends
   every second; recvfrom drops these without a log line. */
bool recomp_live_game_datagram_expected_drop(const uint8_t *datagram, size_t size);

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
