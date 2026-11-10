#ifdef _WIN32
#define _CRT_RAND_S /* rand_s, from the system's secure random source */
#endif
#include "netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int SocketLength;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef socklen_t SocketLength;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

enum {
    MAGIC = 0x42523244u,       /* "D2RB" */
    GOODBYE_MAGIC = 0x59423244u, /* "D2BY", the whole packet */
    ROOM_CLOSED_MAGIC = 0x43523244u, /* "D2RC", the whole packet: the sender left the room */
    LOBBY_MAGIC = 0x424c3244u, /* "D2LB" */
    LOBBY_VERSION = 5u,
    LOBBY_HELLO = 1u,
    LOBBY_WELCOME = 2u,
    LOBBY_REQUEST = 3u, /* send your chunks from this index; past your last, I hold them all */
    LOBBY_CHUNK = 4u,   /* one chunk of the sender's profile */
    LOBBY_ROOM = 5u,    /* a room member's state */
    LOBBY_BYTES = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 6u,
    ROOM_BYTES = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 2u + RECOMP_NETPLAY_NAME_BYTES + 10u + 5u +
        RECOMP_NETPLAY_CHAT_BYTES + 8u,
    ROOM_READY = 1u,
    ROOM_START = 2u,
    ROOM_SILENCE_MS = 3000u,
    PART_HEADER = 10u,
    CHUNK_BYTES = 1024u,
    CHUNKS_PER_REQUEST = 16u,
    MAX_CHUNKS = RECOMP_NETPLAY_MAX_PROFILE / CHUNK_BYTES,
};

static void put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static uint32_t get32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static uint16_t get16(const uint8_t *in)
{
    return (uint16_t)(in[0] | in[1] << 8);
}

static void put_pad(uint8_t *out, const RecompInputGamepad *pad)
{
    put16(out, pad->buttons);
    memcpy(out + 2, pad->analog_buttons, RECOMP_INPUT_ANALOG_BUTTON_COUNT);
    put16(out + 10, (uint16_t)pad->thumb_lx);
    put16(out + 12, (uint16_t)pad->thumb_ly);
    put16(out + 14, (uint16_t)pad->thumb_rx);
    put16(out + 16, (uint16_t)pad->thumb_ry);
}

static void get_pad(const uint8_t *in, RecompInputGamepad *pad)
{
    memset(pad, 0, sizeof *pad);
    pad->buttons = get16(in);
    memcpy(pad->analog_buttons, in + 2, RECOMP_INPUT_ANALOG_BUTTON_COUNT);
    pad->thumb_lx = (int16_t)get16(in + 10);
    pad->thumb_ly = (int16_t)get16(in + 12);
    pad->thumb_rx = (int16_t)get16(in + 14);
    pad->thumb_ry = (int16_t)get16(in + 16);
}

/* Layout: magic, sender's next frame, pads it holds from us, its advantage,
   first pad's frame, pad count, its newest desync check's frame and hash,
   pads. */
size_t recomp_netplay_encode(const RecompNetplay *net, const RecompRollbackSession *s,
    int32_t advantage, uint8_t *packet)
{
    uint32_t first = net->peer_ack;
    if (s->local_count > RECOMP_ROLLBACK_HISTORY && first < s->local_count - RECOMP_ROLLBACK_HISTORY)
        first = s->local_count - RECOMP_ROLLBACK_HISTORY;
    if (first > s->local_count) first = s->local_count;
    uint32_t count = s->local_count - first;
    if (count > RECOMP_NETPLAY_MAX_PADS) count = RECOMP_NETPLAY_MAX_PADS;
    put32(packet, MAGIC);
    put32(packet + 4, s->frame);
    put32(packet + 8, s->remote_count);
    put32(packet + 12, (uint32_t)advantage);
    put32(packet + 16, first);
    packet[20] = (uint8_t)count;
    const uint32_t newest = (net->check_count + RECOMP_NETPLAY_CHECKS - 1u) % RECOMP_NETPLAY_CHECKS;
    const bool checked = net->check_count != 0u;
    put32(packet + 21, checked ? net->check_frames[newest] : 0u);
    put32(packet + 25, checked ? (uint32_t)net->check_hashes[newest] : 0u);
    put32(packet + 29, checked ? (uint32_t)(net->check_hashes[newest] >> 32) : 0u);
    for (uint32_t i = 0u; i < count; ++i)
        put_pad(packet + RECOMP_NETPLAY_HEADER_BYTES + i * RECOMP_NETPLAY_PAD_BYTES,
            &s->local[(first + i) % RECOMP_ROLLBACK_HISTORY]);
    return RECOMP_NETPLAY_HEADER_BYTES + count * RECOMP_NETPLAY_PAD_BYTES;
}

static uint64_t now_ms(void);

bool recomp_netplay_decode(RecompNetplay *net, RecompRollbackSession *s,
    const uint8_t *packet, size_t size)
{
    if (size == 4u && get32(packet) == GOODBYE_MAGIC) {
        net->peer_left = true;
        return true;
    }
    if (size < RECOMP_NETPLAY_HEADER_BYTES || get32(packet) != MAGIC) return false;
    const uint32_t count = packet[20];
    if (count > RECOMP_NETPLAY_MAX_PADS ||
        size != RECOMP_NETPLAY_HEADER_BYTES + count * RECOMP_NETPLAY_PAD_BYTES)
        return false;
    const uint32_t ack = get32(packet + 8);
    if (ack > s->local_count) return false;
    const uint32_t first = get32(packet + 16);
    const uint32_t frame = get32(packet + 4);
    const int32_t advantage = (int32_t)get32(packet + 12);
    if (!net->peer_seen || frame > net->peer_frame)
        net->peer_samples[net->peer_count++ % RECOMP_NETPLAY_SYNC_FRAMES] = advantage;
    /* Packets can arrive out of order; only newer state moves forward. */
    if (!net->peer_seen || frame >= net->peer_frame) {
        net->peer_frame = frame;
        net->peer_advantage = advantage;
    }
    if (ack > net->peer_ack) {
        /* The newest pad the peer holds was sent once, when it was stamped. */
        if (ack <= net->pads_stamped && ack + RECOMP_NETPLAY_MAX_PADS > net->pads_stamped) {
            const uint64_t sample = now_ms() - net->pad_sent_ms[(ack - 1u) % RECOMP_NETPLAY_MAX_PADS];
            net->rtt_ms = net->rtt_known ? (uint32_t)((net->rtt_ms * 7u + sample) / 8u) : (uint32_t)sample;
            net->rtt_known = true;
        }
        net->peer_ack = ack;
    }
    net->peer_seen = true;
    const uint32_t check_frame = get32(packet + 21);
    if (check_frame > net->peer_check_frame) {
        net->peer_check_frame = check_frame;
        net->peer_check_hash = get32(packet + 25) | (uint64_t)get32(packet + 29) << 32;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t frame = first + i;
        if (frame < s->remote_count) continue;
        if (frame > s->remote_count) break;
        RecompInputGamepad pad;
        get_pad(packet + RECOMP_NETPLAY_HEADER_BYTES + i * RECOMP_NETPLAY_PAD_BYTES, &pad);
        if (!recomp_rollback_add_remote(s, &pad)) break;
    }
    return true;
}

/* Layout: magic, version, kind, digest, delay, flags, the sender's profile size. */
static size_t encode_lobby(uint8_t kind, const RecompNetplayLobby *lobby, uint32_t profile_size,
    uint8_t *out)
{
    put32(out, LOBBY_MAGIC);
    out[4] = LOBBY_VERSION;
    out[5] = kind;
    memcpy(out + 6, lobby->digest, RECOMP_NETPLAY_DIGEST_BYTES);
    out[6u + RECOMP_NETPLAY_DIGEST_BYTES] = lobby->delay;
    out[7u + RECOMP_NETPLAY_DIGEST_BYTES] = lobby->flags;
    put32(out + 8u + RECOMP_NETPLAY_DIGEST_BYTES, profile_size);
    return LOBBY_BYTES;
}

/* The packet's kind, or 0 when it is not a lobby packet of this version. */
static uint8_t decode_lobby(const uint8_t *in, size_t size, RecompNetplayLobby *lobby,
    uint32_t *profile_size)
{
    if (size != LOBBY_BYTES || get32(in) != LOBBY_MAGIC || in[4] != LOBBY_VERSION) return 0u;
    memcpy(lobby->digest, in + 6, RECOMP_NETPLAY_DIGEST_BYTES);
    lobby->delay = in[6u + RECOMP_NETPLAY_DIGEST_BYTES];
    lobby->flags = in[7u + RECOMP_NETPLAY_DIGEST_BYTES];
    *profile_size = get32(in + 8u + RECOMP_NETPLAY_DIGEST_BYTES);
    return in[5];
}

/* A request or chunk: magic, version, kind, chunk index, then a chunk's bytes. */
static size_t encode_part(uint8_t kind, uint32_t index, uint8_t *out)
{
    put32(out, LOBBY_MAGIC);
    out[4] = LOBBY_VERSION;
    out[5] = kind;
    put32(out + 6, index);
    return PART_HEADER;
}

static bool decode_part(const uint8_t *in, size_t size, uint8_t kind, uint32_t *index)
{
    if (size < PART_HEADER || get32(in) != LOBBY_MAGIC || in[4] != LOBBY_VERSION || in[5] != kind)
        return false;
    *index = get32(in + 6);
    return true;
}

static uint32_t chunk_count(uint32_t size) { return (size + CHUNK_BYTES - 1u) / CHUNK_BYTES; }

static uint32_t chunk_size(uint32_t size, uint32_t index)
{
    const uint32_t left = size - index * CHUNK_BYTES;
    return left < CHUNK_BYTES ? left : CHUNK_BYTES;
}

static bool random_bytes(uint8_t *out, size_t size)
{
#ifdef _WIN32
    for (size_t i = 0u; i < size; ++i) {
        unsigned int value;
        if (rand_s(&value) != 0) return false;
        out[i] = (uint8_t)value;
    }
    return true;
#else
    FILE *source = fopen("/dev/urandom", "rb");
    const bool read = source != NULL && fread(out, 1u, size, source) == size;
    if (source != NULL) fclose(source);
    return read;
#endif
}

static bool open_socket(RecompNetplay *net, uint16_t local_port)
{
    memset(net, 0, sizeof *net);
    net->socket = (uintptr_t)INVALID_SOCKET;
    if (!random_bytes(net->match.nonce, sizeof net->match.nonce)) return false;
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
    const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) return false;
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef _WIN32
    u_long nonblocking = 1u;
    const bool configured = ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
#else
    const bool configured = fcntl(handle, F_SETFL, fcntl(handle, F_GETFL) | O_NONBLOCK) == 0;
#endif
    if (!configured || bind(handle, (const struct sockaddr *)&local, sizeof local) != 0) {
        closesocket(handle);
        return false;
    }
    net->socket = (uintptr_t)handle;
    return true;
}

static bool resolve(const char *host, uint32_t *address)
{
    struct addrinfo hints, *found = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0 || found == NULL) return false;
    *address = ((const struct sockaddr_in *)(const void *)found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return true;
}

bool recomp_netplay_open(RecompNetplay *net, uint16_t local_port,
    const char *peer_host, uint16_t peer_port)
{
    if (!open_socket(net, local_port)) return false;
    if (!resolve(peer_host, &net->peer_address)) {
        recomp_netplay_close(net);
        return false;
    }
    net->peer_port = htons(peer_port);
    return true;
}

void recomp_netplay_close(RecompNetplay *net)
{
    if ((SOCKET)net->socket != INVALID_SOCKET) closesocket((SOCKET)net->socket);
    net->socket = (uintptr_t)INVALID_SOCKET;
}

bool recomp_netplay_listen(RecompNetplay *net, uint16_t port)
{
    return open_socket(net, port);
}

bool recomp_netplay_route_address(char *text, size_t size)
{
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
    const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) return false;
    /* Connecting a UDP socket sends nothing; it only picks the route. */
    struct sockaddr_in remote, local;
    memset(&remote, 0, sizeof remote);
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808u);
    SocketLength length = sizeof local;
    const bool found = connect(handle, (const struct sockaddr *)&remote, sizeof remote) == 0 &&
        getsockname(handle, (struct sockaddr *)&local, &length) == 0 &&
        inet_ntop(AF_INET, &local.sin_addr, text, (SocketLength)size) != NULL;
    closesocket(handle);
    return found;
}

/* The matchmaker's packets (tools/matchmaker.py): "D2MM", version, kind,
   the client socket's nonce, then for HOST, JOIN, LEAVE and RELAY the
   server's cookie, then a body whose numbers are big endian. */
static const uint8_t MATCH_MAGIC[4] = {'D', '2', 'M', 'M'};
enum {
    MATCH_VERSION = 2u,
    MATCH_HOST = 1u, MATCH_HOSTED, MATCH_LIST, MATCH_ROOMS, MATCH_JOIN, MATCH_PEER, MATCH_GONE, MATCH_LEAVE,
    MATCH_RELAY, MATCH_COOKIE,
    MATCH_HEADER = 6u,
    MATCH_BODY = MATCH_HEADER + RECOMP_NETPLAY_TOKEN_BYTES, /* after the nonce */
    MATCH_ASKED = MATCH_BODY + RECOMP_NETPLAY_TOKEN_BYTES,  /* after the cookie */
    MATCH_LIST_BYTES = 512u, /* a list request is padded, so its answer is never larger */
    MATCH_ENTRY = 42u, /* ends with the host's round trip to the server */
    MATCH_PEER_BYTES = MATCH_BODY + 6u + 6u + 1u + RECOMP_NETPLAY_NAME_BYTES,
    PUNCH_MS = 5000u,
};

static void send_raw(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof peer);
    peer.sin_family = AF_INET;
    peer.sin_port = port;
    peer.sin_addr.s_addr = address;
    /* A full send buffer drops this packet; the next one repeats its pads. */
    (void)sendto((SOCKET)net->socket, (const char *)packet, (int)size, 0,
        (const struct sockaddr *)&peer, sizeof peer);
}

/* Writes a packet's start and returns where its body goes. */
static uint8_t *match_header(const RecompNetplay *net, uint8_t *out, uint8_t kind)
{
    memcpy(out, MATCH_MAGIC, sizeof MATCH_MAGIC);
    out[4] = MATCH_VERSION;
    out[5] = kind;
    memcpy(out + MATCH_HEADER, net->match.nonce, RECOMP_NETPLAY_TOKEN_BYTES);
    if (kind == MATCH_LIST) return out + MATCH_BODY;
    memcpy(out + MATCH_BODY, net->match.cookie, RECOMP_NETPLAY_TOKEN_BYTES);
    return out + MATCH_ASKED;
}

/* A relaying socket reaches its peer through the matchmaker, which knows
   where to forward. */
static void send_to(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size)
{
    uint8_t wrapped[MATCH_ASKED + RECOMP_NETPLAY_MAX_PACKET];
    if (net->match.relay && address == net->peer_address && port == net->peer_port &&
        size <= RECOMP_NETPLAY_MAX_PACKET) {
        memcpy(match_header(net, wrapped, MATCH_RELAY), packet, size);
        send_raw(net, net->match.server_address, net->match.server_port, wrapped, MATCH_ASKED + size);
        return;
    }
    send_raw(net, address, port, packet, size);
}

void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *s, int32_t advantage)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET];
    const uint64_t now = now_ms();
    for (; net->pads_stamped < s->local_count; ++net->pads_stamped)
        net->pad_sent_ms[net->pads_stamped % RECOMP_NETPLAY_MAX_PADS] = now;
    send_to(net, net->peer_address, net->peer_port, packet,
        recomp_netplay_encode(net, s, advantage, packet));
}

void recomp_netplay_send_goodbye(const RecompNetplay *net)
{
    uint8_t packet[4];
    put32(packet, GOODBYE_MAGIC);
    for (int i = 0; i < 3; ++i) send_to(net, net->peer_address, net->peer_port, packet, sizeof packet);
}

static uint64_t now_ms(void);

static uint32_t get32be(const uint8_t *in)
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

static void relay_on(RecompNetplay *net, const char *why)
{
    RecompNetplayMatch *m = &net->match;
    if (m->relay) return;
    m->relay = true;
    m->punching = false;
    /* Without an endpoint the server's own stands for the peer; wrapped packets ignore it. */
    if (net->peer_port == 0u) {
        net->peer_address = m->server_address;
        net->peer_port = m->server_port;
    }
    fprintf(stderr, "recomp netplay: %s; relaying through the matchmaker\n", why);
}

static void match_answer(RecompNetplay *net, const uint8_t *packet, size_t size)
{
    RecompNetplayMatch *m = &net->match;
    if (packet[5] == MATCH_HOSTED && size == MATCH_BODY + 10u) {
        const uint32_t id = get32be(packet + MATCH_BODY);
        if (id != m->room_id) fprintf(stderr, "recomp netplay: listed on the matchmaker as room %u\n", id);
        m->room_id = id;
        if (m->host_sent_ms != 0u) {
            const uint64_t rtt = now_ms() - m->host_sent_ms;
            m->host_rtt_ms = (uint16_t)(rtt == 0u ? 1u : rtt > 65535u ? 65535u : rtt); /* 0 means unknown */
            m->host_sent_ms = 0u;
        }
    } else if (packet[5] == MATCH_ROOMS && m->list_sent_ms != 0u && size >= MATCH_BODY + 1u &&
        packet[MATCH_BODY] <= RECOMP_NETPLAY_LISTED_ROOMS) {
        const uint32_t count = packet[MATCH_BODY];
        if (size != MATCH_BODY + 1u + count * MATCH_ENTRY) return;
        const uint64_t rtt = now_ms() - m->list_sent_ms;
        m->list_sent_ms = 0u;
        m->room_count = count;
        for (uint32_t i = 0u; i < m->room_count; ++i) {
            const uint8_t *at = packet + MATCH_BODY + 1u + i * MATCH_ENTRY;
            RecompNetplayListedRoom *room = &m->rooms[i];
            const size_t length = at[7] < RECOMP_NETPLAY_NAME_BYTES ? at[7] : RECOMP_NETPLAY_NAME_BYTES;
            memset(room, 0, sizeof *room);
            room->id = get32be(at);
            room->age_s = (uint32_t)at[4] << 8 | at[5];
            room->delay = at[6];
            memcpy(room->name, at + 8u, length);
            room->name[length] = '\0';
            const uint32_t host_rtt = (uint32_t)at[MATCH_ENTRY - 2u] << 8 | at[MATCH_ENTRY - 1u];
            room->pinged = host_rtt != 0u;
            room->ping_ms = room->pinged ? (uint32_t)rtt + host_rtt : 0u;
        }
        m->listed = true;
    } else if (packet[5] == MATCH_GONE) {
        m->gone = true;
    } else if (packet[5] == MATCH_COOKIE && size == MATCH_BODY + RECOMP_NETPLAY_TOKEN_BYTES) {
        memcpy(m->cookie, packet + MATCH_BODY, sizeof m->cookie);
        /* The server dropped a recent ask for want of this cookie, or took it and sent a newer one. */
        if (m->asked_size != 0u && now_ms() - m->asked_ms < 2000u) {
            memcpy(m->asked + MATCH_BODY, m->cookie, sizeof m->cookie);
            send_raw(net, m->server_address, m->server_port, m->asked, m->asked_size);
        }
        m->asked_size = 0u; /* once */
    } else if (packet[5] == MATCH_PEER && size == MATCH_PEER_BYTES) {
        if (m->busy) return;
        uint32_t address[2];
        uint16_t port[2];
        for (unsigned i = 0u; i < 2u; ++i) {
            memcpy(&address[i], packet + MATCH_BODY + 6u * i, 4u);
            memcpy(&port[i], packet + MATCH_BODY + 6u * i + 4u, 2u);
        }
        /* The server says it again for a repeated join; the pair is already met. */
        if (m->met && memcmp(address, m->candidate_address, sizeof address) == 0 &&
            memcmp(port, m->candidate_port, sizeof port) == 0)
            return;
        memcpy(m->candidate_address, address, sizeof address);
        memcpy(m->candidate_port, port, sizeof port);
        m->met = true;
        m->relay = false;
        const unsigned first = port[0] != 0u ? 0u : 1u;
        if (port[first] == 0u) {
            net->peer_port = 0u;
            relay_on(net, "matched without endpoints");
            return;
        }
        net->peer_address = address[first];
        net->peer_port = port[first];
        m->punching = true;
        m->punch_ms = now_ms();
        /* No addresses here or below: players paste this log into public issues. */
        fprintf(stderr, "recomp netplay: matched; punching to the other player\n");
    }
}

/* The next queued packet's size, or -1 when none is left. The matchmaker's
   answers are recorded in net->match on the way, and a relayed packet comes
   out unwrapped, from the peer. */
static int receive_from(RecompNetplay *net, uint8_t *packet, size_t capacity,
    struct sockaddr_in *from)
{
    uint8_t raw[MATCH_BODY + RECOMP_NETPLAY_MAX_PACKET + 1u];
    for (;;) {
        SocketLength from_size = sizeof *from;
        const int size = (int)recvfrom((SOCKET)net->socket, (char *)raw, (int)sizeof raw, 0,
            (struct sockaddr *)from, &from_size);
        /* Windows reports an ICMP port-unreachable from an earlier send as a
           failed receive; it is not fatal before the peer has bound its port. */
        if (size < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET) continue;
#endif
            return -1;
        }
        if (from->sin_family != AF_INET) continue;
        const uint8_t *body = raw;
        size_t length = (size_t)size;
        RecompNetplayMatch *m = &net->match;
        if (m->server_port != 0u && from->sin_addr.s_addr == m->server_address && from->sin_port == m->server_port) {
            /* Anyone can send from the server's address; only the server knows this socket's nonce. */
            if (length < MATCH_BODY || memcmp(raw, MATCH_MAGIC, sizeof MATCH_MAGIC) != 0 || raw[4] != MATCH_VERSION ||
                memcmp(raw + MATCH_HEADER, m->nonce, RECOMP_NETPLAY_TOKEN_BYTES) != 0)
                continue;
            if (raw[5] != MATCH_RELAY) {
                match_answer(net, raw, length);
                continue;
            }
            /* The other side gave up on a direct path, so this one follows. */
            relay_on(net, "the other player relays");
            from->sin_addr.s_addr = net->peer_address;
            from->sin_port = net->peer_port;
            body += MATCH_BODY;
            length -= MATCH_BODY;
        }
        if (length > capacity) length = capacity;
        memcpy(packet, body, length);
        return (int)length;
    }
}

static bool from_peer(const RecompNetplay *net, const struct sockaddr_in *from)
{
    return from->sin_addr.s_addr == net->peer_address && from->sin_port == net->peer_port;
}

static uint64_t now_ms(void)
{
    /* Monotonic, so a system clock change cannot end a lobby or stall its retries. */
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 0u;
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart / frequency.QuadPart * 1000 +
        counter.QuadPart % frequency.QuadPart * 1000 / frequency.QuadPart);
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/* What the clocks a host sends an address it was not given are offset by. Every player this host
   answers sees its clock, so without this a clock shown to one address could be echoed from a forged one.
   shortcut: a keyed mixer, not a MAC; replace it if a seat ever guards more than the room. */
static uint32_t clock_salt(uint32_t address, uint16_t port)
{
    static uint64_t key;
    if (key == 0u && !random_bytes((uint8_t *)&key, sizeof key)) return 0u;
    uint64_t mixed = key ^ ((uint64_t)address << 16 | port);
    mixed = (mixed ^ mixed >> 30) * 0xbf58476d1ce4e5b9u; /* splitmix64's finalizer */
    mixed = (mixed ^ mixed >> 27) * 0x94d049bb133111ebu;
    return (uint32_t)((mixed ^ mixed >> 31) >> 32);
}

/* Fills a room packet but for its chat line and returns where that goes.
   Layout: magic, version, kind, digest, flags, name length, name, the
   sender's clock, the last clock it heard and how long ago, for the ping,
   a chat line's id, length and text, then the sender's install ID. */
static uint8_t *room_packet(uint8_t *packet, const RecompNetplayRoom *room, const char *digest, const char *name,
    bool ready, bool start, uint32_t clock, uint32_t echo, uint64_t age)
{
    memset(packet, 0, ROOM_BYTES);
    put32(packet, LOBBY_MAGIC);
    packet[4] = LOBBY_VERSION;
    packet[5] = LOBBY_ROOM;
    memcpy(packet + 6u, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    uint8_t *at = packet + 6u + RECOMP_NETPLAY_DIGEST_BYTES;
    at[0] = (uint8_t)((ready ? ROOM_READY : 0u) | (start ? ROOM_START : 0u));
    const size_t length = strlen(name);
    at[1] = (uint8_t)(length < RECOMP_NETPLAY_NAME_BYTES ? length : RECOMP_NETPLAY_NAME_BYTES);
    memcpy(at + 2u, name, at[1]);
    at += 2u + RECOMP_NETPLAY_NAME_BYTES;
    put32(at, clock | 1u); /* never 0, which means no clock heard */
    put32(at + 4u, echo);
    put16(at + 8u, (uint16_t)(echo == 0u ? 0u : age > 65535u ? 65535u : age));
    put32(packet + ROOM_BYTES - 8u, (uint32_t)room->id);
    put32(packet + ROOM_BYTES - 4u, (uint32_t)(room->id >> 32));
    return at + 10u;
}

void recomp_netplay_room_tick(RecompNetplay *net, RecompNetplayRoom *room, const char *digest,
    const char *name, bool ready, bool start)
{
    const uint64_t now = now_ms();
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    int size;
    while ((size = receive_from(net, packet, sizeof packet, &from)) >= 0) {
        if (room->hosting && !start && size == LOBBY_BYTES && get32(packet) == LOBBY_MAGIC &&
            packet[5] == LOBBY_HELLO) {
            /* A joiner calling a match this host cancelled: send it back to its room. */
            uint8_t goodbye[4];
            put32(goodbye, GOODBYE_MAGIC);
            send_to(net, from.sin_addr.s_addr, from.sin_port, goodbye, sizeof goodbye);
            continue;
        }
        if (size == 4 && get32(packet) == ROOM_CLOSED_MAGIC) {
            /* A session's goodbye can linger into the room, so leaving has its own packet. */
            if (net->peer_port != 0u && from_peer(net, &from)) {
                fprintf(stderr, "recomp netplay: the other player left the room\n");
                recomp_netplay_room_drop(net, room);
                net->peer_port = 0u; /* a joiner stops calling the closed room */
                room->left = true;
            }
            continue;
        }
        if (size != ROOM_BYTES || get32(packet) != LOBBY_MAGIC || packet[4] != LOBBY_VERSION ||
            packet[5] != LOBBY_ROOM)
            continue;
        RecompNetplayMatch *m = &net->match;
        bool candidate = false;
        for (unsigned i = 0u; i < 2u; ++i)
            candidate = candidate || (m->punching && m->candidate_port[i] == from.sin_port &&
                                         m->candidate_address[i] == from.sin_addr.s_addr);
        const uint8_t *sender = packet + ROOM_BYTES - 8u;
        if (room->blocked != NULL && room->blocked((uint64_t)get32(sender + 4u) << 32 | get32(sender))) {
            /* Say nothing back. Relay stays on, so the server's relayed packets end here quietly. */
            /* Only this room's own peer, a candidate or, for an empty host, the would-be peer. */
            if (from_peer(net, &from) || candidate || (room->hosting && !room->joined)) room->refused = true;
            if (from_peer(net, &from) || candidate) {
                net->peer_port = 0u;
                m->punching = false;
            }
            continue;
        }
        if (!from_peer(net, &from)) {
            /* A joiner takes either endpoint the matchmaker gave. */
            if (room->joined || (!room->hosting && !candidate)) continue;
            if (!candidate) {
                /* An empty host hears anyone, at an address that may be forged. It answers one packet for
                   one and takes the player only when they echo a clock sent to that address in the last
                   3 s. Nothing is kept until then, so forged packets neither hold the room nor keep a
                   player out. */
                const uint8_t *heard = packet + 6u + RECOMP_NETPLAY_DIGEST_BYTES + 2u + RECOMP_NETPLAY_NAME_BYTES;
                const uint32_t salt = clock_salt(from.sin_addr.s_addr, from.sin_port), echoed = get32(heard + 4u);
                /* Signed: the echoed clock is forced odd, so it can be 1 ms ahead. */
                const int32_t since = (int32_t)((uint32_t)now + salt - echoed);
                if (echoed == 0u || since < -1 || since > (int32_t)ROOM_SILENCE_MS) {
                    uint8_t answer[ROOM_BYTES];
                    room_packet(answer, room, digest, name, false, false, (uint32_t)now + salt, get32(heard), 0u);
                    send_raw(net, from.sin_addr.s_addr, from.sin_port, answer, ROOM_BYTES);
                    continue;
                }
                room->salt = salt;
            }
            net->peer_address = from.sin_addr.s_addr;
            net->peer_port = from.sin_port;
            room->pinged = false;
            m->relay = false; /* heard directly */
        }
        if (m->punching) { /* relayed packets end punching first, so this one came directly */
            m->punching = false;
            fprintf(stderr, "recomp netplay: reached the other player directly\n");
        }
        const uint8_t *at = packet + 6u + RECOMP_NETPLAY_DIGEST_BYTES;
        const uint8_t *clocks = at + 2u + RECOMP_NETPLAY_NAME_BYTES;
        room->echo = get32(clocks);
        room->echo_at_ms = now;
        const uint32_t echo = get32(clocks + 4u), age = get16(clocks + 8u);
        room->joined = true;
        m->busy = true;
        room->heard_ms = now;
        room->mismatch = memcmp(packet + 6u, digest, RECOMP_NETPLAY_DIGEST_BYTES) != 0;
        room->ready = (at[0] & ROOM_READY) != 0u;
        room->start = (at[0] & ROOM_START) != 0u;
        const size_t length = at[1] < RECOMP_NETPLAY_NAME_BYTES ? at[1] : RECOMP_NETPLAY_NAME_BYTES;
        memcpy(room->name, at + 2u, length);
        room->name[length] = '\0';
        room->peer_id = (uint64_t)get32(sender + 4u) << 32 | get32(sender);
        if (echo != 0u) {
            /* Signed: the echoed clock is forced odd, so it can be 1 ms ahead. */
            const int64_t ping = (int64_t)(int32_t)((uint32_t)now + room->salt - echo) - (int64_t)age;
            room->ping_ms = ping > 0 ? (uint32_t)ping : 0u;
            room->pinged = true;
        }
        at = clocks + 10u;
        const uint32_t id = get32(at);
        const size_t said = at[4];
        /* One line a second; a line held back here comes again in a later repeat. */
        if (id != room->heard_id && said != 0u && said <= RECOMP_NETPLAY_CHAT_BYTES &&
            now - room->heard_line_ms >= 1000u) {
            room->heard_id = id;
            room->heard_line_ms = now;
            char line[RECOMP_NETPLAY_CHAT_BYTES + 1u];
            bool printable = true;
            for (size_t i = 0u; i < said; ++i) printable = printable && at[5u + i] >= 0x20u && at[5u + i] < 0x7fu;
            memcpy(line, at + 5u, said);
            line[said] = '\0';
            if (printable && !recomp_netplay_name_banned(line)) {
                memcpy(room->heard, line, said + 1u);
                room->heard_new = true;
            }
        }
    }
    if (room->joined && now - room->heard_ms > ROOM_SILENCE_MS) recomp_netplay_room_drop(net, room);
    /* punch_ms can be newer than now when this tick read the match itself. */
    if (net->match.punching && now > net->match.punch_ms + PUNCH_MS) relay_on(net, "no direct path after 5 s");
    if (net->peer_port == 0u || (!start && now - room->sent_ms < 100u)) return;
    room->sent_ms = now;
    uint8_t *at = room_packet(packet, room, digest, name, ready, start, (uint32_t)now + room->salt, room->echo,
        now - room->echo_at_ms);
    if (now - room->said_ms < 2000u) {
        put32(at, room->say_id);
        at[4] = (uint8_t)strlen(room->say);
        memcpy(at + 5u, room->say, at[4]);
    }
    const RecompNetplayMatch *m = &net->match;
    if (!m->punching) {
        send_to(net, net->peer_address, net->peer_port, packet, ROOM_BYTES);
        return;
    }
    /* Sending first opens this side's router for the other's answer. */
    for (unsigned i = 0u; i < 2u; ++i)
        if (m->candidate_port[i] != 0u) send_raw(net, m->candidate_address[i], m->candidate_port[i], packet, ROOM_BYTES);
}

void recomp_netplay_room_say(RecompNetplayRoom *room, const char *text)
{
    const uint64_t now = now_ms();
    const size_t length = strlen(text);
    const size_t kept = length < RECOMP_NETPLAY_CHAT_BYTES ? length : RECOMP_NETPLAY_CHAT_BYTES;
    memcpy(room->say, text, kept);
    room->say[kept] = '\0';
    /* The clock makes each line's id differ from those of earlier rooms, too. */
    const uint32_t id = (uint32_t)now | 1u;
    room->say_id = id == room->say_id ? id + 2u : id;
    room->said_ms = now;
    room->sent_ms = 0u; /* on the next tick */
}

void recomp_netplay_room_drop(RecompNetplay *net, RecompNetplayRoom *room)
{
    room->joined = room->ready = room->start = room->pinged = false;
    room->echo = 0u;
    net->match.busy = false;
    if (room->hosting) { /* let another player in */
        net->peer_port = 0u;
        net->match.relay = false;
    }
}

void recomp_netplay_room_leave(const RecompNetplay *net)
{
    if (net->peer_port == 0u) return;
    uint8_t packet[4];
    put32(packet, ROOM_CLOSED_MAGIC);
    for (int i = 0; i < 3; ++i) send_to(net, net->peer_address, net->peer_port, packet, sizeof packet);
}

bool recomp_netplay_match_server(RecompNetplay *net, const char *host, uint16_t port)
{
    if (port == 0u || !resolve(host, &net->match.server_address)) return false;
    net->match.server_port = htons(port);
    return true;
}

static void send_match(RecompNetplay *net, const uint8_t *packet, size_t size)
{
    RecompNetplayMatch *m = &net->match;
    if (m->server_port == 0u) return;
    send_raw(net, m->server_address, m->server_port, packet, size);
    if (packet[5] != MATCH_LIST && size <= sizeof m->asked) {
        memcpy(m->asked, packet, size);
        m->asked_size = size;
        m->asked_ms = now_ms();
    }
}

/* Name length and name, zero padded. */
static uint8_t *put_name(uint8_t *out, const char *name)
{
    const size_t length = strlen(name);
    out[0] = (uint8_t)(length < RECOMP_NETPLAY_NAME_BYTES ? length : RECOMP_NETPLAY_NAME_BYTES);
    memset(out + 1u, 0, RECOMP_NETPLAY_NAME_BYTES);
    memcpy(out + 1u, name, out[0]);
    return out + 1u + RECOMP_NETPLAY_NAME_BYTES;
}

/* This computer's LAN address and the socket's port, for players behind the same router. */
static void put_lan(const RecompNetplay *net, uint8_t *out)
{
    memset(out, 0, 6u);
    char text[INET_ADDRSTRLEN];
    struct sockaddr_in local;
    SocketLength length = sizeof local;
    if (!recomp_netplay_route_address(text, sizeof text) ||
        getsockname((SOCKET)net->socket, (struct sockaddr *)&local, &length) != 0 ||
        inet_pton(AF_INET, text, out) != 1)
        return;
    memcpy(out + 4u, &local.sin_port, 2u);
}

void recomp_netplay_match_host(RecompNetplay *net, const char *digest, uint8_t delay, bool open,
    const char *name)
{
    uint8_t packet[MATCH_ASKED + RECOMP_NETPLAY_DIGEST_BYTES + 2u + 1u + RECOMP_NETPLAY_NAME_BYTES + 6u + 2u];
    uint8_t *at = match_header(net, packet, MATCH_HOST);
    memcpy(at, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    at += RECOMP_NETPLAY_DIGEST_BYTES;
    at[0] = delay;
    at[1] = open ? 1u : 0u;
    at = put_name(at + 2u, name);
    put_lan(net, at);
    at[6] = (uint8_t)(net->match.host_rtt_ms >> 8);
    at[7] = (uint8_t)net->match.host_rtt_ms;
    net->match.host_sent_ms = now_ms();
    send_match(net, packet, sizeof packet);
}

void recomp_netplay_match_list(RecompNetplay *net, const char *digest)
{
    uint8_t packet[MATCH_LIST_BYTES];
    memset(packet, 0, sizeof packet);
    memcpy(match_header(net, packet, MATCH_LIST), digest, RECOMP_NETPLAY_DIGEST_BYTES);
    net->match.list_sent_ms = now_ms();
    send_match(net, packet, sizeof packet);
}

void recomp_netplay_match_join(RecompNetplay *net, uint32_t room_id, const char *digest, const char *name)
{
    uint8_t packet[MATCH_ASKED + 4u + RECOMP_NETPLAY_DIGEST_BYTES + 1u + RECOMP_NETPLAY_NAME_BYTES + 6u];
    uint8_t *at = match_header(net, packet, MATCH_JOIN);
    for (unsigned i = 0u; i < 4u; ++i) at[i] = (uint8_t)(room_id >> (24u - 8u * i));
    memcpy(at + 4u, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    put_lan(net, put_name(at + 4u + RECOMP_NETPLAY_DIGEST_BYTES, name));
    send_match(net, packet, sizeof packet);
}

void recomp_netplay_match_leave(RecompNetplay *net)
{
    uint8_t packet[MATCH_ASKED];
    (void)match_header(net, packet, MATCH_LEAVE);
    send_match(net, packet, sizeof packet);
    net->match.room_id = 0u;
}

void recomp_netplay_match_poll(RecompNetplay *net)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    while (receive_from(net, packet, sizeof packet, &from) >= 0) {}
}

uint32_t recomp_netplay_suggested_delay(uint32_t rtt_ms)
{
    if (rtt_ms <= 20u) return 0u;
    const uint32_t frames = rtt_ms / 33u + (rtt_ms % 33u != 0u);
    return frames < RECOMP_ROLLBACK_WINDOW ? frames : RECOMP_ROLLBACK_WINDOW;
}

static void wait_readable(const RecompNetplay *net, unsigned milliseconds)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET((SOCKET)net->socket, &readable);
    struct timeval timeout = {0, (long)milliseconds * 1000L};
    (void)select((int)net->socket + 1, &readable, NULL, NULL, &timeout);
}

/* One direction each way: this side serves its profile in chunks on request
   and fetches the peer's the same way. A request past the last chunk says
   the asker holds them all; a session packet says the same, if every one of
   those was lost. */
typedef struct Transfer {
    RecompNetplayProfiles *profiles;
    uint32_t chunks, missing, asked; /* of the peer's profile */
    uint64_t next_ask, next_serve;
    uint64_t served; /* bytes of this side's profile sent */
    uint32_t served_end; /* past the last chunk sent */
    bool known, peer_done;
    bool have[MAX_CHUNKS];
} Transfer;

static void transfer_start(Transfer *t, uint32_t size)
{
    t->profiles->theirs_size = size;
    t->chunks = chunk_count(size);
    t->known = true;
    t->next_ask = 0u;
}

static void transfer_packet(RecompNetplay *net, Transfer *t, const uint8_t *packet, size_t size)
{
    const RecompNetplayProfiles *p = t->profiles;
    uint32_t index;
    if (decode_part(packet, size, LOBBY_REQUEST, &index)) {
        const uint32_t mine = chunk_count(p->mine_size);
        if (index >= mine) {
            t->peer_done = true;
            return;
        }
        /* A 10-byte request draws up to 16 KB. The next window goes out at
           once, as an honest peer moves on; an earlier one, a retransmit or
           a replay, waits 50 ms, and eight copies end it, far more than a
           lossy link needs. */
        const uint64_t now = now_ms();
        if (t->served >= 8ull * p->mine_size || (index < t->served_end && now < t->next_serve)) return;
        t->next_serve = now + 50u;
        const uint32_t end = index + CHUNKS_PER_REQUEST < mine ? index + CHUNKS_PER_REQUEST : mine;
        if (end > t->served_end) t->served_end = end; /* only forward, so a replayed window stays a replay */
        uint8_t chunk[PART_HEADER + CHUNK_BYTES];
        for (uint32_t i = index; i < mine && i < index + CHUNKS_PER_REQUEST; ++i) {
            const uint32_t bytes = chunk_size(p->mine_size, i);
            memcpy(chunk + encode_part(LOBBY_CHUNK, i, chunk), p->mine + i * CHUNK_BYTES, bytes);
            send_to(net, net->peer_address, net->peer_port, chunk, PART_HEADER + bytes);
            t->served += bytes;
        }
    } else if (t->known && decode_part(packet, size, LOBBY_CHUNK, &index) && index < t->chunks &&
        size == PART_HEADER + chunk_size(p->theirs_size, index)) {
        memcpy(p->theirs + index * CHUNK_BYTES, packet + PART_HEADER, size - PART_HEADER);
        t->have[index] = true;
    } else if (size >= 4u && get32(packet) == MAGIC) {
        t->peer_done = true;
    }
}

/* Asks for the first missing chunk once a burst has arrived or after a
   loss, and repeats done until the peer has finished too. True once both
   sides hold both profiles, after saying done once more in case the peer
   finished before hearing it. */
static bool transfer_tick(RecompNetplay *net, Transfer *t, uint64_t now)
{
    if (!t->known) return false;
    while (t->missing < t->chunks && t->have[t->missing]) ++t->missing;
    const bool done = t->missing == t->chunks && t->peer_done;
    if (done || now >= t->next_ask || (t->missing < t->chunks && t->missing >= t->asked + CHUNKS_PER_REQUEST)) {
        uint8_t request[PART_HEADER];
        send_to(net, net->peer_address, net->peer_port, request,
            encode_part(LOBBY_REQUEST, t->missing, request));
        t->asked = t->missing;
        t->next_ask = now + 100u;
    }
    return done;
}

static bool is_goodbye(const uint8_t *packet, int size)
{
    return size == 4 && get32(packet) == GOODBYE_MAGIC;
}

RecompNetplayLobbyResult recomp_netplay_host(RecompNetplay *net, const RecompNetplayLobby *lobby,
    RecompNetplayProfiles *profiles, uint32_t timeout_ms, bool (*cancelled)(void))
{
    profiles->theirs_size = 0u;
    const uint64_t deadline = now_ms() + timeout_ms;
    Transfer t;
    memset(&t, 0, sizeof t);
    t.profiles = profiles;
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u], welcome[LOBBY_BYTES];
    const size_t welcome_size = encode_lobby(LOBBY_WELCOME, lobby, profiles->mine_size, welcome);
    /* A room's socket arrives with its peer, so a forged hello from elsewhere cannot take the match. */
    const bool had_peer = net->peer_port != 0u;
    for (;;) {
        struct sockaddr_in from;
        int size;
        while ((size = receive_from(net, packet, sizeof packet, &from)) >= 0) {
            if ((t.known || had_peer) && !from_peer(net, &from)) continue;
            if (t.known && is_goodbye(packet, size)) {
                net->peer_left = true;
                return RECOMP_NETPLAY_LOBBY_CANCELLED;
            }
            RecompNetplayLobby hello;
            uint32_t theirs;
            if (decode_lobby(packet, (size_t)size, &hello, &theirs) == LOBBY_HELLO) {
                if (t.known) {
                    send_to(net, from.sin_addr.s_addr, from.sin_port, welcome, welcome_size);
                    continue;
                }
                if (theirs > RECOMP_NETPLAY_MAX_PROFILE) continue;
                send_to(net, from.sin_addr.s_addr, from.sin_port, welcome, welcome_size);
                // Keep waiting, so a stray hello cannot end the lobby; the welcome tells that joiner.
                if (memcmp(hello.digest, lobby->digest, RECOMP_NETPLAY_DIGEST_BYTES) != 0) {
                    fprintf(stderr, "recomp netplay: refused a player who runs a different build; still waiting\n");
                    continue;
                }
                net->peer_address = from.sin_addr.s_addr;
                net->peer_port = from.sin_port;
                transfer_start(&t, theirs);
            } else if (t.known) {
                transfer_packet(net, &t, packet, (size_t)size);
            }
        }
        const uint64_t now = now_ms();
        if (transfer_tick(net, &t, now)) return RECOMP_NETPLAY_LOBBY_OK;
        if (now >= deadline) {
            profiles->theirs_size = 0u;
            return RECOMP_NETPLAY_LOBBY_TIMEOUT;
        }
        if (cancelled != NULL && cancelled()) {
            if (t.known) recomp_netplay_send_goodbye(net);
            profiles->theirs_size = 0u;
            return RECOMP_NETPLAY_LOBBY_CANCELLED;
        }
        wait_readable(net, 20u);
    }
}

RecompNetplayLobbyResult recomp_netplay_join(RecompNetplay *net, RecompNetplayLobby *lobby,
    RecompNetplayProfiles *profiles, uint32_t timeout_ms, bool (*cancelled)(void))
{
    profiles->theirs_size = 0u;
    const uint64_t deadline = now_ms() + timeout_ms;
    Transfer t;
    memset(&t, 0, sizeof t);
    t.profiles = profiles;
    uint64_t next_hello = 0u;
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u], hello[LOBBY_BYTES];
    const size_t hello_size = encode_lobby(LOBBY_HELLO, lobby, profiles->mine_size, hello);
    for (;;) {
        struct sockaddr_in from;
        int size;
        while ((size = receive_from(net, packet, sizeof packet, &from)) >= 0) {
            if (!from_peer(net, &from)) continue;
            if (is_goodbye(packet, size)) {
                net->peer_left = true;
                return RECOMP_NETPLAY_LOBBY_CANCELLED;
            }
            RecompNetplayLobby welcome;
            uint32_t theirs;
            if (!t.known && decode_lobby(packet, (size_t)size, &welcome, &theirs) == LOBBY_WELCOME) {
                if (memcmp(welcome.digest, lobby->digest, RECOMP_NETPLAY_DIGEST_BYTES) != 0)
                    return RECOMP_NETPLAY_LOBBY_MISMATCH;
                if (theirs > RECOMP_NETPLAY_MAX_PROFILE) continue;
                lobby->delay = welcome.delay;
                lobby->flags = welcome.flags;
                transfer_start(&t, theirs);
            } else if (t.known) {
                transfer_packet(net, &t, packet, (size_t)size);
            }
        }
        const uint64_t now = now_ms();
        if (!t.known && now >= next_hello) {
            send_to(net, net->peer_address, net->peer_port, hello, hello_size);
            next_hello = now + 100u;
        }
        if (transfer_tick(net, &t, now)) return RECOMP_NETPLAY_LOBBY_OK;
        if (now >= deadline) {
            profiles->theirs_size = 0u;
            return RECOMP_NETPLAY_LOBBY_TIMEOUT;
        }
        if (cancelled != NULL && cancelled()) {
            recomp_netplay_send_goodbye(net);
            profiles->theirs_size = 0u;
            return RECOMP_NETPLAY_LOBBY_CANCELLED;
        }
        wait_readable(net, 20u);
    }
}

void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *s)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    int size;
    while ((size = receive_from(net, packet, sizeof packet, &from)) >= 0)
        if (from_peer(net, &from)) (void)recomp_netplay_decode(net, s, packet, (size_t)size);
}

uint32_t recomp_netplay_wait_frames(RecompNetplay *net, int32_t advantage)
{
    enum { N = RECOMP_NETPLAY_SYNC_FRAMES };
    net->local_samples[net->local_count++ % N] = advantage;
    if (net->cooldown != 0u) {
        --net->cooldown;
        return 0u;
    }
    if (net->local_count < N || net->peer_count < N) return 0u;
    int32_t difference = 0;
    for (uint32_t i = 0u; i < N; ++i) difference += net->local_samples[i] - net->peer_samples[i];
    if (difference <= 0) return 0u;
    /* Half the averaged difference, rounded to the nearest frame. */
    const int32_t wait = (difference + (int32_t)N) / (int32_t)(2u * N);
    if (wait <= 0) return 0u;
    /* The peer needs a round trip to report the new advantage. */
    net->cooldown = RECOMP_NETPLAY_SYNC_FRAMES;
    return wait > (int32_t)RECOMP_ROLLBACK_WINDOW ? RECOMP_ROLLBACK_WINDOW : (uint32_t)wait;
}

void recomp_netplay_add_check(RecompNetplay *net, uint32_t frame, uint64_t hash)
{
    net->check_frames[net->check_count % RECOMP_NETPLAY_CHECKS] = frame;
    net->check_hashes[net->check_count % RECOMP_NETPLAY_CHECKS] = hash;
    ++net->check_count;
}

bool recomp_netplay_own_check(const RecompNetplay *net, uint64_t *mine)
{
    for (uint32_t i = 0u; net->peer_check_frame != 0u && i < RECOMP_NETPLAY_CHECKS; ++i) {
        if (net->check_frames[i] != net->peer_check_frame) continue;
        *mine = net->check_hashes[i];
        return true;
    }
    return false;
}

