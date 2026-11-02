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
    RECOMP_NETPLAY_HEADER_BYTES = 21u,
    RECOMP_NETPLAY_MAX_PADS = RECOMP_ROLLBACK_HISTORY,
    RECOMP_NETPLAY_MAX_PACKET =
        RECOMP_NETPLAY_HEADER_BYTES + RECOMP_NETPLAY_MAX_PADS * RECOMP_NETPLAY_PAD_BYTES,
    RECOMP_NETPLAY_SYNC_FRAMES = 32u,
    RECOMP_NETPLAY_DIGEST_BYTES = 64u,
    /* The largest profile a host sends; it arrives in 1 KiB chunks. */
    RECOMP_NETPLAY_MAX_PROFILE = 1024u * 1024u,
};

/* What the lobby agrees on before either runner boots. */
typedef struct RecompNetplayLobby {
    char digest[RECOMP_NETPLAY_DIGEST_BYTES]; /* runner build; both sides must match */
    uint8_t delay;                            /* input delay in frames */
    uint8_t flags;                            /* host options the joiner adopts */
} RecompNetplayLobby;

/* The two players' profiles, which the lobby swaps. */
typedef struct RecompNetplayProfiles {
    const uint8_t *mine;
    uint32_t mine_size;   /* at most RECOMP_NETPLAY_MAX_PROFILE */
    uint8_t *theirs;      /* holds RECOMP_NETPLAY_MAX_PROFILE bytes */
    uint32_t theirs_size; /* set by a successful lobby */
} RecompNetplayProfiles;

typedef enum RecompNetplayLobbyResult {
    RECOMP_NETPLAY_LOBBY_OK,
    RECOMP_NETPLAY_LOBBY_TIMEOUT,
    RECOMP_NETPLAY_LOBBY_MISMATCH, /* the other runner is a different build */
    RECOMP_NETPLAY_LOBBY_SOCKET,
} RecompNetplayLobbyResult;

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
} RecompNetplay;

/* Binds local_port on all interfaces; packets from any other address are
   dropped. shortcut: IPv4 only; add AF_INET6 when the lobby needs it. */
bool recomp_netplay_open(RecompNetplay *net, uint16_t local_port,
    const char *peer_host, uint16_t peer_port);
/* Binds port and waits for a joiner's hello, answering each with the host's
   digest and settings. The first joiner becomes the peer; one with another
   digest ends the wait with MISMATCH. Returns once both sides hold both
   profiles. */
RecompNetplayLobbyResult recomp_netplay_host(RecompNetplay *net, uint16_t port,
    const RecompNetplayLobby *lobby, RecompNetplayProfiles *profiles, uint32_t timeout_ms);
/* Says hello to the host until it answers, copies the host's delay and flags
   into lobby, then swaps profiles as the host does. Binds an ephemeral local
   port. */
RecompNetplayLobbyResult recomp_netplay_join(RecompNetplay *net, const char *host,
    uint16_t port, RecompNetplayLobby *lobby, RecompNetplayProfiles *profiles, uint32_t timeout_ms);
void recomp_netplay_close(RecompNetplay *net);
/* The local IPv4 address of the interface that reaches the internet, as
   text; it also starts Winsock on Windows. */
bool recomp_netplay_route_address(char *text, size_t size);
/* Asks the router over UPnP to forward UDP port to this computer, and
   prints the outcome and the address the other player joins. False when no
   router answered or it refused; the port may still be reachable. */
bool recomp_netplay_open_router_port(uint16_t port);
/* Removes that forward; nothing when none was made. */
void recomp_netplay_close_router_port(void);
void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage);
/* Drains the socket into the session. */
void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *session);
/* Frames to wait now so neither side runs ahead: half the difference of the
   two averaged advantages, which cancels the latency both of them include.
   Call once per new frame with the advantage that frame sent. */
uint32_t recomp_netplay_wait_frames(RecompNetplay *net, int32_t advantage);

size_t recomp_netplay_encode(const RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage, uint8_t *packet);
/* False for a malformed packet, which changes nothing. */
bool recomp_netplay_decode(RecompNetplay *net, RecompRollbackSession *session,
    const uint8_t *packet, size_t size);

#ifdef __cplusplus
}
#endif

#endif
