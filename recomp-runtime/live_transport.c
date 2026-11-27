/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xbox Live's transport (live_transport.h). The game's one UDP socket on port
   1000 is carried as "D2GD" datagrams over the host's single Live socket,
   beside the matchmaker's "D2MM" packets (tools/matchmaker.py, docs/
   matchmaking.md) and the rollback session's "D2RB"/"D2BY" packets on the
   same socket. The matchmaker must see the endpoint the peers punch to, so
   nothing here opens a second one.

   Reused from netplay.c: recomp_netplay_listen
   opens and randomizes the socket, recomp_netplay_resolve looks up the
   server, netplay_send_raw sends, netplay_now_ms reads the clock, and the
   matchmaker's cookie/nonce framing is matchmaker_wire.h's, shared with
   netplay.c. Punching repeats
   to both endpoints the matchmaker gave, carries the pair token the LPEER
   handed both players so only a peer in this pairing sets the path, the first
   checked answer becomes it, and after 5 s the packets go through the
   server's relay (kind 34, naming one addressee).
   A bound rollback session reaches its opponent through the same route (see
   netplay_wire.h), so a fight and its guest datagrams share one path.

   The transport is one instance per runner process; the test pairs it with a
   scripted peer and matchmaker on loopback.
   Everything runs on the frame thread: recomp_live_transport_pump once per
   frame, and from the guest's recvfrom, which never blocks. The one exception
   is the matchmaker's name lookup (LiveLookup), which blocks and so has a
   thread of its own. */

#include "live_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>
#endif

#include "kernel_abi.h"
#include "live_backend.h"
#include "live_session.h"
#include "matchmaker_wire.h"
#include "netplay_wire.h"

/* live_xapi.h declares this with the rest of the guest's thunks; it is
   declared beside its definition so the shared Live headers stay unchanged. */
RecompFunction recomp_live_transport_lookup(uint32_t guest_address);

enum {
    /* The game's session holds up to 8 players; one entry per virtual address. */
    LIVE_PEERS = 8,
    /* Game datagrams held per peer until its path works. The game repeats what
       matters (lockstep resends, results) and a punch normally answers in
       well under a frame, so this only has to cover a connect in progress. */
    LIVE_PEER_QUEUE = 16,
    /* Datagrams waiting for the guest's next recvfrom. */
    LIVE_GUEST_QUEUE = 32,
    /* The game's datagram, and the largest one it sends. */
    LIVE_GUEST_BYTES = 1200,
    /* What each session frame received the first time it ran, so a frame run
       again after a rewind receives the same datagrams in the same order. */
    LIVE_RECV_LOG = 256,
    /* Netplay packets waiting for the bound rollback session. */
    LIVE_ROLLBACK_QUEUE = 32,
    /* The game's socket port, in the guest's XNADDR and its sockaddr_in. */
    LIVE_GUEST_PORT = 1000,
    /* Punching: repeat to both endpoints this often, and relay after this long. */
    LIVE_PUNCH_MS = 100u,
    LIVE_PUNCH_GIVE_UP_MS = 5000u,
    /* The game sent but nothing checked has come back for this long: ask the
       matchmaker for the other player again, at most this often. */
    LIVE_SILENT_MS = 10000u,
    /* The matchmaker's cap: a Live request is at most this long, and a relay's
       whole packet at most MATCH_ASKED more, tools/matchmaker.py. */
    LIVE_MATCH_BYTES = 1200,
    /* The Live log counts each peer's game datagrams by type (0-21, then one
       slot for the rest) and says them this often. */
    LIVE_LOG_TYPES = 23,
    LIVE_LOG_MS = 10000u,
};

/* The guest's datagram: magic, kind, the sender's virtual address and port
   (network order, as the guest's sockaddr_in holds them), then the body. A
   game datagram's body is the game's own; a punch's is the pair token, and a
   punch never reaches the game, whatever its token. */
static const uint8_t GUEST_MAGIC[4] = {'D', '2', 'G', 'D'};
enum {
    GUEST_VERSION = 1u, /* a game datagram */
    GUEST_PUNCH = 2u,
    GUEST_HEADER = 4u + 1u + 4u + 2u,
    /* The largest guest datagram the relay can carry: the server allows a
       whole request of LIVE_MATCH_BYTES, and the addressee and this header
       come out of it first. */
    LIVE_RELAY_BYTES = LIVE_MATCH_BYTES - 4u - GUEST_HEADER,
};

/* Winsock's error numbers, which the guest reads whatever the host platform. */
enum {
    LIVE_WSAEWOULDBLOCK = 10035u,
    LIVE_WSAEMSGSIZE = 10040u,
    LIVE_WSAEHOSTUNREACH = 10065u,
};

typedef struct LiveQueued {
    uint16_t port; /* network order */
    uint16_t size;
    uint8_t data[LIVE_GUEST_BYTES];
} LiveQueued;

typedef struct LiveGuestPacket {
    uint32_t virtual_ina;
    uint16_t port; /* network order */
    uint16_t size;
    uint8_t data[LIVE_GUEST_BYTES];
} LiveGuestPacket;

typedef struct LiveLogEntry {
    uint32_t frame;
    uint32_t virtual_ina;
    uint16_t port;
    uint16_t size;
    uint8_t data[LIVE_GUEST_BYTES];
} LiveLogEntry;

typedef struct LiveRollbackPacket {
    uint32_t virtual_ina;
    uint16_t port;
    uint16_t size;
    uint8_t data[RECOMP_NETPLAY_MAX_PACKET];
} LiveRollbackPacket;

/* One other player: where the matchmaker said it is, and how it answers. */
typedef struct LivePeer {
    bool used;
    uint32_t virtual_ina; /* network order */
    bool introduced;      /* a LPEER named it */
    /* The public and LAN endpoints the matchmaker gave, network order, port 0
       when unknown. Punching sends to both until one answers. */
    uint32_t address[2];
    uint16_t port[2];
    /* The pair's token, which the matchmaker sends both players identically and
       their punches repeat. It is not a socket nonce: knowing it only proves
       membership of this pairing. */
    uint8_t token[8];
    bool relay;       /* our packets to it go through the matchmaker */
    bool connected;   /* a token-checked punch set the direct path */
    uint32_t address_now;
    uint16_t port_now;
    uint64_t punch_start;
    uint64_t next_punch;
    uint64_t last_ms; /* its last activity, for evicting the stalest peer */
    bool heard;       /* a checked packet came from it since its introduction */
    bool waiting;     /* the game sent to it since the last checked packet */
    uint64_t waiting_ms; /* the first of those sends */
    uint64_t heard_ms; /* that packet's time, or when the slot was claimed */
    uint64_t sent_ms;  /* the game's last datagram to it */
    uint64_t asked_ms; /* the last time this side asked the matchmaker for it */
    uint32_t sent_types[LIVE_LOG_TYPES]; /* game datagrams since the last log line */
    uint32_t got_types[LIVE_LOG_TYPES];
    bool quiet_said; /* the log said it went quiet, and nothing came since */
    uint16_t queue_head;
    uint16_t queue_count;
    LiveQueued queue[LIVE_PEER_QUEUE];
} LivePeer;

/* The matchmaker's name lookup. getaddrinfo blocks, for seconds on a slow
   resolver, and every request comes from the frame thread, so the lookup runs
   on a thread of its own. The transport may close while it runs, so the
   lookup belongs to both: whichever lets go of it second frees it. */
enum { LOOKUP_RUNNING, LOOKUP_FOUND, LOOKUP_FAILED, LOOKUP_DROPPED };
typedef struct LiveLookup {
    char host[256];
    RecompLiveResolver resolve;
    uint32_t address; /* network order, once the state is LOOKUP_FOUND */
#ifdef _WIN32
    volatile LONG state;
#else
    atomic_int state;
#endif
} LiveLookup;

static struct {
    bool started; /* recomp_live_transport_open ran; the socket waits for the first request */
    bool open; /* the socket is up and the server resolved */
    char server_host[256];
    uint16_t server_port;
    uint64_t retry_ms; /* when a lookup or socket that failed is tried again */
    LiveLookup *lookup; /* the server's name, being looked up */
    RecompNetplay net; /* the socket, its nonce, cookie and the relay framing */
    uint32_t last_error;
    uint32_t local_ina; /* this player's virtual address, once logged on */
    uint16_t guest_port; /* the game's bound port, network order */
    LivePeer peers[LIVE_PEERS];
    /* What waits for the guest's recvfrom. */
    uint16_t guest_head;
    uint16_t guest_count;
    LiveGuestPacket guest[LIVE_GUEST_QUEUE];
    /* What each session frame received the first time it ran. */
    uint32_t log_head;
    uint32_t replay_frame;
    uint32_t replay_at;
    LiveLogEntry log[LIVE_RECV_LOG];
    /* Matchmaker: the last request, sent again when a new cookie answers it. */
    uint8_t asked[MATCH_ASKED + LIVE_GUEST_BYTES];
    size_t asked_size;
    uint64_t asked_ms;
    RecompLiveServerHandler handler;
    /* The rollback session of a Live fight, and its opponent's datagrams. */
    bool rollback_bound;
    uint32_t rollback_ina;
    RecompNetplay *rollback_net;
    uint16_t rollback_head;
    uint16_t rollback_count;
    LiveRollbackPacket rollback[LIVE_ROLLBACK_QUEUE];
    uint64_t log_ms; /* the last per-peer datagram line */
} live;

/* A virtual address for the log. The peer table holds it as the matchmaker's
   big-endian number (live_client.c reads it with get32be), first octet on top. */
static void ina_text(uint32_t virtual_ina, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(virtual_ina >> 24), (unsigned)(virtual_ina >> 16 & 0xffu),
        (unsigned)(virtual_ina >> 8 & 0xffu), (unsigned)(virtual_ina & 0xffu));
}

static unsigned log_type(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 3u || data[2] >= LIVE_LOG_TYPES - 1u) return LIVE_LOG_TYPES - 1u;
    return data[2];
}

size_t recomp_live_format_type_counts(char *out, size_t capacity, const uint32_t *counts, unsigned count)
{
    if (out == NULL || capacity == 0u) return 0u;
    out[0] = '\0';
    size_t at = 0u;
    for (unsigned i = 0u; i < count && at + 1u < capacity; ++i) {
        if (counts[i] == 0u) continue;
        char item[32];
        if (i + 1u == count) snprintf(item, sizeof item, "%s?x%u", at != 0u ? " " : "", counts[i]);
        else snprintf(item, sizeof item, "%s%ux%u", at != 0u ? " " : "", i, counts[i]);
        const int wrote = snprintf(out + at, capacity - at, "%s", item);
        if (wrote < 0) break;
        at += (size_t)wrote < capacity - at ? (size_t)wrote : capacity - at - 1u;
    }
    if (at == 0u) at = (size_t)snprintf(out, capacity, "none");
    return at < capacity ? at : capacity - 1u;
}

/* The session frame being run, or UINT32_MAX outside a Live fight. Replaying
   needs both: a frame run again after a rewind is not the socket's turn. */
static uint32_t session_frame(void)
{
    return recomp_live_rollback_active() ? recomp_live_rollback_frame() : UINT32_MAX;
}

static bool replaying(void)
{
    return recomp_live_rollback_active() && recomp_live_rollback_hidden();
}

static LivePeer *peer_find(uint32_t virtual_ina)
{
    for (unsigned i = 0u; i < LIVE_PEERS; ++i)
        if (live.peers[i].used && live.peers[i].virtual_ina == virtual_ina) return &live.peers[i];
    return NULL;
}

/* Datagrams from a peer that is gone are stale too. */
static void drop_rollback_from(uint32_t virtual_ina)
{
    uint16_t kept = 0u;
    for (uint16_t i = 0u; i < live.rollback_count; ++i) {
        const LiveRollbackPacket *slot = &live.rollback[(live.rollback_head + i) % LIVE_ROLLBACK_QUEUE];
        if (slot->virtual_ina == virtual_ina) continue;
        if (kept != i) live.rollback[(live.rollback_head + kept) % LIVE_ROLLBACK_QUEUE] = *slot;
        ++kept;
    }
    live.rollback_count = kept;
}

static void peer_clear(LivePeer *peer)
{
    const uint32_t virtual_ina = peer->virtual_ina;
    memset(peer, 0, sizeof *peer);
    drop_rollback_from(virtual_ina);
}

/* A slot for one player, or NULL when every slot holds a peer that must stay
   (the bound rollback session's). A full table evicts the least recently
   active peer, so a long run of matches does not exhaust it. An introduction
   the game did not ask for (keep_heard) never evicts a peer heard in the last
   LIVE_SILENT_MS, so strangers CONNECTing to a host cannot push out its
   opponent. */
static LivePeer *peer_claim(uint32_t virtual_ina, bool keep_heard)
{
    LivePeer *peer = peer_find(virtual_ina);
    if (peer != NULL) return peer;
    const uint64_t now = netplay_now_ms();
    LivePeer *free_slot = NULL, *stale = NULL;
    for (unsigned i = 0u; i < LIVE_PEERS; ++i) {
        if (!live.peers[i].used) {
            free_slot = &live.peers[i];
            break;
        }
        if (live.rollback_bound && live.peers[i].virtual_ina == live.rollback_ina) continue;
        if (keep_heard && live.peers[i].heard && now < live.peers[i].heard_ms + LIVE_SILENT_MS) continue;
        if (stale == NULL || live.peers[i].last_ms < stale->last_ms) stale = &live.peers[i];
    }
    if (free_slot == NULL) {
        free_slot = stale;
        if (free_slot != NULL) {
            char old[16], incoming[16];
            ina_text(free_slot->virtual_ina, old);
            ina_text(virtual_ina, incoming);
            fprintf(stderr, "recomp live: forgot %s, the least active player, to make room for %s\n", old, incoming);
            peer_clear(free_slot);
        }
    }
    if (free_slot == NULL) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->virtual_ina = virtual_ina;
    free_slot->last_ms = now;
    free_slot->heard_ms = free_slot->last_ms;
    free_slot->asked_ms = free_slot->last_ms;
    return free_slot;
}

/* A packet that passed the token or path check: the peer is alive. */
static void peer_heard(LivePeer *peer)
{
    peer->last_ms = netplay_now_ms();
    peer->heard = true;
    peer->heard_ms = peer->last_ms;
    peer->waiting = false;
}

static void peer_ask(LivePeer *peer, uint64_t now)
{
    peer->asked_ms = now;
    const RecompLiveBackend *backend = recomp_live_backend();
    if (backend != NULL && backend->connect != NULL) backend->connect(backend->self, peer->virtual_ina);
}

/* A guest header's sender address is big-endian on the wire, as the
   matchmaker writes it into LOGGED, LPEER and a relayed datagram. */
static void put_ina(uint8_t *at, uint32_t virtual_ina)
{
    at[0] = (uint8_t)(virtual_ina >> 24);
    at[1] = (uint8_t)(virtual_ina >> 16);
    at[2] = (uint8_t)(virtual_ina >> 8);
    at[3] = (uint8_t)virtual_ina;
}

/* Sends one guest datagram to one endpoint. */
static void send_wire(uint32_t address, uint16_t port, uint32_t virtual_ina, uint16_t guest_port,
    const uint8_t *data, size_t size)
{
    uint8_t packet[GUEST_HEADER + LIVE_GUEST_BYTES];
    if (size > LIVE_GUEST_BYTES) return;
    memcpy(packet, GUEST_MAGIC, sizeof GUEST_MAGIC);
    packet[4] = GUEST_VERSION;
    put_ina(packet + 5u, virtual_ina);
    memcpy(packet + 9u, &guest_port, 2u);
    if (size != 0u) memcpy(packet + GUEST_HEADER, data, size);
    /* A full send buffer drops this datagram; the game repeats what matters. */
    netplay_send_raw(&live.net, address, port, packet, GUEST_HEADER + size);
}

/* A punch is the guest header plus the pair's token. Only its answer proves the
   path, and a token from another pairing cannot move it. */
static void send_punch(uint32_t address, uint16_t port, const uint8_t token[RECOMP_NETPLAY_TOKEN_BYTES],
    bool answer)
{
    uint8_t packet[GUEST_HEADER + RECOMP_NETPLAY_TOKEN_BYTES + 1u];
    memcpy(packet, GUEST_MAGIC, sizeof GUEST_MAGIC);
    packet[4] = GUEST_PUNCH;
    put_ina(packet + 5u, live.local_ina);
    memcpy(packet + 9u, &live.guest_port, 2u);
    memcpy(packet + GUEST_HEADER, token, RECOMP_NETPLAY_TOKEN_BYTES);
    packet[GUEST_HEADER + RECOMP_NETPLAY_TOKEN_BYTES] = 1u; /* sent only on an answer */
    netplay_send_raw(&live.net, address, port, packet, sizeof packet - (answer ? 0u : 1u));
}

/* Wraps one guest datagram or punch (kind) for the relay, naming the
   addressee: the server checks the pair CONNECT made, stamps the sender's real
   address and forwards it to that one peer. */
static void send_relay(uint32_t destination_ina, uint16_t guest_port, uint8_t kind, const uint8_t *data,
    size_t size)
{
    uint8_t packet[MATCH_ASKED + 4u + GUEST_HEADER + LIVE_GUEST_BYTES];
    static bool said_too_big;
    if (live.net.match.server_port == 0u) return;
    /* The server refuses a request longer than its cap, which leaves the guest
       datagram LIVE_RELAY_BYTES: a rollback packet of 1,185 bytes still fits.
       A send while relaying fails with WSAEMSGSIZE first; this is one queued
       before the relay began. */
    if (size > LIVE_RELAY_BYTES) {
        if (!said_too_big) fprintf(stderr, "recomp live: a %u-byte datagram is too big for the relay\n", (unsigned)size);
        said_too_big = true;
        return;
    }
    uint8_t *body = match_header(packet, MATCH_LIVE_RELAY, live.net.match.nonce, live.net.match.cookie);
    put_ina(body, destination_ina);
    body += 4u;
    memcpy(body, GUEST_MAGIC, sizeof GUEST_MAGIC);
    body[4] = kind;
    put_ina(body + 5u, live.local_ina);
    memcpy(body + 9u, &guest_port, 2u);
    if (size != 0u) memcpy(body + GUEST_HEADER, data, size);
    netplay_send_raw(&live.net, live.net.match.server_address, live.net.match.server_port, packet,
        MATCH_ASKED + 4u + GUEST_HEADER + size);
}

static void peer_send(LivePeer *peer, uint16_t port, const uint8_t *data, size_t size)
{
    if (peer == NULL || !peer->introduced) return;
    peer->last_ms = netplay_now_ms();
    if (peer->relay) {
        send_relay(peer->virtual_ina, port, GUEST_VERSION, data, size);
        return;
    }
    if (peer->connected) send_wire(peer->address_now, peer->port_now, live.local_ina, port, data, size);
    /* No path yet: the callers queue, and the punch above opens one. */
}

static void peer_queue(LivePeer *peer, uint16_t port, const uint8_t *data, size_t size)
{
    if (peer == NULL || size > LIVE_GUEST_BYTES) return;
    peer->last_ms = netplay_now_ms();
    if (peer->queue_count == LIVE_PEER_QUEUE) { /* keep the newest, as a socket buffer would */
        peer->queue_head = (uint16_t)((peer->queue_head + 1u) % LIVE_PEER_QUEUE);
        --peer->queue_count;
    }
    LiveQueued *slot = &peer->queue[(peer->queue_head + peer->queue_count) % LIVE_PEER_QUEUE];
    slot->port = port;
    slot->size = (uint16_t)size;
    if (size != 0u) memcpy(slot->data, data, size);
    ++peer->queue_count;
}

static void peer_flush(LivePeer *peer)
{
    while (peer->queue_count != 0u) {
        const LiveQueued *slot = &peer->queue[peer->queue_head];
        peer_send(peer, slot->port, slot->data, slot->size);
        peer->queue_head = (uint16_t)((peer->queue_head + 1u) % LIVE_PEER_QUEUE);
        --peer->queue_count;
    }
}

static void peer_relay_on(LivePeer *peer, const char *why)
{
    if (peer->relay) return;
    peer->relay = true;
    fprintf(stderr, "recomp live: %s; relaying through the matchmaker\n", why);
    /* A relayed punch tells the other side to relay too; otherwise it may keep
       sending on a direct path this side never proved. */
    send_relay(peer->virtual_ina, live.guest_port, GUEST_PUNCH, peer->token, RECOMP_NETPLAY_TOKEN_BYTES);
    peer_flush(peer);
}

/* The first datagram to an address without a route asks the backend to pair
   us (CONNECT); the matchmaker then sends the other player's
   LPEER, which recomp_live_transport_peer takes. */
static LivePeer *peer_for_send(uint32_t virtual_ina)
{
    LivePeer *peer = peer_find(virtual_ina);
    if (peer != NULL) return peer;
    peer = peer_claim(virtual_ina, false);
    if (peer == NULL) return NULL;
    peer_ask(peer, peer->last_ms);
    return peer;
}

/* One datagram for the guest's next recvfrom, as peer_packet accepted it. */
static void queue_guest(uint32_t virtual_ina, uint16_t port, const uint8_t *data, size_t size)
{
    if (size > LIVE_GUEST_BYTES) return;
    /* In a session the rollback replaces the game's lockstep, and a type-9
       lockstep datagram would reset its batch and stall state on whichever
       frame it arrived, so the guest never sees one. */
    if (session_frame() != UINT32_MAX && size > 2u && data[2] == 9u) return;
    if (live.guest_count == LIVE_GUEST_QUEUE) {
        live.guest_head = (uint16_t)((live.guest_head + 1u) % LIVE_GUEST_QUEUE);
        --live.guest_count;
    }
    LiveGuestPacket *slot = &live.guest[(live.guest_head + live.guest_count) % LIVE_GUEST_QUEUE];
    slot->virtual_ina = virtual_ina;
    slot->port = port;
    slot->size = (uint16_t)size;
    if (size != 0u) memcpy(slot->data, data, size);
    ++live.guest_count;
}

static bool rollback_packet(const uint8_t *data, size_t size)
{
    if (size < 4u) return false;
    const uint32_t magic = get32(data);
    return magic == MAGIC || magic == GOODBYE_MAGIC;
}

static void queue_rollback(uint32_t virtual_ina, uint16_t port, const uint8_t *data, size_t size)
{
    if (size > RECOMP_NETPLAY_MAX_PACKET) return;
    if (live.rollback_count == LIVE_ROLLBACK_QUEUE) {
        live.rollback_head = (uint16_t)((live.rollback_head + 1u) % LIVE_ROLLBACK_QUEUE);
        --live.rollback_count;
    }
    LiveRollbackPacket *slot = &live.rollback[(live.rollback_head + live.rollback_count) % LIVE_ROLLBACK_QUEUE];
    slot->virtual_ina = virtual_ina;
    slot->port = port;
    slot->size = (uint16_t)size;
    memcpy(slot->data, data, size);
    ++live.rollback_count;
}

/* One guest datagram, from a direct source when from is set, or unwrapped from
   the server's relay when relayed. The header names the sender, but only a
   LPEER introduction grants it a slot: a datagram for an unknown address is
   dropped, and the sender, which keeps punching, gets a slot when the
   matchmaker introduces it.

   A punch is its own kind, with the pair's token as its body. It is the only
   datagram that may set or move the direct path, and it is accepted from any
   source, so a NAT rebinding still finds its way; it never reaches the game.
   Every game datagram on the direct path must come from that path; a relayed
   one is server-stamped and trusted for an introduced peer. */
static void peer_packet(const uint8_t *raw, size_t size, const struct sockaddr_in *from, bool relayed)
{
    if (size < GUEST_HEADER || memcmp(raw, GUEST_MAGIC, sizeof GUEST_MAGIC) != 0 ||
        (raw[4] != GUEST_VERSION && raw[4] != GUEST_PUNCH))
        return;
    uint32_t virtual_ina = (uint32_t)raw[5] << 24 | (uint32_t)raw[6] << 16 | (uint32_t)raw[7] << 8 | raw[8];
    uint16_t port;
    memcpy(&port, raw + 9u, 2u);
    if (virtual_ina == 0u || virtual_ina == live.local_ina) return;
    LivePeer *peer = peer_find(virtual_ina);
    if (peer == NULL || !peer->introduced) return;
    const uint8_t *payload = raw + GUEST_HEADER;
    const size_t payload_size = size - GUEST_HEADER;
    if (raw[4] == GUEST_PUNCH) {
        if (relayed) {
            /* The relayed punch a switch to the relay sends. The server
               vouches for the sender, so its token need not match: it may be
               a re-pairing whose LPEER this side has not got yet. */
            peer_heard(peer);
            peer_relay_on(peer, "the other player relays");
            return;
        }
        const bool answer = payload_size == RECOMP_NETPLAY_TOKEN_BYTES + 1u &&
            payload[RECOMP_NETPLAY_TOKEN_BYTES] == 1u;
        if ((payload_size != RECOMP_NETPLAY_TOKEN_BYTES && !answer) ||
            memcmp(payload, peer->token, RECOMP_NETPLAY_TOKEN_BYTES) != 0)
            return; /* another pairing's punch proves nothing */
        /* It is this pairing's: only now is the peer seen to be alive. */
        peer_heard(peer);
        /* Answer a punch, not an answer: the other side may have been
           introduced after its own punches arrived here and were dropped. */
        if (!answer && from != NULL) send_punch(from->sin_addr.s_addr, from->sin_port, peer->token, true);
        if (from != NULL &&
            (!peer->connected || peer->address_now != from->sin_addr.s_addr ||
                peer->port_now != from->sin_port)) {
            if (!peer->connected) fprintf(stderr, "recomp live: reached the other player directly\n");
            peer->connected = true;
            peer->address_now = from->sin_addr.s_addr;
            peer->port_now = from->sin_port;
        }
        return;
    }
    if (payload_size == 0u) return; /* an empty game datagram carries nothing */
    if (relayed) {
        peer_heard(peer);
        peer_relay_on(peer, "the other player relays");
    } else if (from == NULL || !peer->connected || peer->address_now != from->sin_addr.s_addr ||
        peer->port_now != from->sin_port) {
        return; /* not the path a checked punch made */
    } else {
        peer_heard(peer); /* activity on the peer's own path */
    }
    /* A session's packets belong to the bound session, and to no one else: a goodbye
       can arrive just after one side ended and unbound, and the game must not read it
       as one of its own datagrams. */
    if (rollback_packet(payload, payload_size)) {
        if (live.rollback_bound && virtual_ina == live.rollback_ina)
            queue_rollback(virtual_ina, port, payload, payload_size);
        return;
    }
    queue_guest(virtual_ina, port, payload, payload_size);
}

/* The matchmaker's answer: a cookie to store and use again, a relayed packet
   from a peer, or one for the Live client (live_client.c) to decode. */
static void server_packet(const uint8_t *raw, size_t size)
{
    if (size < MATCH_HEADER + RECOMP_NETPLAY_TOKEN_BYTES || memcmp(raw, MATCH_MAGIC, sizeof MATCH_MAGIC) != 0 ||
        raw[4] != MATCH_VERSION)
        return;
    /* Anyone can send from the server's address; only the server knows this
       socket's nonce, so a forged answer is dropped. */
    if (memcmp(raw + MATCH_HEADER, live.net.match.nonce, RECOMP_NETPLAY_TOKEN_BYTES) != 0) return;
    if (raw[5] == MATCH_COOKIE) {
        if (size != MATCH_ASKED) return;
        memcpy(live.net.match.cookie, raw + MATCH_BODY, RECOMP_NETPLAY_TOKEN_BYTES);
        /* The server dropped a recent request for want of this cookie, or took
           it and sent a newer one; say the last request again with it. */
        if (live.asked_size != 0u && netplay_now_ms() - live.asked_ms < 2000u) {
            uint8_t again[sizeof live.asked];
            memcpy(again, live.asked, live.asked_size);
            memcpy(again + MATCH_HEADER, live.net.match.nonce, RECOMP_NETPLAY_TOKEN_BYTES);
            memcpy(again + MATCH_BODY, live.net.match.cookie, RECOMP_NETPLAY_TOKEN_BYTES);
            netplay_send_raw(&live.net, live.net.match.server_address, live.net.match.server_port, again,
                live.asked_size);
        }
        live.asked_size = 0u; /* once */
        return;
    }
    if (raw[5] == MATCH_RELAY) {
        /* The server forwards header, the destination's nonce, then the guest
           datagram: the cookie is a request field, so the body starts at
           MATCH_BODY. */
        if (size > MATCH_BODY) peer_packet(raw + MATCH_BODY, size - MATCH_BODY, NULL, true);
        return;
    }
    if (live.handler != NULL) live.handler(raw, size);
}

static void receive_all(void)
{
    /* Room for a relayed datagram, the largest thing the server forwards. */
    uint8_t raw[MATCH_ASKED + GUEST_HEADER + LIVE_GUEST_BYTES + 64u];
    for (;;) {
        struct sockaddr_in from;
        SocketLength from_size = sizeof from;
        const int size = (int)recvfrom((SOCKET)live.net.socket, (char *)raw, (int)sizeof raw, 0,
            (struct sockaddr *)&from, &from_size);
        if (size < 0) {
#ifdef _WIN32
            /* Windows reports an earlier send's ICMP port-unreachable as a
               failed receive, and a too-long datagram as a failure. */
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue;
#endif
            break;
        }
        if (from.sin_family != AF_INET) continue;
        if (live.net.match.server_port != 0u && from.sin_addr.s_addr == live.net.match.server_address &&
            from.sin_port == live.net.match.server_port) {
            server_packet(raw, (size_t)size);
            continue;
        }
        peer_packet(raw, (size_t)size, &from, false);
    }
}

/* Punching, the relay fallback after 5 s, and the datagrams that waited for a
   path. */
static void peer_tick(LivePeer *peer, uint64_t now)
{
    /* The game sent and nothing checked has come back for LIVE_SILENT_MS: the
       other player may have restarted elsewhere, or an introduction was lost.
       Ask again, at most that often and only while the game still sends; a
       changed LPEER resets the route, the same one changes nothing, and the
       path in use stays until then. A quiet peer the game is not sending to is
       never asked for. */
    if (peer->waiting && now > peer->waiting_ms + LIVE_SILENT_MS && peer->sent_ms >= peer->asked_ms &&
        now > peer->asked_ms + LIVE_SILENT_MS)
        peer_ask(peer, now);
    if (!peer->introduced) return;
    if (peer->relay || peer->connected) {
        peer_flush(peer);
        return;
    }
    if (peer->port[0] == 0u && peer->port[1] == 0u) {
        peer_relay_on(peer, "matched without endpoints");
        return;
    }
    if (now > peer->punch_start + LIVE_PUNCH_GIVE_UP_MS) {
        peer_relay_on(peer, "no direct path after 5 s");
        return;
    }
    if (now < peer->next_punch) return;
    peer->next_punch = now + LIVE_PUNCH_MS;
    /* Sending first opens this side's router for the answer. The punch carries
       the pair's token, so the peer takes only a punch from this pairing as its
       path, and the game datagrams that waited go out once it answers. */
    for (unsigned i = 0u; i < 2u; ++i)
        if (peer->port[i] != 0u) send_punch(peer->address[i], peer->port[i], peer->token, false);
}

/* Every LIVE_LOG_MS: each peer's game datagrams by type both ways, or once
   that a peer has gone quiet (not while a rollback session carries the fight). */
static void log_peers(uint64_t now)
{
    if (live.log_ms == 0u) live.log_ms = now;
    if (now < live.log_ms + LIVE_LOG_MS) return;
    live.log_ms = now;
    for (unsigned i = 0u; i < LIVE_PEERS; ++i) {
        LivePeer *peer = &live.peers[i];
        if (!peer->used) continue;
        char ina[16], heard[32];
        ina_text(peer->virtual_ina, ina);
        if (peer->heard) snprintf(heard, sizeof heard, "heard %.1f s ago", (double)(now - peer->heard_ms) / 1000.0);
        else snprintf(heard, sizeof heard, "never heard");
        const char *path = peer->relay ? "relay" : peer->connected ? "direct" : "no path";
        uint32_t traffic = 0u;
        for (unsigned t = 0u; t < LIVE_LOG_TYPES; ++t) traffic += peer->sent_types[t] + peer->got_types[t];
        if (traffic != 0u) {
            char sent[256], got[256];
            recomp_live_format_type_counts(sent, sizeof sent, peer->sent_types, LIVE_LOG_TYPES);
            recomp_live_format_type_counts(got, sizeof got, peer->got_types, LIVE_LOG_TYPES);
            fprintf(stderr, "recomp live: %s, game datagrams in %u s: sent %s; got %s (%s, %s)\n", ina,
                (unsigned)(LIVE_LOG_MS / 1000u), sent, got, path, heard);
            memset(peer->sent_types, 0, sizeof peer->sent_types);
            memset(peer->got_types, 0, sizeof peer->got_types);
            peer->quiet_said = false;
        } else if (!peer->quiet_said && !(live.rollback_bound && peer->virtual_ina == live.rollback_ina)) {
            fprintf(stderr, "recomp live: %s, no game datagrams either way in %u s (%s, %s)\n", ina,
                (unsigned)(LIVE_LOG_MS / 1000u), path, heard);
            peer->quiet_said = true;
        }
    }
}

void recomp_live_transport_pump(uint64_t now_ms)
{
    if (!live.open) return;
    receive_all();
    for (unsigned i = 0u; i < LIVE_PEERS; ++i)
        if (live.peers[i].used) peer_tick(&live.peers[i], now_ms);
    log_peers(now_ms);
}

bool recomp_live_transport_open(const char *server_host, uint16_t server_port)
{
    /* The matchmaker handler and this player's virtual address are the
       caller's settings, not socket state: a caller may register them before
       it opens, so opening keeps them. */
    const RecompLiveServerHandler handler = live.handler;
    const uint32_t local_ina = live.local_ina;
    recomp_live_transport_close();
    live.handler = handler;
    live.local_ina = local_ina;
    if (server_host == NULL || server_port == 0u || strlen(server_host) >= sizeof live.server_host) return false;
    memcpy(live.server_host, server_host, strlen(server_host) + 1u);
    live.server_port = server_port;
    live.started = true;
    live.guest_port = htons(LIVE_GUEST_PORT);
    live.replay_frame = UINT32_MAX;
    return true;
}

static RecompLiveResolver resolver = recomp_netplay_resolve;

void recomp_live_transport_set_resolver(RecompLiveResolver replacement)
{
    resolver = replacement != NULL ? replacement : recomp_netplay_resolve;
}

/* Moves a lookup on from running, or reads its state with to = LOOKUP_RUNNING.
   Returns the state it had. */
static int lookup_leave(LiveLookup *lookup, int to)
{
#ifdef _WIN32
    return (int)InterlockedCompareExchange(&lookup->state, to, LOOKUP_RUNNING);
#else
    int was = LOOKUP_RUNNING;
    atomic_compare_exchange_strong(&lookup->state, &was, to);
    return was;
#endif
}

#ifdef _WIN32
static DWORD WINAPI lookup_thread(void *context)
#else
static void *lookup_thread(void *context)
#endif
{
    LiveLookup *lookup = context;
    const bool found = lookup->resolve(lookup->host, &lookup->address);
    /* The transport closed meanwhile: nobody else will free it. */
    if (lookup_leave(lookup, found ? LOOKUP_FOUND : LOOKUP_FAILED) == LOOKUP_DROPPED) free(lookup);
    return 0;
}

static LiveLookup *lookup_start(const char *host)
{
    LiveLookup *lookup = calloc(1u, sizeof *lookup); /* zeroed: LOOKUP_RUNNING */
    if (lookup == NULL) return NULL;
    memcpy(lookup->host, host, strlen(host) + 1u);
    lookup->resolve = resolver;
#ifdef _WIN32
    const HANDLE thread = CreateThread(NULL, 0u, lookup_thread, lookup, 0u, NULL);
    if (thread != NULL) {
        CloseHandle(thread);
        return lookup;
    }
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, lookup_thread, lookup) == 0) {
        pthread_detach(thread);
        return lookup;
    }
#endif
    free(lookup);
    return NULL;
}

/* An address needs no lookup. */
static bool literal_address(const char *host, uint32_t *address)
{
    unsigned part[4];
    uint8_t bytes[4];
    char rest;
    if (sscanf(host, "%3u.%3u.%3u.%3u%c", &part[0], &part[1], &part[2], &part[3], &rest) != 4) return false;
    for (unsigned i = 0u; i < 4u; ++i) {
        if (part[i] > 255u) return false;
        bytes[i] = (uint8_t)part[i];
    }
    memcpy(address, bytes, sizeof bytes);
    return true;
}

/* Looks the server up and opens the socket from the first request on, so a
   player who never logs on sends no lookup and meets no firewall prompt. The
   lookup never holds a request: it runs on its own thread, and the request
   that finds it done opens the socket and goes out, which is the client's
   resend 500 ms later. A failure is tried again 5 s after it began. */
static bool live_socket_up(void)
{
    if (live.open) return true;
    if (!live.started) return false;
    uint32_t address = 0u;
    if (live.lookup != NULL) {
        const int state = lookup_leave(live.lookup, LOOKUP_RUNNING);
        if (state == LOOKUP_RUNNING) return false;
        address = live.lookup->address;
        free(live.lookup);
        live.lookup = NULL;
        if (state != LOOKUP_FOUND) {
            fprintf(stderr, "recomp live: cannot resolve the matchmaker %s\n", live.server_host);
            return false;
        }
    } else {
        const uint64_t now = netplay_now_ms();
        if (now < live.retry_ms) return false;
        live.retry_ms = now + 5000u;
        if (!literal_address(live.server_host, &address)) {
            live.lookup = lookup_start(live.server_host);
            if (live.lookup == NULL) fprintf(stderr, "recomp live: cannot start the matchmaker lookup\n");
            return false;
        }
    }
    if (!recomp_netplay_listen(&live.net, 0u)) {
        fprintf(stderr, "recomp live: cannot open the socket\n");
        return false;
    }
    live.net.match.server_address = address;
    live.net.match.server_port = htons(live.server_port);
    live.open = true;
    return true;
}

void recomp_live_transport_close(void)
{
    recomp_live_transport_unbind_rollback();
    if (live.open) recomp_netplay_close(&live.net);
    /* A lookup still running is left to its thread, which frees it. */
    if (live.lookup != NULL && lookup_leave(live.lookup, LOOKUP_DROPPED) != LOOKUP_RUNNING) free(live.lookup);
    memset(&live, 0, sizeof live);
}

void recomp_live_transport_set_server_handler(RecompLiveServerHandler handler)
{
    live.handler = handler;
}

/* packet is a whole "D2MM" request: magic, version and kind, then place for
   the nonce and the cookie, then the body. The transport writes its socket's
   nonce and the server's cookie, and keeps the request to send again when a
   new cookie answers it. */
void recomp_live_transport_send_server(const uint8_t *packet, size_t size)
{
    if (packet == NULL || !live_socket_up()) return;
    if (size < MATCH_ASKED || size > LIVE_MATCH_BYTES || size > sizeof live.asked) return;
    if (memcmp(packet, MATCH_MAGIC, sizeof MATCH_MAGIC) != 0 || packet[4] != MATCH_VERSION) return;
    memcpy(live.asked, packet, size);
    memcpy(live.asked + MATCH_HEADER, live.net.match.nonce, RECOMP_NETPLAY_TOKEN_BYTES);
    memcpy(live.asked + MATCH_BODY, live.net.match.cookie, RECOMP_NETPLAY_TOKEN_BYTES);
    live.asked_size = size;
    live.asked_ms = netplay_now_ms();
    netplay_send_raw(&live.net, live.net.match.server_address, live.net.match.server_port, live.asked, size);
}

void recomp_live_transport_nonce(uint8_t nonce[8])
{
    memcpy(nonce, live.net.match.nonce, RECOMP_NETPLAY_TOKEN_BYTES);
}

void recomp_live_transport_peer(const RecompLivePeerIntro *intro)
{
    if (!live.open || intro == NULL || intro->virtual_ina == 0u || intro->virtual_ina == live.local_ina) return;
    LivePeer *peer = peer_claim(intro->virtual_ina, true);
    if (peer == NULL) return;
    /* The same introduction again says nothing new, and it must not restart a
       punch that is already under way. */
    if (peer->introduced && peer->address[0] == intro->public_address &&
        peer->port[0] == intro->public_port && peer->address[1] == intro->lan_address &&
        peer->port[1] == intro->lan_port && memcmp(peer->token, intro->token, sizeof peer->token) == 0 &&
        peer->relay == intro->relay)
        return;
    const bool known = peer->introduced;
    const bool moved = known && (peer->address[0] != intro->public_address ||
        peer->port[0] != intro->public_port || peer->address[1] != intro->lan_address ||
        peer->port[1] != intro->lan_port || memcmp(peer->token, intro->token, sizeof peer->token) != 0);
    const uint64_t now = netplay_now_ms();
    peer->introduced = true;
    peer->address[0] = intro->public_address;
    peer->port[0] = intro->public_port;
    peer->address[1] = intro->lan_address;
    peer->port[1] = intro->lan_port;
    memcpy(peer->token, intro->token, sizeof peer->token);
    peer->last_ms = now;
    if (moved) {
        /* Another pairing, or other endpoints: the old route is dead, so forget
           it and let punching or the relay start over. */
        peer->heard = false;
        peer->connected = false;
        peer->relay = false;
        peer->address_now = 0u;
        peer->port_now = 0u;
    }
    if (moved || !known) {
        peer->punch_start = now;
        peer->next_punch = 0u; /* the next pump sends the first punch */
        char ina[16];
        ina_text(intro->virtual_ina, ina);
        fprintf(stderr, "recomp live: the matchmaker %s %s\n", moved ? "moved" : "introduced", ina);
    }
    if (intro->relay) peer_relay_on(peer, "the matchmaker paired us on the relay");
    /* The datagrams that waited for a route go out once the peer answers the
       punch; an unchanged introduction keeps what the old path flushed. */
}

/* 0 forgets every peer, as a logoff does. */
void recomp_live_transport_forget(uint32_t virtual_ina)
{
    if (virtual_ina == 0u) {
        unsigned forgot = 0u;
        for (unsigned i = 0u; i < LIVE_PEERS; ++i)
            if (live.peers[i].used) {
                peer_clear(&live.peers[i]);
                ++forgot;
            }
        live.rollback_head = 0u;
        live.rollback_count = 0u;
        if (forgot != 0u) fprintf(stderr, "recomp live: forgot all %u players\n", forgot);
        return;
    }
    LivePeer *peer = peer_find(virtual_ina);
    if (peer != NULL) {
        char ina[16];
        ina_text(virtual_ina, ina);
        fprintf(stderr, "recomp live: forgot %s\n", ina);
        peer_clear(peer);
    }
}

bool recomp_live_transport_reachable(uint32_t virtual_ina)
{
    const LivePeer *peer = peer_find(virtual_ina);
    return peer != NULL && (peer->relay || peer->connected);
}

bool recomp_live_transport_heard(uint32_t virtual_ina)
{
    const LivePeer *peer = peer_find(virtual_ina);
    return peer != NULL && peer->introduced && peer->heard;
}

bool recomp_live_transport_relay(uint32_t virtual_ina)
{
    const LivePeer *peer = peer_find(virtual_ina);
    return peer != NULL && peer->relay;
}

void recomp_live_transport_set_local(uint32_t virtual_ina)
{
    live.local_ina = virtual_ina;
}

int recomp_live_transport_guest_send(uint32_t virtual_ina, uint16_t port, const uint8_t *data, size_t size)
{
    if (!live.open || virtual_ina == 0u) return -1;
    if (size > LIVE_GUEST_BYTES || (size != 0u && data == NULL)) return -1;
    /* The frame ran once and sent this already; its effects were rewound with
       the rest of guest RAM, so say it went out. */
    if (session_frame() != UINT32_MAX && replaying()) return (int)size;
    LivePeer *peer = peer_for_send(virtual_ina);
    if (peer == NULL) return -1;
    ++peer->sent_types[log_type(data, size)];
    peer->sent_ms = netplay_now_ms();
    if (!peer->waiting) {
        peer->waiting = true;
        peer->waiting_ms = peer->sent_ms;
    }
    if (peer->relay && size > LIVE_RELAY_BYTES) return -2; /* the relay cannot carry it */
    if (!peer->relay && !peer->connected) {
        /* No path yet: the datagram waits until the peer answers a punch. */
        peer_queue(peer, port, data, size);
        return (int)size;
    }
    peer_send(peer, port, data, size);
    return (int)size;
}

/* A frame run again after a rewind receives the datagrams its first run did,
   in the same order. */
static int replay_frame(uint32_t frame, uint8_t *data, size_t capacity, uint32_t *virtual_ina, uint16_t *port)
{
    if (frame != live.replay_frame) {
        live.replay_frame = frame;
        live.replay_at = 0u;
    }
    uint32_t seen = 0u;
    for (unsigned i = 0u; i < LIVE_RECV_LOG; ++i) {
        const uint32_t slot = (live.log_head + i) % LIVE_RECV_LOG;
        const LiveLogEntry *entry = &live.log[slot];
        if (entry->frame != frame || entry->size == 0u) continue;
        if (seen++ < live.replay_at) continue;
        ++live.replay_at;
        const size_t copied = entry->size < capacity ? entry->size : capacity;
        if (copied != 0u) memcpy(data, entry->data, copied);
        if (virtual_ina != NULL) *virtual_ina = entry->virtual_ina;
        if (port != NULL) *port = entry->port;
        return (int)copied;
    }
    return -1;
}

int recomp_live_transport_guest_receive(uint8_t *data, size_t capacity, uint32_t *virtual_ina, uint16_t *port)
{
    if (!live.open) return -1;
    const uint32_t frame = session_frame();
    if (frame != UINT32_MAX && replaying()) return replay_frame(frame, data, capacity, virtual_ina, port);
    /* The pump runs here too, so a recvfrom-only frame still hears the socket. */
    recomp_live_transport_pump(netplay_now_ms());
    if (live.guest_count == 0u) return -1;
    const LiveGuestPacket *slot = &live.guest[live.guest_head];
    const size_t size = slot->size;
    const size_t copied = size < capacity ? size : capacity;
    if (copied != 0u) memcpy(data, slot->data, copied);
    if (virtual_ina != NULL) *virtual_ina = slot->virtual_ina;
    if (port != NULL) *port = slot->port;
    if (frame != UINT32_MAX) {
        LiveLogEntry *entry = &live.log[live.log_head++ % LIVE_RECV_LOG];
        entry->frame = frame;
        entry->virtual_ina = slot->virtual_ina;
        entry->port = slot->port;
        entry->size = slot->size;
        if (size != 0u) memcpy(entry->data, slot->data, size);
    }
    live.guest_head = (uint16_t)((live.guest_head + 1u) % LIVE_GUEST_QUEUE);
    --live.guest_count;
    return (int)copied;
}

/* The bound session's sends go to the opponent as guest datagrams, and its
   receives come from the queue peer_packet filled. */
static void rollback_send(const RecompNetplay *net, const uint8_t *packet, size_t size)
{
    (void)net;
    if (!live.rollback_bound || size > RECOMP_NETPLAY_MAX_PACKET) return;
    LivePeer *peer = peer_for_send(live.rollback_ina);
    if (peer == NULL) return;
    if (!peer->relay && !peer->connected) {
        peer_queue(peer, live.guest_port, packet, size);
        return;
    }
    peer_send(peer, live.guest_port, packet, size);
}

static int rollback_receive(RecompNetplay *net, uint8_t *packet, size_t capacity, struct sockaddr_in *from)
{
    (void)net;
    /* The session's wait loops do not return to the runner's frame, so its
       receives read the socket too, as the guest's recvfrom does. */
    recomp_live_transport_pump(netplay_now_ms());
    if (!live.rollback_bound || live.rollback_count == 0u) return -1;
    const LiveRollbackPacket *slot = &live.rollback[live.rollback_head];
    const size_t size = slot->size < capacity ? slot->size : capacity;
    memcpy(packet, slot->data, size);
    /* netplay_from_peer compares the source to the bound opponent, so the
       source is that, whatever port the sender's own socket had. */
    memset(from, 0, sizeof *from);
    from->sin_family = AF_INET;
    from->sin_addr.s_addr = live.rollback_ina;
    from->sin_port = htons(LIVE_GUEST_PORT);
    live.rollback_head = (uint16_t)((live.rollback_head + 1u) % LIVE_ROLLBACK_QUEUE);
    --live.rollback_count;
    return (int)size;
}

static const RecompNetplayRoute ROLLBACK_ROUTE = {rollback_send, rollback_receive};

bool recomp_live_transport_bind_rollback(uint32_t opponent_ina, RecompNetplay *net)
{
    recomp_live_transport_unbind_rollback();
    if (net == NULL || opponent_ina == 0u) return false;
    if (!netplay_set_route(net, &ROLLBACK_ROUTE)) return false;
    /* What netplay_from_peer and the session's logs compare against, so its
       existing checks hold unchanged on the new path. */
    net->peer_address = opponent_ina;
    net->peer_port = htons(LIVE_GUEST_PORT);
    live.rollback_bound = true;
    live.rollback_ina = opponent_ina;
    live.rollback_net = net;
    live.rollback_head = 0u;
    live.rollback_count = 0u;
    return true;
}

void recomp_live_transport_unbind_rollback(void)
{
    if (!live.rollback_bound) return;
    (void)netplay_set_route(live.rollback_net, NULL);
    live.rollback_bound = false;
    live.rollback_ina = 0u;
    live.rollback_net = NULL;
    live.rollback_head = 0u;
    live.rollback_count = 0u;
}

/* A new session counts frames from 0 again, so the last session's datagrams
   must not replay in it (stale entries broke rematches). */
static void clear_log(void)
{
    memset(live.log, 0, sizeof live.log);
    live.log_head = 0u;
    live.replay_frame = UINT32_MAX;
    live.replay_at = 0u;
}

void recomp_live_transport_session_begin(void)
{
    clear_log();
}

void recomp_live_transport_session_end(void)
{
    clear_log();
}

/* --- what the game may receive ---

   The game's receive loop (online_receive_datagrams, 0x2a4b90) trusts its
   peers as Xbox Live's signed consoles were trusted: no handler is told the
   datagram's length, counts and string lengths come from the datagram, and
   type 15 calls through a table indexed by it. So only datagrams shaped as the
   game's own senders build them reach it. Offsets are from the datagram's
   start; byte 2 is the type and the body starts at 3. */
enum {
    GAME_RECV_BYTES = 0x329,  /* the game's recvfrom buffer */
    GAME_PLAYERS = 8,         /* g_session_players' slots */
    GAME_PLAYER_BYTES = 0x5e, /* one slot: XNADDR, XUID, name, wide name */
    GAME_MSG0F_HANDLERS = 12, /* g_online_msg0f_handlers (0x7bfacc) */
};

/* A NUL within bytes [at, at + size): lstrcpyA copies until one. */
static bool game_text_ends(const uint8_t *datagram, size_t at, size_t size)
{
    return memchr(datagram + at, 0, size) != NULL;
}

/* A wide NUL within units [at, at + 2 * units): lstrcpyW copies until one. */
static bool game_wide_ends(const uint8_t *datagram, size_t at, size_t units)
{
    for (size_t i = 0u; i < units; ++i)
        if (datagram[at + 2u * i] == 0u && datagram[at + 2u * i + 1u] == 0u) return true;
    return false;
}

/* A player list: a count byte at count_at, then that many slots from
   players_at to the datagram's end. session_load_player_list copies count
   slots into the 8 of g_session_players, and recent_player_add copies each
   slot's names unbounded. */
static bool game_players_ok(const uint8_t *datagram, size_t size, size_t count_at, size_t players_at)
{
    if (size < players_at) return false;
    const size_t count = datagram[count_at];
    if (count > GAME_PLAYERS || size != players_at + count * GAME_PLAYER_BYTES) return false;
    for (size_t i = 0u; i < count; ++i) {
        const size_t slot = players_at + i * GAME_PLAYER_BYTES;
        if (!game_text_ends(datagram, slot + 0x2cu, 16u) || !game_wide_ends(datagram, slot + 0x3cu, 16u))
            return false;
    }
    return true;
}

static uint16_t game_u16(const uint8_t *at) { return (uint16_t)(at[0] | at[1] << 8); }
static uint32_t game_u32(const uint8_t *at) { return (uint32_t)game_u16(at) | (uint32_t)game_u16(at + 2u) << 16; }

/* A type-16 record's size by subtype, as online_msg10_record_size (0x294bb0)
   gives it to the sender. online_receive_msg10 (0x2999f0) never reads the
   length; it reads the subtype's fields, and 0x294c40 copies 4's and 5's
   whole, so a shorter record would be filled from the last datagram. */
static uint32_t game_msg10_record_bytes(uint32_t subtype)
{
    switch (subtype) {
    case 2u: return 0x1du;
    case 3u: return 0x21u;
    case 4u: return 0x170u;
    case 5u: return 0x19du;
    case 0u: case 1u: case 6u: case 7u: case 8u: case 9u: case 10u: return 0x19u;
    default: return 0x18u;
    }
}

bool recomp_live_game_datagram_ok(const uint8_t *datagram, size_t size)
{
    if (datagram == NULL || size < 3u || size > GAME_RECV_BYTES) return false;
    switch (datagram[2]) {
    case 0u: /* a join (online_session_join): name, wide name, XUID */
    case 1u:
        return size == 0x3cu && game_text_ends(datagram, 3u, 16u) && game_wide_ends(datagram, 0x13u, 16u);
    case 2u: /* the joined session: its settings record (type 14's), then the players */
        return size >= 0x39u && game_wide_ends(datagram, 3u, 17u) && game_players_ok(datagram, size, 0x34u, 0x39u);
    case 3u: case 5u: case 11u: case 17u: /* no body */
        return size == 3u;
    case 6u: /* a XUID */
        return size == 11u;
    case 7u: /* a count, then that many 10-byte records */
        return size >= 4u && size == 4u + datagram[3] * 10u;
    case 8u:
        return size == 20u;
    case 9u: { /* lockstep: id, batch, then batch_frames 20-byte pads */
        if (size < 0xfu + 20u || (size - 0xfu) % 20u != 0u || (size - 0xfu) / 20u > 4u) return false;
        /* The batch that starts lockstep carries the frames per batch and the
           delay, which online_lockstep_receive takes as they come and copies
           frames * 20 bytes into an 84-byte local. The host clamps them to
           1-4 and 2-7 (0x293a90, 0x293a60). */
        if (game_u32(datagram + 3u + 8u) != 0xffffffffu) return true;
        const uint8_t frames = datagram[3u + 0x10u], delay = datagram[3u + 0x11u];
        return frames >= 1u && frames <= 4u && delay >= 2u && delay <= 7u;
    }
    case 10u: /* the player list */
        return game_players_ok(datagram, size, 3u, 8u);
    case 12u: /* the session key, then the player list */
        return game_players_ok(datagram, size, 0x1bu, 0x20u);
    case 13u: /* a slot, which the game checks */
        return size == 4u;
    case 14u: /* the settings record: its wide name is lstrcpyW'd into 17 units */
        return size == 0x34u && game_wide_ends(datagram, 3u, 17u);
    case 15u: { /* a handler index, ..., the payload's length at 0x13, the payload */
        if (size < 0x15u) return false;
        const uint32_t handler = game_u32(datagram + 3u);
        const uint16_t payload = game_u16(datagram + 0x13u);
        if (handler >= GAME_MSG0F_HANDLERS || size != 0x15u + payload) return false;
        /* Character select: 0x130f90 and 0x130ef0 copy a 0x14-byte record
           without its length. The empty reply a sender makes before it has
           chosen (0x130d30, 0x130e10) would copy the last datagram's bytes
           whenever the id it names, which the peer sets, passes their check.
           The game asks again every second (0x133539) until it has a record;
           recomp_live_game_datagram_expected_drop tells these apart. */
        if (handler <= 3u) return payload == 0x14u;
        /* Match settings: controller settings, 0x42 bytes, then the stage's
           own state, which 0x268320 copies as payload - 0x54 bytes; the
           largest a stage sends is 0x1d (0x268d00). An empty one would make
           that copy 0xffac bytes. */
        if (handler == 4u) return payload >= 0x54u && payload <= 0x54u + 0x1du;
        /* Controller settings: 0xcffc0 reads a fixed 0x16 bytes, so the empty
           not-ready reply (0xd00b0) would apply stale ones. Asked again
           every second (0xd1874). */
        if (handler == 5u) return payload == 0x16u;
        return true; /* requests: their handlers write the reply */
    }
    case 16u: { /* a record from byte 3: its subtype at 0x13, its own length at 0x17 */
        if (size < 3u + 0x18u) return false;
        const uint32_t record = game_u32(datagram + 0x17u);
        return record == size - 3u && record == game_msg10_record_bytes(game_u32(datagram + 0x13u) & 0x7fffffffu);
    }
    case 19u:
        return size == 0x78u;
    case 20u:
        return size == 200u;
    case 21u: /* a recent player's record: a name, a wide name */
        return size == 0x60u && game_text_ends(datagram, 0x0bu, 0x27u) && game_wide_ends(datagram, 0x32u, 16u);
    default: /* 4, 18 and above 21 have no handler */
        return false;
    }
}

bool recomp_live_game_datagram_expected_drop(const uint8_t *datagram, size_t size)
{
    if (datagram == NULL || size != 0x15u || datagram[2] != 15u || game_u16(datagram + 0x13u) != 0u) return false;
    const uint32_t handler = game_u32(datagram + 3u);
    return handler <= 3u || handler == 5u;
}

/* --- the guest's winsock and XNet transport thunks --- */

/* socket(af, type, protocol): the game's one UDP socket, carried on the
   transport's, which opens with the first matchmaker request. */
static void live_socket(void)
{
    kernel_return(3u, 0x100u);
}

/* bind(s, name, namelen): keeps the guest's port for its XNADDR and packets.
   The host socket's port is the one the matchmaker sees. */
static void live_bind(void)
{
    const uint32_t name = kernel_arg(2u);
    if (name != 0u) memcpy(&live.guest_port, recomp_memory(name + 2u, 2u), 2u);
    kernel_return(3u, 0u);
}

/* ioctlsocket(s, cmd, argp): the socket is already non-blocking. */
static void live_ioctlsocket(void)
{
    kernel_return(3u, 0u);
}

static void live_closesocket(void)
{
    kernel_return(1u, 0u);
}

static void live_shutdown(void)
{
    kernel_return(2u, 0u);
}

static void live_wsa_last_error(void)
{
    kernel_return(0u, live.last_error);
}

/* sendto(s, buf, len, flags, to, tolen): the guest's sockaddr_in names the
   other player's virtual address on the game's port. */
static void live_sendto(void)
{
    const uint32_t buffer = kernel_arg(2u), size = kernel_arg(3u), to = kernel_arg(5u);
    if (to == 0u || size > LIVE_GUEST_BYTES) {
        live.last_error = LIVE_WSAEMSGSIZE;
        kernel_return(6u, 0xffffffffu);
        return;
    }
    const uint8_t *name = recomp_memory(to, 8u);
    uint32_t virtual_ina;
    uint16_t port;
    memcpy(&port, name + 2u, 2u);
    memcpy(&virtual_ina, name + 4u, 4u);
    const int sent = recomp_live_transport_guest_send(virtual_ina, port,
        size != 0u ? recomp_memory(buffer, size) : NULL, size);
    if (sent < 0) {
        live.last_error = sent == -2 ? LIVE_WSAEMSGSIZE : LIVE_WSAEHOSTUNREACH;
        kernel_return(6u, 0xffffffffu);
        return;
    }
    kernel_return(6u, (uint32_t)sent);
}

/* recvfrom(s, buf, len, flags, from, fromlen): never blocks; WSAEWOULDBLOCK
   when nothing is queued. The guest's source is the virtual address and port
   the datagram's header carried. */
static void live_recvfrom(void)
{
    const uint32_t buffer = kernel_arg(2u), length = kernel_arg(3u);
    const uint32_t from = kernel_arg(5u), from_length = kernel_arg(6u);
    uint8_t data[LIVE_GUEST_BYTES];
    uint32_t virtual_ina = 0u;
    uint16_t port = 0u;
    int size;
    /* A frame run again after a rewind receives the same datagrams, so it
       drops the same ones. */
    while ((size = recomp_live_transport_guest_receive(data, sizeof data, &virtual_ina, &port)) >= 0 &&
        !recomp_live_game_datagram_ok(data, (size_t)size)) {
        /* The game's own empty replies come every second; keep the line for real rejections. */
        if (recomp_live_game_datagram_expected_drop(data, (size_t)size)) continue;
        static uint32_t said; /* one line per type */
        const unsigned type = size >= 3 ? data[2] : 0u;
        if (type < 32u && (said & 1u << type) == 0u) {
            said |= 1u << type;
            fprintf(stderr, "recomp live: dropped a malformed type-%u game datagram (%d bytes)\n", type, size);
        }
    }
    if (size < 0) {
        live.last_error = LIVE_WSAEWOULDBLOCK;
        kernel_return(6u, 0xffffffffu);
        return;
    }
    if (!replaying()) {
        LivePeer *peer = peer_find(virtual_ina);
        if (peer != NULL) ++peer->got_types[log_type(data, (size_t)size)];
    }
    const size_t copied = (size_t)size < length ? (size_t)size : length;
    if (copied != 0u) memcpy(recomp_memory(buffer, copied), data, copied);
    if (from != 0u) {
        uint8_t *guest = recomp_memory(from, 16u);
        memset(guest, 0, 16u);
        guest[0] = 2u; /* AF_INET */
        memcpy(guest + 2u, &port, 2u);
        memcpy(guest + 4u, &virtual_ina, 4u);
    }
    if (from_length != 0u) *recomp_memory_u32(from_length) = 16u;
    kernel_return(6u, (uint32_t)copied);
}

/* XNetXnAddrToInAddr(xnaddr, kid, inaddr): the virtual address stands in for
   both ina and inaOnline, as the matchmaker gave it at logon. */
static void live_xnaddr_to_inaddr(void)
{
    const uint32_t xnaddr = kernel_arg(1u), inaddr = kernel_arg(3u);
    (void)kernel_arg(2u);
    if (xnaddr != 0u && inaddr != 0u) memcpy(recomp_memory(inaddr, 4u), recomp_memory(xnaddr, 4u), 4u);
    kernel_return(3u, 0u);
}

/* XNetInAddrToXnAddr(ina, xnaddr, kid): an XNADDR for a virtual address, whose
   MAC comes from the address since the XUID is not known here. */
static void live_inaddr_to_xnaddr(void)
{
    const uint32_t inaddr = kernel_arg(1u), xnaddr = kernel_arg(2u), kid = kernel_arg(3u);
    if (xnaddr != 0u) {
        uint8_t *out = recomp_memory(xnaddr, 0x24u);
        uint8_t ina[4];
        memcpy(ina, &inaddr, 4u);
        memset(out, 0, 0x24u);
        memcpy(out, ina, 4u);
        memcpy(out + 4u, ina, 4u);
        out[10] = 0x00u; /* 00:50:F2, the Xbox NIC's prefix */
        out[11] = 0x50u;
        out[12] = 0xf2u;
        out[13] = ina[1];
        out[14] = ina[2];
        out[15] = ina[3];
    }
    if (kid != 0u) memset(recomp_memory(kid, 8u), 0, 8u);
    kernel_return(3u, 0u);
}

RecompFunction recomp_live_transport_lookup(uint32_t guest_address)
{
    switch (guest_address) {
    case 0x00381019u: return live_socket;
    case 0x00381054u: return live_bind;
    case 0x0038103au: return live_ioctlsocket;
    case 0x00381024u: return live_closesocket;
    case 0x0038102fu: return live_shutdown;
    case 0x003810a2u: return live_recvfrom;
    case 0x003810c0u: return live_sendto;
    case 0x003814d3u: return live_wsa_last_error;
    case 0x00380f9fu: return live_xnaddr_to_inaddr;
    case 0x00381109u: return live_inaddr_to_xnaddr;
    default: return NULL;
    }
}
