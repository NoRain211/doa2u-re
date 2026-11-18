#ifndef DOA2U_RECOMP_NETPLAY_H
#define DOA2U_RECOMP_NETPLAY_H

#include "rollback.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP transport for a rollback session. Each packet carries every local pad
   the peer has not acknowledged, so a lost packet is covered by the next one
   and there is no resend. */
enum {
    RECOMP_NETPLAY_PAD_BYTES = 18u,
    RECOMP_NETPLAY_HEADER_BYTES = 33u,
    RECOMP_NETPLAY_MAX_PADS = RECOMP_ROLLBACK_HISTORY,
    /* At most the 1,200 bytes the matchmaker relays (tools/matchmaker.py). */
    RECOMP_NETPLAY_MAX_PACKET =
        RECOMP_NETPLAY_HEADER_BYTES + RECOMP_NETPLAY_MAX_PADS * RECOMP_NETPLAY_PAD_BYTES,
    RECOMP_NETPLAY_SYNC_FRAMES = 32u,
    /* Desync checks a side keeps to compare with the other's newest. */
    RECOMP_NETPLAY_CHECKS = 8u,
    RECOMP_NETPLAY_TOKEN_BYTES = 8u, /* a socket's nonce and the matchmaker's cookie */
};

/* A socket's matchmaker (tools/matchmaker.py): its address and the tokens
   that prove each side. Live's transport (live_transport.c) owns the rest of
   the match: peers, their endpoints and the relay. */
typedef struct RecompNetplayMatch {
    uint32_t server_address; /* network order; port 0 means none */
    uint16_t server_port;
    /* Random for each socket; the server's packets echo it, so forged ones are dropped. */
    uint8_t nonce[RECOMP_NETPLAY_TOKEN_BYTES];
    /* The server's proof of this socket's address, zeros until it sends one. */
    uint8_t cookie[RECOMP_NETPLAY_TOKEN_BYTES];
} RecompNetplayMatch;

typedef struct RecompNetplay {
    uintptr_t socket;
    uint32_t peer_address; /* IPv4, network order */
    uint16_t peer_port;    /* network order */
    bool peer_seen;
    uint32_t peer_ack;     /* local pads the peer holds */
    uint32_t peer_frame;   /* the peer's next frame when it last sent */
    int32_t peer_advantage;
    /* One advantage per frame on each side. Sampling the peer once per local
       frame would count its stalls, when it sends nothing new, too often. */
    int32_t local_samples[RECOMP_NETPLAY_SYNC_FRAMES];
    int32_t peer_samples[RECOMP_NETPLAY_SYNC_FRAMES];
    uint32_t local_count;
    uint32_t peer_count;
    uint32_t cooldown;
    /* Round trip: from a local pad's first send until the peer says it holds it. */
    uint64_t pad_sent_ms[RECOMP_NETPLAY_MAX_PADS];
    uint32_t pads_stamped;
    uint32_t rtt_ms; /* smoothed over the last few samples */
    bool rtt_known;
    bool peer_left; /* the other runner said goodbye */
    /* Desync checks: hashes of guest RAM at frames both sides' pads are
       known for. Each packet carries this side's newest; frame 0 is none. */
    uint32_t check_frames[RECOMP_NETPLAY_CHECKS];
    uint64_t check_hashes[RECOMP_NETPLAY_CHECKS];
    uint32_t check_count;
    uint32_t peer_check_frame;
    uint64_t peer_check_hash;
    RecompNetplayMatch match;
} RecompNetplay;

/* Binds port on all interfaces with no peer yet. Live's transport sets the
   peer from the matchmaker's introductions. */
bool recomp_netplay_listen(RecompNetplay *net, uint16_t port);
/* Points a socket at the matchmaker; false when host does not resolve. */
bool recomp_netplay_match_server(RecompNetplay *net, const char *host, uint16_t port);
void recomp_netplay_close(RecompNetplay *net);
/* True for a profile name that is not allowed online, or one too long to
   check. Takes UTF-8. */
bool recomp_netplay_name_banned(const char *name);
void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage);
/* Tells the peer this runner is quitting, a few times since UDP may drop one. */
void recomp_netplay_send_goodbye(const RecompNetplay *net);
/* Drains the socket into the session. */
void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *session);
/* Frames to wait now so neither side runs ahead: half the difference of the
   two averaged advantages, which cancels the latency both of them include.
   Call once per new frame with the advantage that frame sent. */
uint32_t recomp_netplay_wait_frames(RecompNetplay *net, int32_t advantage);
/* Keeps this side's hash of a confirmed frame for its packets to carry. */
void recomp_netplay_add_check(RecompNetplay *net, uint32_t frame, uint64_t hash);
/* This side's hash for the frame of the other side's newest check (peer_check_frame);
   false when this side has not hashed that frame or no longer keeps it. */
bool recomp_netplay_own_check(const RecompNetplay *net, uint64_t *mine);

size_t recomp_netplay_encode(const RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage, uint8_t *packet);
/* False for a malformed packet, which changes nothing. A goodbye sets peer_left. */
bool recomp_netplay_decode(RecompNetplay *net, RecompRollbackSession *session,
    const uint8_t *packet, size_t size);

#ifdef __cplusplus
}
#endif

#endif
