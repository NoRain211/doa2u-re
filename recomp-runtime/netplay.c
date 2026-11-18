#ifdef _WIN32
#define _CRT_RAND_S /* rand_s, from the system's secure random source */
#endif
#include "netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "netplay_wire.h"

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
            const uint64_t sample = netplay_now_ms() - net->pad_sent_ms[(ack - 1u) % RECOMP_NETPLAY_MAX_PADS];
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

bool recomp_netplay_listen(RecompNetplay *net, uint16_t port)
{
    return open_socket(net, port);
}

void recomp_netplay_close(RecompNetplay *net)
{
    if ((SOCKET)net->socket != INVALID_SOCKET) closesocket((SOCKET)net->socket);
    net->socket = (uintptr_t)INVALID_SOCKET;
}

void netplay_send_raw(const RecompNetplay *net, uint32_t address, uint16_t port,
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

/* Sessions whose packets belong to another owner's socket: a Live fight's,
   which live_transport.c routes to the opponent (see netplay_wire.h). */
enum { ROUTED_SESSIONS = 4u };
static struct {
    RecompNetplay *net;
    RecompNetplayRoute route;
} routes[ROUTED_SESSIONS];

static const RecompNetplayRoute *route_of(const RecompNetplay *net)
{
    for (unsigned i = 0u; i < ROUTED_SESSIONS; ++i)
        if (routes[i].net == net) return &routes[i].route;
    return NULL;
}

bool netplay_set_route(RecompNetplay *net, const RecompNetplayRoute *route)
{
    for (unsigned i = 0u; i < ROUTED_SESSIONS; ++i) {
        if (routes[i].net != net) continue;
        if (route == NULL)
            routes[i].net = NULL;
        else
            routes[i].route = *route;
        return true;
    }
    if (route == NULL) return true;
    for (unsigned i = 0u; i < ROUTED_SESSIONS; ++i) {
        if (routes[i].net != NULL) continue;
        routes[i].net = net;
        routes[i].route = *route;
        return true;
    }
    return false;
}

/* A session packet: through the route's owner when Live bound one, else
   straight to the peer this socket holds. */
static void send_session(const RecompNetplay *net, const uint8_t *packet, size_t size)
{
    const RecompNetplayRoute *route = route_of(net);
    if (route != NULL)
        route->send(net, packet, size);
    else
        netplay_send_raw(net, net->peer_address, net->peer_port, packet, size);
}

void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *s, int32_t advantage)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET];
    const uint64_t now = netplay_now_ms();
    for (; net->pads_stamped < s->local_count; ++net->pads_stamped)
        net->pad_sent_ms[net->pads_stamped % RECOMP_NETPLAY_MAX_PADS] = now;
    send_session(net, packet, recomp_netplay_encode(net, s, advantage, packet));
}

void recomp_netplay_send_goodbye(const RecompNetplay *net)
{
    uint8_t packet[4];
    put32(packet, GOODBYE_MAGIC);
    for (int i = 0; i < 3; ++i) send_session(net, packet, sizeof packet);
}

/* The next queued packet's size, or -1 when none is left. A routed session
   asks its owner's queue; otherwise the socket's own datagrams arrive here. */
int netplay_receive_from(RecompNetplay *net, uint8_t *packet, size_t capacity,
    struct sockaddr_in *from)
{
    const RecompNetplayRoute *route = route_of(net);
    if (route != NULL) return route->receive(net, packet, capacity, from);
    uint8_t raw[RECOMP_NETPLAY_MAX_PACKET + 1u];
    for (;;) {
        SocketLength from_size = sizeof *from;
        int size = (int)recvfrom((SOCKET)net->socket, (char *)raw, (int)sizeof raw, 0,
            (struct sockaddr *)from, &from_size);
        /* Windows reports an ICMP port-unreachable from an earlier send as a
           failed receive, and a datagram longer than any packet here as a
           failure where others cut it. */
        if (size < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue;
#endif
            return -1;
        }
        if (from->sin_family != AF_INET) continue;
        if ((size_t)size > capacity) size = (int)capacity;
        memcpy(packet, raw, (size_t)size);
        return size;
    }
}

bool netplay_from_peer(const RecompNetplay *net, const struct sockaddr_in *from)
{
    return from->sin_addr.s_addr == net->peer_address && from->sin_port == net->peer_port;
}

uint64_t netplay_now_ms(void)
{
    /* Monotonic, so a system clock change cannot end a session or stall its retries. */
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

bool recomp_netplay_match_server(RecompNetplay *net, const char *host, uint16_t port)
{
    RecompNetplayMatch *m = &net->match;
    if (port == 0u) return false;
    if (!resolve(host, &m->server_address)) {
        m->server_port = 0u;
        return false;
    }
    m->server_port = htons(port);
    return true;
}

void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *s)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    int size;
    while ((size = netplay_receive_from(net, packet, sizeof packet, &from)) >= 0)
        if (netplay_from_peer(net, &from)) (void)recomp_netplay_decode(net, s, packet, (size_t)size);
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
