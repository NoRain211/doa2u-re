#ifndef DOA2U_RECOMP_NETPLAY_WIRE_H
#define DOA2U_RECOMP_NETPLAY_WIRE_H

/* What netplay.c and live_transport.c share: the socket's platform types, the
   session packets' constants and byte order, and the transport under both.
   Callers use netplay.h. */
#include "netplay.h"

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
};

static inline void put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static inline uint32_t get32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static inline void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static inline uint16_t get16(const uint8_t *in)
{
    return (uint16_t)(in[0] | in[1] << 8);
}

/* Monotonic milliseconds. */
uint64_t netplay_now_ms(void);
void netplay_send_raw(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size);
/* The next queued packet's size, or -1 when none is left; a routed session
   asks its owner's queue instead of the socket. */
int netplay_receive_from(RecompNetplay *net, uint8_t *packet, size_t capacity,
    struct sockaddr_in *from);
bool netplay_from_peer(const RecompNetplay *net, const struct sockaddr_in *from);

/* A session that does not own the socket takes its route from its owner:
   live_transport.c binds the one for a Live fight, whose packets must go to
   the opponent over the same socket the matchmaker and the game use, and
   whose receives come from that transport's rollback queue. With no route
   set, netplay.c sends to and receives from its socket as before. */
typedef struct RecompNetplayRoute {
    void (*send)(const RecompNetplay *net, const uint8_t *packet, size_t size);
    int (*receive)(RecompNetplay *net, uint8_t *packet, size_t capacity, struct sockaddr_in *from);
} RecompNetplayRoute;
/* Binds one RecompNetplay's route, or clears it with NULL; false when the
   table of routed sessions is full. */
bool netplay_set_route(RecompNetplay *net, const RecompNetplayRoute *route);

#endif
