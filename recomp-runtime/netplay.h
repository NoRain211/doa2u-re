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
};

typedef struct RecompNetplay {
    uintptr_t socket;
    uint32_t peer_address; /* IPv4, network order */
    uint16_t peer_port;    /* network order */
    bool peer_seen;
    uint32_t peer_ack;     /* local pads the peer holds */
    uint32_t peer_frame;   /* the peer's next frame when it last sent */
    int32_t peer_advantage;
    /* Recent (local - peer advantage) samples for the frame wait. */
    int32_t gaps[RECOMP_NETPLAY_SYNC_FRAMES];
    uint32_t gap_count;
    uint32_t cooldown;
} RecompNetplay;

/* Binds local_port on all interfaces; packets from any other address are
   dropped. shortcut: IPv4 only; add AF_INET6 when the lobby needs it. */
bool recomp_netplay_open(RecompNetplay *net, uint16_t local_port,
    const char *peer_host, uint16_t peer_port);
void recomp_netplay_close(RecompNetplay *net);
void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage);
/* Drains the socket into the session. */
void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *session);
/* Frames to wait now so neither side runs ahead: half the averaged difference
   of the two advantages, which cancels the latency both of them include. */
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
