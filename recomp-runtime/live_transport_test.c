/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic test for live_transport.c: the guest's winsock/XNet transport
   thunks, the matchmaker's cookie and relay handling, per-peer punching and
   queueing, the session's receive log across a rewind, the type-9 drop, and a
   bound rollback RecompNetplay carrying real netplay packets.

   The transport is one instance per runner process, so the far
   side is a scripted pair of loopback sockets here: one stands in for
   tools/matchmaker.py, the other for the other player. Both speak the bytes a
   second runner would send (the "D2MM" framing netplay.c uses, "D2GD"
   datagrams, and recomp_netplay_encode's own packets), so the transport is
   driven the way live_client.c and the game drive it. */

#include "live_transport.h"

#include "netplay_wire.h"
#include "kernel_abi.h"
#include "live_backend.h"
#include "live_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* live_xapi.h declares this with the rest of the guest's thunks; it is
   declared here so the shared Live headers stay unchanged, as live_transport.c
   does. */
RecompFunction recomp_live_transport_lookup(uint32_t guest_address);

/* The guest's memory: enough for the buffers and stack the thunks are called
   with. Addresses here are synthetic, as in netplay_test.c. */
enum {
    GUEST_BASE = 0x00400000u,
    GUEST_BYTES = 0x00010000u,
    STACK_TOP = GUEST_BASE + 0x8000u,
    DATA = GUEST_BASE + 0x0100u,
    TO_ADDR = GUEST_BASE + 0x1000u,
    FROM = GUEST_BASE + 0x1040u,
    FROM_LENGTH = GUEST_BASE + 0x1060u,
    XNADDR = GUEST_BASE + 0x1100u,
    XNADDR_OUT = GUEST_BASE + 0x1180u,
    INADDR_OUT = GUEST_BASE + 0x1200u,
    KID_OUT = GUEST_BASE + 0x1240u,
    RANDOM_OUT = GUEST_BASE + 0x1280u,
    PORT_1000 = 1000u,
};
static uint8_t guest_ram[GUEST_BYTES];

RecompRuntime recomp_runtime;
/* No fast-RAM window in this synthetic map; every access uses recomp_memory. */
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= GUEST_BASE && address + width <= GUEST_BASE + GUEST_BYTES)
        return &guest_ram[address - GUEST_BASE];
    fprintf(stderr, "FAIL guest memory %08x+%zu\n", (unsigned)address, width);
    abort();
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

/* --- the backend live_xapi.c talks to, with the connect counter --- */
static const RecompLiveBackend *backend;
static unsigned connects;
static uint32_t connected_ina;

const RecompLiveBackend *recomp_live_backend(void)
{
    return backend;
}

void recomp_live_backend_set(const RecompLiveBackend *value)
{
    backend = value;
}

static uint32_t connect_log[16]; /* the players asked for, by connects modulo 16 */

static void connect_peer(void *self, uint32_t virtual_ina)
{
    (void)self;
    connect_log[connects % 16u] = virtual_ina;
    ++connects;
    connected_ina = virtual_ina;
}

/* --- the rollback session's frame state, which netplay_session.cpp owns --- */
static bool rollback_active;
static bool rollback_hidden;
static uint32_t rollback_frame;

bool recomp_live_rollback_active(void)
{
    return rollback_active;
}

bool recomp_live_rollback_hidden(void)
{
    return rollback_hidden;
}

uint32_t recomp_live_rollback_frame(void)
{
    return rollback_frame;
}

/* --- harness --- */
static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

/* Pushes stdcall arguments and the return address, as the guest's CALL does,
   and reports EAX. ESP must come back where it started: every thunk is
   stdcall with the number of arguments its guest prototype has. */
static uint32_t call(RecompFunction function, const uint32_t *arguments, unsigned count)
{
    uint32_t esp = STACK_TOP;
    for (unsigned i = count; i > 0u; --i) {
        esp -= 4u;
        *recomp_memory_u32(esp) = arguments[i - 1u];
    }
    esp -= 4u;
    *recomp_memory_u32(esp) = 0u;
    recomp_runtime.registers.esp = esp;
    recomp_runtime.registers.eax = 0u;
    function();
    const bool balanced = recomp_runtime.registers.esp == STACK_TOP;
    if (!balanced) {
        fprintf(stderr, "FAIL stdcall cleanup for %u arguments\n", count);
        ++failures;
    }
    return recomp_runtime.registers.eax;
}

/* --- the two scripted sockets --- */
static SOCKET server_socket = INVALID_SOCKET;
static SOCKET peer_socket = INVALID_SOCKET;
/* A source that was never introduced and is not anyone's path. */
static SOCKET intruder_socket = INVALID_SOCKET;

static bool bind_loopback(SOCKET *out, struct sockaddr_in *address)
{
    *out = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (*out == INVALID_SOCKET) return false;
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(*out, (const struct sockaddr *)&local, sizeof local) != 0) return false;
    SocketLength length = sizeof *address;
    return getsockname(*out, (struct sockaddr *)address, &length) == 0;
}

static int read_socket(SOCKET handle, uint8_t *packet, size_t capacity, struct sockaddr_in *from,
    unsigned milliseconds)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(handle, &readable);
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = (long)milliseconds * 1000L;
    if (select((int)handle + 1, &readable, NULL, NULL, &timeout) <= 0) return -1;
    SocketLength length = sizeof *from;
    return (int)recvfrom(handle, (char *)packet, (int)capacity, 0, (struct sockaddr *)from, &length);
}

static void send_socket(SOCKET handle, const struct sockaddr_in *to, const uint8_t *packet, size_t size)
{
    (void)sendto(handle, (const char *)packet, (int)size, 0, (const struct sockaddr *)to, sizeof *to);
}

/* The matchmaker's name lookup, made slow: 300 ms, then loopback or a failure.
   It runs on the lookup thread, so both values it shares with main are atomic. */
#ifdef _WIN32
static volatile LONG lookups, lookup_fails;
static int lookups_seen(void) { return (int)InterlockedCompareExchange(&lookups, 0, 0); }
static void fail_lookups(bool fail) { InterlockedExchange(&lookup_fails, fail ? 1 : 0); }
#else
#include <stdatomic.h>
static atomic_int lookups;
static atomic_bool lookup_fails;
static int lookups_seen(void) { return atomic_load(&lookups); }
static void fail_lookups(bool fail) { atomic_store(&lookup_fails, fail); }
#endif

static bool slow_resolver(const char *host, uint32_t *address)
{
    (void)host;
#ifdef _WIN32
    Sleep(300);
    *address = htonl(0x7f000001u);
    InterlockedIncrement(&lookups);
    return InterlockedCompareExchange(&lookup_fails, 0, 0) == 0;
#else
    usleep(300000);
    *address = htonl(0x7f000001u);
    atomic_fetch_add(&lookups, 1);
    return !atomic_load(&lookup_fails);
#endif
}

/* Waits up to 3 s for the resolver to have run this many times. */
static int lookups_after_wait(int count)
{
    const uint64_t until = netplay_now_ms() + 3000u;
    while (lookups_seen() < count && netplay_now_ms() < until) {
    }
    return lookups_seen();
}

/* Lets the transport's millisecond clock move on, so two sends are told apart. */
static void spin_ms(uint64_t milliseconds)
{
    const uint64_t until = netplay_now_ms() + milliseconds;
    while (netplay_now_ms() < until) {
    }
}

enum {
    MATCH_ASKED = 22u, /* the matchmaker's header, nonce and cookie */
    GUEST_HEADER = 11u,
    RELAY_KIND = 9u,
    LIVE_RELAY_KIND = 34u,
    COOKIE_KIND = 10u,
    DONE_KIND = 25u,
};

static size_t match_request(uint8_t *out, uint8_t kind, uint32_t id)
{
    memset(out, 0, MATCH_ASKED + 4u);
    memcpy(out, "D2MM", 4u);
    out[4] = 2u;
    out[5] = kind;
    out[MATCH_ASKED] = (uint8_t)(id >> 24);
    out[MATCH_ASKED + 1u] = (uint8_t)(id >> 16);
    out[MATCH_ASKED + 2u] = (uint8_t)(id >> 8);
    out[MATCH_ASKED + 3u] = (uint8_t)id;
    return MATCH_ASKED + 4u;
}

static size_t match_answer(uint8_t *out, uint8_t kind, const uint8_t nonce[8], uint32_t value)
{
    /* An answer carries the nonce, then its body: no cookie. */
    memset(out, 0, 6u + 8u + 4u);
    memcpy(out, "D2MM", 4u);
    out[4] = 2u;
    out[5] = kind;
    memcpy(out + 6u, nonce, 8u);
    out[14] = (uint8_t)(value >> 24);
    out[15] = (uint8_t)(value >> 16);
    out[16] = (uint8_t)(value >> 8);
    out[17] = (uint8_t)value;
    return 6u + 8u + 4u;
}

static size_t match_cookie(uint8_t *out, const uint8_t nonce[8], const uint8_t cookie[8])
{
    memset(out, 0, MATCH_ASKED);
    memcpy(out, "D2MM", 4u);
    out[4] = 2u;
    out[5] = COOKIE_KIND;
    memcpy(out + 6u, nonce, 8u);
    memcpy(out + 14u, cookie, 8u);
    return MATCH_ASKED;
}

/* What the server forwards: its header, the destination's nonce, then the
   guest datagram (no cookie: that is a request field). */
static size_t match_relay(uint8_t *out, const uint8_t nonce[8], const uint8_t *body, size_t size)
{
    memcpy(out, "D2MM", 4u);
    out[4] = 2u;
    out[5] = RELAY_KIND;
    memcpy(out + 6u, nonce, 8u);
    memcpy(out + 14u, body, size);
    return 14u + size;
}

/* The guest datagram a second runner's transport would put on the wire. */
static size_t guest_wire(uint8_t *out, uint32_t virtual_ina, uint16_t port, const uint8_t *data, size_t size)
{
    memcpy(out, "D2GD", 4u);
    out[4] = 1u;
    out[5] = (uint8_t)(virtual_ina >> 24); /* big-endian, as the transport sends it */
    out[6] = (uint8_t)(virtual_ina >> 16);
    out[7] = (uint8_t)(virtual_ina >> 8);
    out[8] = (uint8_t)virtual_ina;
    memcpy(out + 9u, &port, 2u);
    if (size != 0u) memcpy(out + GUEST_HEADER, data, size);
    return GUEST_HEADER + size;
}

/* A punch a second runner's transport would send: kind 2, then the pair
   token, and a ninth byte of 1 when it answers a punch. */
static size_t punch_wire(uint8_t *out, uint32_t virtual_ina, uint16_t port, const uint8_t token[8])
{
    const size_t size = guest_wire(out, virtual_ina, port, token, 8u);
    out[4] = 2u;
    return size;
}

/* The kind of the last datagram parse_guest read: 1 a game datagram, 2 a punch. */
static uint8_t parsed_kind;

/* A client-to-server LIVE_RELAY (kind 34): destination, then the guest
   datagram the sender would have put on the direct path. */
static bool parse_guest(const uint8_t *packet, size_t size, uint32_t *virtual_ina, uint16_t *port,
    const uint8_t **payload, size_t *payload_size);

static bool parse_live_relay(const uint8_t *packet, size_t size, uint32_t *destination, uint32_t *virtual_ina,
    uint16_t *port, const uint8_t **payload, size_t *payload_size)
{
    if (size < MATCH_ASKED + 4u + GUEST_HEADER || memcmp(packet, "D2MM", 4u) != 0 || packet[4] != 2u ||
        packet[5] != LIVE_RELAY_KIND)
        return false;
    const uint8_t *body = packet + MATCH_ASKED;
    *destination = (uint32_t)body[0] << 24 | (uint32_t)body[1] << 16 | (uint32_t)body[2] << 8 | body[3];
    return parse_guest(body + 4u, size - MATCH_ASKED - 4u, virtual_ina, port, payload, payload_size);
}

/* The next relayed game datagram the server gets, past the relayed punches a
   switch to the relay sends first. */
static int read_relayed(SOCKET handle, uint8_t *packet, size_t capacity, struct sockaddr_in *from)
{
    for (;;) {
        const int size = read_socket(handle, packet, capacity, from, 1000u);
        uint32_t destination = 0u, virtual_ina = 0u;
        uint16_t port = 0u;
        const uint8_t *payload = NULL;
        size_t payload_size = 0u;
        if (size <= 0 || !parse_live_relay(packet, (size_t)size, &destination, &virtual_ina, &port, &payload,
                &payload_size) || parsed_kind != 2u)
            return size;
    }
}

/* One "D2GD" the transport sent: its sender, port and payload. */
static bool parse_guest(const uint8_t *packet, size_t size, uint32_t *virtual_ina, uint16_t *port,
    const uint8_t **payload, size_t *payload_size)
{
    if (size < GUEST_HEADER || memcmp(packet, "D2GD", 4u) != 0 || (packet[4] != 1u && packet[4] != 2u)) return false;
    parsed_kind = packet[4];
    *virtual_ina = (uint32_t)packet[5] << 24 | (uint32_t)packet[6] << 16 | (uint32_t)packet[7] << 8 | packet[8];
    memcpy(port, packet + 9u, 2u);
    *payload = packet + GUEST_HEADER;
    *payload_size = size - GUEST_HEADER;
    return true;
}

/* This socket's nonce, which a matchmaker request carries. */
static const uint8_t SOCKET_NONCE[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
/* The pair token the matchmaker hands both players of one pairing in LPEER,
   which their punches repeat. */
static const uint8_t PAIR_TOKEN[8] = {8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u};
/* The pair token of the player whose endpoint nobody answers. */
static const uint8_t RELAY_TOKEN[8] = {33u, 34u, 35u, 36u, 37u, 38u, 39u, 40u};
/* The server's proof of this socket's address; the transport must store it and
   carry it on every later request. */
static const uint8_t COOKIE[8] = {9u, 8u, 7u, 6u, 5u, 4u, 3u, 2u};

static uint8_t handler_packet[1400];
static size_t handler_size;
static unsigned handler_count;

static void on_server(const uint8_t *packet, size_t size)
{
    ++handler_count;
    handler_size = size < sizeof handler_packet ? size : sizeof handler_packet;
    memcpy(handler_packet, packet, handler_size);
}

/* --- the transport's guest side, as the game calls it --- */
static int guest_read(uint8_t *data, size_t capacity, uint32_t *virtual_ina, uint16_t *port)
{
    return recomp_live_transport_guest_receive(data, capacity, virtual_ina, port);
}

/* The game datagrams recvfrom passes, as the game's senders build them, and
   the shapes its handlers would read past their buffers. */
static void check_game_datagrams(void)
{
    uint8_t d[0x400];
    memset(d, 0, sizeof d);
    check("an empty type 3 passes", (d[2] = 3u, recomp_live_game_datagram_ok(d, 3u)));
    check("a type 3 with a body is dropped", !recomp_live_game_datagram_ok(d, 4u));
    check("a header alone is dropped", !recomp_live_game_datagram_ok(d, 2u));
    check("type 4 has no handler", (d[2] = 4u, !recomp_live_game_datagram_ok(d, 3u)));
    check("type 22 has no handler", (d[2] = 22u, !recomp_live_game_datagram_ok(d, 3u)));

    memset(d, 0, sizeof d);
    d[2] = 0u;
    memcpy(d + 3u, "player", 7u);
    check("a join passes", recomp_live_game_datagram_ok(d, 0x3cu));
    memset(d + 3u, 'A', 16u);
    check("a join whose name runs on is dropped", !recomp_live_game_datagram_ok(d, 0x3cu));
    memset(d + 3u, 0, 16u);
    memset(d + 0x13u, 'A', 32u);
    check("a join whose wide name runs on is dropped", !recomp_live_game_datagram_ok(d, 0x3cu));

    /* The player list: a count, then that many 0x5e-byte slots. */
    memset(d, 0, sizeof d);
    d[2] = 10u;
    d[3] = 8u;
    check("eight players pass", recomp_live_game_datagram_ok(d, 8u + 8u * 0x5eu));
    d[3] = 9u;
    check("nine players are dropped", !recomp_live_game_datagram_ok(d, 8u + 9u * 0x5eu));
    d[3] = 2u;
    check("a count the length disagrees with is dropped", !recomp_live_game_datagram_ok(d, 8u + 3u * 0x5eu));
    memset(d + 8u + 0x5eu + 0x2cu, 'A', 16u);
    check("a slot whose name runs on is dropped", !recomp_live_game_datagram_ok(d, 8u + 2u * 0x5eu));
    memset(d, 0, sizeof d);
    d[2] = 12u;
    d[0x1b] = 1u;
    check("a session key and one player pass", recomp_live_game_datagram_ok(d, 0x20u + 0x5eu));
    d[0x1b] = 200u;
    check("a session key with 200 players is dropped", !recomp_live_game_datagram_ok(d, 0x329u));
    memset(d, 0, sizeof d);
    d[2] = 2u;
    d[0x34] = 1u;
    check("a joined session passes", recomp_live_game_datagram_ok(d, 0x39u + 0x5eu));
    memset(d + 3u, 'A', 34u);
    check("a session name that runs on is dropped", !recomp_live_game_datagram_ok(d, 0x39u + 0x5eu));
    memset(d, 0, sizeof d);
    d[2] = 14u;
    memset(d + 3u, 'A', 34u);
    check("a settings name that runs on is dropped", !recomp_live_game_datagram_ok(d, 0x34u));

    /* Type 15: a handler index into a 12-entry table, and its payload. */
    memset(d, 0, sizeof d);
    d[2] = 15u;
    d[0x13] = 0x14u;
    check("character select passes", recomp_live_game_datagram_ok(d, 0x15u + 0x14u));
    d[0x13] = 0u;
    check("an empty character select (not chosen yet) is dropped", !recomp_live_game_datagram_ok(d, 0x15u));
    check("an empty character select is an expected drop", recomp_live_game_datagram_expected_drop(d, 0x15u));
    check("a character select is not an expected drop", !recomp_live_game_datagram_expected_drop(d, 0x15u + 0x14u));
    d[0x13] = 0x14u;
    check("a payload the length disagrees with is dropped", !recomp_live_game_datagram_ok(d, 0x15u + 0x15u));
    check("a payload the length disagrees with is not an expected drop", !recomp_live_game_datagram_expected_drop(d, 0x15u));
    d[3] = 12u;
    check("handler 12 is dropped", !recomp_live_game_datagram_ok(d, 0x15u + 0x14u));
    d[3] = 0x41u; d[6] = 0x40u; /* the table index an attacker would aim elsewhere */
    check("a far handler is dropped", !recomp_live_game_datagram_ok(d, 0x15u + 0x14u));
    d[6] = 0u;
    d[3] = 4u;
    d[0x13] = 0x54u + 0x1du;
    check("match settings with a stage's state pass", recomp_live_game_datagram_ok(d, 0x15u + 0x54u + 0x1du));
    d[0x13] = 0x40u;
    check("match settings too short to hold their own are dropped", !recomp_live_game_datagram_ok(d, 0x15u + 0x40u));
    d[0x13] = 0x54u + 0x1eu;
    check("a stage's state past 0x1d bytes is dropped", !recomp_live_game_datagram_ok(d, 0x15u + 0x54u + 0x1eu));
    d[0x13] = 0u;
    check("empty match settings are dropped", !recomp_live_game_datagram_ok(d, 0x15u));
    check("empty match settings are not an expected drop", !recomp_live_game_datagram_expected_drop(d, 0x15u));
    d[3] = 5u;
    check("an empty settings answer (not ready) is dropped", !recomp_live_game_datagram_ok(d, 0x15u));
    check("an empty settings answer is an expected drop", recomp_live_game_datagram_expected_drop(d, 0x15u));
    d[0x13] = 0x16u;
    check("a settings answer passes", recomp_live_game_datagram_ok(d, 0x15u + 0x16u));
    d[0x13] = 0x15u;
    check("a settings answer of another size is dropped", !recomp_live_game_datagram_ok(d, 0x15u + 0x15u));

    /* Type 9: the batch that starts lockstep names frames and delay. */
    memset(d, 0, sizeof d);
    d[2] = 9u;
    check("a lockstep batch passes", recomp_live_game_datagram_ok(d, 0xfu + 2u * 20u));
    check("five frames a batch are dropped", !recomp_live_game_datagram_ok(d, 0xfu + 5u * 20u));
    memset(d + 3u + 8u, 0xff, 4u);
    d[3u + 0x10u] = 2u;
    d[3u + 0x11u] = 3u;
    check("a lockstep start passes", recomp_live_game_datagram_ok(d, 0xfu + 2u * 20u));
    d[3u + 0x10u] = 255u;
    check("a start with 255 frames is dropped", !recomp_live_game_datagram_ok(d, 0xfu + 2u * 20u));
    d[3u + 0x10u] = 2u;
    d[3u + 0x11u] = 200u;
    check("a start with a 200-batch delay is dropped", !recomp_live_game_datagram_ok(d, 0xfu + 2u * 20u));

    memset(d, 0, sizeof d);
    d[2] = 7u;
    d[3] = 3u;
    check("three 10-byte records pass", recomp_live_game_datagram_ok(d, 4u + 30u));
    check("a record count the length disagrees with is dropped", !recomp_live_game_datagram_ok(d, 4u + 20u));
    d[2] = 21u;
    check("a recent player's record passes", recomp_live_game_datagram_ok(d, 0x60u));
    memset(d + 0x32u, 'A', 32u);
    check("a recent player whose wide name runs on is dropped", !recomp_live_game_datagram_ok(d, 0x60u));
    memset(d, 0, sizeof d);
    /* Type 16: online_send_msg10 sends its record from byte 3, and the
       record's length at 0x17 counts all of it. */
    d[2] = 16u;
    d[0x17] = 0x19u;
    check("a match message passes", recomp_live_game_datagram_ok(d, 3u + 0x19u));
    check("a match message the length disagrees with is dropped", !recomp_live_game_datagram_ok(d, 3u + 0x1au));
    d[0x13] = 5u; d[0x16] = 0x80u; /* subtype 5 with its high flag */
    d[0x17] = 0x9du; d[0x18] = 0x01u;
    check("a whole batch record passes", recomp_live_game_datagram_ok(d, 3u + 0x19du));
    d[0x17] = 0x19u; d[0x18] = 0u;
    check("a batch record shorter than its subtype's is dropped", !recomp_live_game_datagram_ok(d, 3u + 0x19u));
    d[0x13] = 2u; d[0x16] = 0u;
    d[0x17] = 0x1du; d[0x19] = 1u;
    check("a length with a high half is dropped", !recomp_live_game_datagram_ok(d, 3u + 0x1du));
    d[0x19] = 0u;
    check("a settings record passes", recomp_live_game_datagram_ok(d, 3u + 0x1du));
    d[0x13] = 11u;
    d[0x17] = 0x18u;
    check("an unknown subtype's bare header passes", recomp_live_game_datagram_ok(d, 3u + 0x18u));
    d[0x17] = 0x15u;
    check("a record shorter than its header is dropped", !recomp_live_game_datagram_ok(d, 3u + 0x15u));
    d[2] = 3u;
    check("more than the game's buffer is dropped", !recomp_live_game_datagram_ok(d, 0x32au));
}

/* The Live log's per-type counts. */
static void check_type_counts(void)
{
    uint32_t counts[23];
    char text[64];
    memset(counts, 0, sizeof counts);
    recomp_live_format_type_counts(text, sizeof text, counts, 23u);
    check("no datagrams read as none", strcmp(text, "none") == 0);
    counts[9] = 600u;
    counts[22] = 3u;
    counts[0] = 1u;
    const size_t length = recomp_live_format_type_counts(text, sizeof text, counts, 23u);
    check("the counts read type by type, the last slot as ?", strcmp(text, "0x1 9x600 ?x3") == 0);
    check("the length is the text's", length == strlen(text));
    char tight[6];
    recomp_live_format_type_counts(tight, sizeof tight, counts, 23u);
    check("a short buffer stays terminated", strlen(tight) < sizeof tight);
}

int main(void)
{
    check_game_datagrams();
    check_type_counts();
#ifdef _WIN32
    WSADATA winsock;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        fprintf(stderr, "FAIL winsock\n");
        return 1;
    }
#endif
    /* Virtual addresses are the numbers the matchmaker's LOGGED answer and
       LPEER carry (10.0.0.1 is 0x0a000001). */
    const uint32_t ina_a = 0x0a000001u; /* 10.0.0.1, this runner */
    const uint32_t ina_b = 0x0a000002u; /* the other player */
    const uint32_t ina_c = 0x0a000003u; /* a relayed player */
    const uint32_t ina_d = 0x0a000004u; /* an endpoint nobody answers */
    const uint32_t ina_e = 0x0a000005u; /* no endpoint at all */
    const uint32_t ina_f = 0x0a000006u; /* an address nobody introduced */
    const uint32_t ina_g = 0x0a000007u; /* re-paired while this side missed the LPEER */

    struct sockaddr_in server_address, peer_address, client_endpoint, intruder_address;
    memset(&client_endpoint, 0, sizeof client_endpoint);
    if (!bind_loopback(&server_socket, &server_address) || !bind_loopback(&peer_socket, &peer_address) ||
        !bind_loopback(&intruder_socket, &intruder_address)) {
        fprintf(stderr, "FAIL loopback sockets\n");
        return 1;
    }
    /* What live_xapi.c would install: a backend whose only Live-to-transport
       call occurs here, the pairing request. */
    static RecompLiveBackend fake_backend;
    fake_backend.connect = connect_peer;
    recomp_live_backend_set(&fake_backend);

    /* --- open, and the matchmaker's cookie handling --- */
    /* A caller may register its handler and address before the socket exists;
       opening must keep both. */
    recomp_live_transport_set_server_handler(on_server);
    recomp_live_transport_set_local(ina_a);
    check("transport opens", recomp_live_transport_open("127.0.0.1", ntohs(server_address.sin_port)));
    /* The game's socket and bind: one UDP socket on port 1000. */
    uint32_t args[6];
    args[0] = 2u; args[1] = 2u; args[2] = 0u;
    check("socket returns the guest handle", call(recomp_live_transport_lookup(0x00381019u), args, 3u) == 0x100u);
    uint8_t *name = recomp_memory(TO_ADDR, 16u);
    memset(name, 0, 16u);
    name[0] = 2u;
    const uint16_t port_1000 = htons(PORT_1000);
    memcpy(name + 2u, &port_1000, 2u);
    args[0] = 0x100u; args[1] = TO_ADDR; args[2] = 16u;
    check("bind", call(recomp_live_transport_lookup(0x00381054u), args, 3u) == 0u);

    uint8_t packet[1400];
    /* Opening only stores the server: nothing binds or goes out before the
       first request, so a player who never logs on meets no firewall prompt. */
    recomp_live_transport_pump(netplay_now_ms());
    check("a guest send before the first request is refused",
        recomp_live_transport_guest_send(ina_b, port_1000, (const uint8_t *)"early", 5u) < 0 && connects == 0u);
    uint8_t unopened[8];
    recomp_live_transport_nonce(unopened);
    bool unbound = true;
    for (unsigned i = 0u; i < 8u; ++i) unbound = unbound && unopened[i] == 0u;
    check("opening binds no socket", unbound);
    check("opening sends nothing", read_socket(server_socket, packet, sizeof packet, &client_endpoint, 50u) < 0);
    uint8_t request[MATCH_ASKED + 4u];
    const size_t request_size = match_request(request, 12u, 1u);
    memcpy(request + 6u, SOCKET_NONCE, sizeof SOCKET_NONCE); /* the client writes its nonce; the transport overwrites both */
    recomp_live_transport_send_server(request, request_size);
    int size = read_socket(server_socket, packet, sizeof packet, &client_endpoint, 1000u);
    check("the matchmaker hears the request", size == (int)request_size && memcmp(packet, "D2MM", 4u) == 0 &&
        packet[5] == 12u && packet[MATCH_ASKED + 3u] == 1u);
    uint8_t nonce[8];
    memcpy(nonce, packet + 6u, sizeof nonce);
    uint8_t socket_nonce[8];
    recomp_live_transport_nonce(socket_nonce);
    check("the request carries the socket's nonce", memcmp(nonce, socket_nonce, sizeof nonce) == 0);
    bool cookie_zero = true;
    for (unsigned i = 0u; i < 8u; ++i) cookie_zero = cookie_zero && packet[14u + i] == 0u;
    check("no cookie before the server sends one", cookie_zero);

    /* The server drops the first request for want of a cookie; the transport
       stores it and says the request again with it. */
    const size_t cookie_size = match_cookie(packet, nonce, COOKIE);
    send_socket(server_socket, &client_endpoint, packet, cookie_size);
    recomp_live_transport_pump(netplay_now_ms());
    size = read_socket(server_socket, packet, sizeof packet, &client_endpoint, 1000u);
    check("the request comes again with the cookie", size == (int)request_size &&
        memcmp(packet + 14u, COOKIE, 8u) == 0 && memcmp(packet + 6u, nonce, 8u) == 0);

    /* An answer for the Live client, and a forged one without the nonce. */
    size = (int)match_answer(packet, DONE_KIND, nonce, 0u);
    send_socket(server_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms());
    check("the server handler sees the answer", handler_count == 1u && handler_size == (size_t)size &&
        handler_packet[5] == DONE_KIND);
    uint8_t forged[8];
    memcpy(forged, nonce, sizeof forged);
    forged[0] ^= 0xffu;
    size = (int)match_answer(packet, DONE_KIND, forged, 0u);
    send_socket(server_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms());
    check("an answer without the nonce is dropped", handler_count == 1u);

    /* --- a datagram to a player with no route is queued, and asks for a pairing --- */
    check("first send is taken", recomp_live_transport_guest_send(ina_b, port_1000, (const uint8_t *)"queued", 6u) == 6);
    check("the backend is asked to pair once", connects == 1u && connected_ina == ina_b);
    check("not reachable while queued", !recomp_live_transport_reachable(ina_b));
    recomp_live_transport_pump(netplay_now_ms());
    struct sockaddr_in source;
    check("nothing reaches an unintroduced peer", read_socket(peer_socket, packet, sizeof packet, &source, 50u) < 0);

    /* The matchmaker's LPEER: punch to the endpoint it saw, carrying the pair
       token both players were given so only this pairing's punch counts. */
    RecompLivePeerIntro intro;
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = ina_b;
    intro.public_address = peer_address.sin_addr.s_addr;
    intro.public_port = peer_address.sin_port;
    memcpy(intro.token, PAIR_TOKEN, sizeof intro.token);
    recomp_live_transport_peer(&intro);
    check("still punching after LPEER", !recomp_live_transport_reachable(ina_b));
    recomp_live_transport_pump(netplay_now_ms() + 1000u);
    uint32_t from_ina = 0u;
    uint16_t from_port = 0u;
    const uint8_t *payload = NULL;
    size_t payload_size = 0u;
    uint8_t data[1200];
    size = read_socket(peer_socket, packet, sizeof packet, &source, 1000u);
    check("the punch names this player", size > 0 && parse_guest(packet, (size_t)size, &from_ina, &from_port,
        &payload, &payload_size) && from_ina == ina_a && payload_size == 8u && parsed_kind == 2u &&
        memcmp(payload, PAIR_TOKEN, sizeof PAIR_TOKEN) == 0);

    /* The same introduction again says nothing new: a punch already under way
       must not start over (the next one is due 100 ms after the last). */
    recomp_live_transport_peer(&intro);
    recomp_live_transport_pump(netplay_now_ms() + 1000u);
    check("an identical LPEER does not restart the punch",
        read_socket(peer_socket, packet, sizeof packet, &source, 0u) < 0);

    /* A punch carrying another pairing's token proves nothing. */
    size = (int)punch_wire(packet, ina_b, port_1000, SOCKET_NONCE);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms() + 1500u);
    check("a punch with another pairing's token does not connect", !recomp_live_transport_reachable(ina_b));
    while (read_socket(peer_socket, packet, sizeof packet, &source, 0u) > 0) {
    }

    /* The peer's own punch proves the path, and the queued datagram goes out once. */
    size = (int)punch_wire(packet, ina_b, port_1000, PAIR_TOKEN);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms() + 2000u);
    check("the peer is reachable after its punch", recomp_live_transport_reachable(ina_b));
    size = read_socket(peer_socket, packet, sizeof packet, &source, 1000u);
    check("its punch is answered with the pair token", size > 0 && parse_guest(packet, (size_t)size, &from_ina,
        &from_port, &payload, &payload_size) && payload_size == 9u && memcmp(payload, PAIR_TOKEN, 8u) == 0 &&
        payload[8] == 1u && parsed_kind == 2u);
    size = read_socket(peer_socket, packet, sizeof packet, &source, 1000u);
    check("the queued datagram arrives once", size > 0 && parse_guest(packet, (size_t)size, &from_ina, &from_port,
        &payload, &payload_size) && memcmp(payload, "queued", 6u) == 0 && payload_size == 6u && parsed_kind == 1u);

    /* The kind, not the bytes, tells a punch from the game's own datagram: an
       8-byte game datagram equal to the token still reaches the game, and a
       punch never does. */
    size = (int)guest_wire(packet, ina_b, port_1000, PAIR_TOKEN, sizeof PAIR_TOKEN);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("a game datagram like a punch reaches the game", guest_read(data, sizeof data, &from_ina, &from_port) == 8 &&
        memcmp(data, PAIR_TOKEN, 8u) == 0);
    size = (int)punch_wire(packet, ina_b, port_1000, PAIR_TOKEN);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("a punch never reaches the game", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    while (read_socket(peer_socket, packet, sizeof packet, &source, 50u) > 0) { /* its answer */
    }

    /* A punch with the other token must not move a path that already works. */
    size = (int)punch_wire(packet, ina_b, port_1000, SOCKET_NONCE);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms() + 2100u);
    size = (int)guest_wire(packet, ina_b, port_1000, (const uint8_t *)"stolen", 6u);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    check("a wrong token's path is not the peer's", guest_read(data, sizeof data, &from_ina, &from_port) < 0);

    /* A datagram from a source that is not that path is not the peer's. */
    size = (int)guest_wire(packet, ina_b, port_1000, (const uint8_t *)"forged", 6u);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    check("a forged datagram is dropped", guest_read(data, sizeof data, &from_ina, &from_port) < 0);

    /* An address nobody introduced never claims a slot. */
    size = (int)guest_wire(packet, ina_f, port_1000, (const uint8_t *)"stranger", 8u);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    check("a datagram for an unknown address is dropped",
        guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    check("and does not claim the peer", recomp_live_transport_guest_send(ina_f, port_1000,
        (const uint8_t *)"ask", 3u) == 3 && connects == 2u && connected_ina == ina_f);

    /* --- game datagrams both ways --- */
    size = (int)guest_wire(packet, ina_b, port_1000, (const uint8_t *)"hello", 5u);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("a datagram from the peer arrives", guest_read(data, sizeof data, &from_ina, &from_port) == 5 &&
        memcmp(data, "hello", 5u) == 0 && from_ina == ina_b && from_port == port_1000);
    check("this side sends to the peer", recomp_live_transport_guest_send(ina_b, port_1000,
        (const uint8_t *)"world", 5u) == 5);
    size = read_socket(peer_socket, packet, sizeof packet, &source, 1000u);
    check("the peer receives it", size > 0 && parse_guest(packet, (size_t)size, &from_ina, &from_port, &payload,
        &payload_size) && from_ina == ina_a && memcmp(payload, "world", 5u) == 0);
    check("the peer port crosses", from_port == port_1000);

    /* A type-9 datagram is the game's own lockstep, which only a session drops. */
    const uint8_t lockstep[4] = {0u, 0u, 9u, 0u};
    size = (int)guest_wire(packet, ina_b, port_1000, lockstep, sizeof lockstep);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("outside a session a type-9 datagram arrives", guest_read(data, sizeof data, &from_ina, &from_port) == 4 &&
        data[2] == 9u);

    /* --- a session: the receive log, a rewind and the type-9 drop --- */
    rollback_active = true;
    rollback_hidden = false;
    rollback_frame = 0u;
    recomp_live_transport_session_begin();
    for (unsigned i = 0u; i < 2u; ++i) {
        const uint8_t body[2] = {(uint8_t)('r'), (uint8_t)('0' + i)};
        size = (int)guest_wire(packet, ina_b, port_1000, body, sizeof body);
        send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    }
    check("frame 0 receives both", guest_read(data, sizeof data, &from_ina, &from_port) == 2 && data[1] == '0' &&
        guest_read(data, sizeof data, &from_ina, &from_port) == 2 && data[1] == '1');
    check("frame 0 has no third", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    rollback_hidden = true;
    check("a rewind replays frame 0 in order", guest_read(data, sizeof data, &from_ina, &from_port) == 2 &&
        data[1] == '0' && guest_read(data, sizeof data, &from_ina, &from_port) == 2 && data[1] == '1');
    check("and then stops", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    rollback_hidden = false;
    rollback_frame = 1u;
    size = (int)guest_wire(packet, ina_b, port_1000, (const uint8_t *)"n1", 2u);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("frame 1 receives its datagram", guest_read(data, sizeof data, &from_ina, &from_port) == 2 &&
        memcmp(data, "n1", 2u) == 0);
    rollback_hidden = true;
    check("a rewind replays frame 1", guest_read(data, sizeof data, &from_ina, &from_port) == 2 &&
        memcmp(data, "n1", 2u) == 0 && guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    rollback_hidden = false;
    rollback_frame = 2u;
    size = (int)guest_wire(packet, ina_b, port_1000, lockstep, sizeof lockstep);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("in a session a type-9 datagram is dropped", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    rollback_hidden = true;
    check("and was not logged", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    /* A frame run again does not send again, but reports the length. */
    check("a rewind drops the frame's send", recomp_live_transport_guest_send(ina_b, port_1000,
        (const uint8_t *)"again", 5u) == 5);
    recomp_live_transport_pump(netplay_now_ms());
    check("the peer hears nothing from the rewind", read_socket(peer_socket, packet, sizeof packet, &source,
        50u) < 0);
    /* A new session counts from 0 again; the old log must not replay in it. */
    rollback_hidden = false;
    rollback_frame = 0u;
    recomp_live_transport_session_begin();
    rollback_hidden = true;
    check("a new session does not replay the last one", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    rollback_hidden = false;
    recomp_live_transport_session_end();
    rollback_active = false;

    /* --- the relay: introduced relayed, a relayed packet, and the fallback --- */
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = ina_c;
    intro.relay = true;
    recomp_live_transport_peer(&intro);
    check("a relayed introduction is reachable", recomp_live_transport_reachable(ina_c));
    const size_t relayed = guest_wire(packet, ina_c, port_1000, (const uint8_t *)"relayed", 7u);
    size = (int)match_relay(packet + 512, nonce, packet, relayed);
    send_socket(server_socket, &client_endpoint, packet + 512, (size_t)size);
    check("a relayed datagram arrives", guest_read(data, sizeof data, &from_ina, &from_port) == 7 &&
        memcmp(data, "relayed", 7u) == 0 && from_ina == ina_c);
    check("this side relays back", recomp_live_transport_guest_send(ina_c, port_1000, (const uint8_t *)"back", 4u)
        == 4);
    size = read_relayed(server_socket, packet, sizeof packet, &client_endpoint);
    uint32_t destination = 0u;
    check("the relay names its addressee", size > (int)MATCH_ASKED && parse_live_relay(packet, (size_t)size,
        &destination, &from_ina, &from_port, &payload, &payload_size) && destination == ina_c &&
        from_ina == ina_a && memcmp(payload, "back", 4u) == 0);
    check("the relay state follows the route", recomp_live_transport_relay(ina_c) &&
        !recomp_live_transport_relay(ina_b));

    /* A rollback packet of 1,185 bytes is the largest the relay has to carry. */
    for (unsigned i = 0u; i < 1185u; ++i) data[i] = (uint8_t)(i * 7u);
    check("a full rollback packet is taken", recomp_live_transport_guest_send(ina_c, port_1000, data, 1185u) == 1185);
    size = read_relayed(server_socket, packet, sizeof packet, &client_endpoint);
    check("a 1,185-byte datagram is relayed", size == (int)(MATCH_ASKED + 4u + GUEST_HEADER + 1185u) &&
        parse_live_relay(packet, (size_t)size, &destination, &from_ina, &from_port, &payload, &payload_size) &&
        destination == ina_c && payload_size == 1185u && memcmp(payload, data, 1185u) == 0);
    check("one byte over is refused on the relay", recomp_live_transport_guest_send(ina_c, port_1000, data, 1186u) ==
        -2);
    check("and nothing reaches the relay", read_socket(server_socket, packet, sizeof packet, &client_endpoint, 100u) < 0);

    /* No answer for 5 s: the packets go through the matchmaker, as a room's do. */
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = ina_d;
    intro.public_address = htonl(INADDR_LOOPBACK);
    intro.public_port = htons(9u); /* nothing listens there */
    memcpy(intro.token, RELAY_TOKEN, sizeof intro.token);
    recomp_live_transport_peer(&intro);
    recomp_live_transport_pump(netplay_now_ms() + 6000u);
    check("the fallback relays after 5 s", recomp_live_transport_reachable(ina_d));
    check("and says so", recomp_live_transport_relay(ina_d));
    size = read_socket(server_socket, packet, sizeof packet, &client_endpoint, 1000u);
    check("and tells the other side with a relayed punch", size > (int)MATCH_ASKED &&
        parse_live_relay(packet, (size_t)size, &destination, &from_ina, &from_port, &payload, &payload_size) &&
        destination == ina_d && payload_size == 8u && parsed_kind == 2u && memcmp(payload, RELAY_TOKEN, 8u) == 0);
    check("and sends there", recomp_live_transport_guest_send(ina_d, port_1000, (const uint8_t *)"late", 4u) == 4);
    size = read_relayed(server_socket, packet, sizeof packet, &client_endpoint);
    check("the fallback's packet is relayed", size > (int)MATCH_ASKED && parse_live_relay(packet, (size_t)size,
        &destination, &from_ina, &from_port, &payload, &payload_size) && destination == ina_d &&
        memcmp(payload, "late", 4u) == 0);

    /* The other side switched to the relay with a token this side never got:
       its relayed punch turns the relay on but never reaches the game. */
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = ina_g;
    intro.public_address = htonl(INADDR_LOOPBACK);
    intro.public_port = htons(9u);
    memcpy(intro.token, RELAY_TOKEN, sizeof intro.token);
    recomp_live_transport_peer(&intro);
    size = (int)punch_wire(packet, ina_g, port_1000, (const uint8_t *)"NEWTOKEN");
    size = (int)match_relay(packet + 512, nonce, packet, (size_t)size);
    send_socket(server_socket, &client_endpoint, packet + 512, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms());
    check("an unknown relayed punch turns the relay on", recomp_live_transport_relay(ina_g));
    check("and the game never reads it", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    (void)read_socket(server_socket, packet, sizeof packet, &client_endpoint, 100u); /* this side's relayed punch */
    /* Nor does one that arrives once the relay is on. */
    size = (int)punch_wire(packet, ina_c, port_1000, (const uint8_t *)"OLDTOKEN");
    size = (int)match_relay(packet + 512, nonce, packet, (size_t)size);
    send_socket(server_socket, &client_endpoint, packet + 512, (size_t)size);
    check("a relayed punch while relaying never reaches the game",
        guest_read(data, sizeof data, &from_ina, &from_port) < 0);

    /* The game sent to g and g answered: a quiet peer that owes nothing is
       never asked for again. */
    check("send to the re-paired player", recomp_live_transport_guest_send(ina_g, port_1000, (const uint8_t *)"q", 1u)
        == 1);
    (void)read_relayed(server_socket, packet, sizeof packet, &client_endpoint);
    size = (int)guest_wire(packet, ina_g, port_1000, (const uint8_t *)"a", 1u);
    size = (int)match_relay(packet + 512, nonce, packet, (size_t)size);
    send_socket(server_socket, &client_endpoint, packet + 512, (size_t)size);
    check("the re-paired player answers", guest_read(data, sizeof data, &from_ina, &from_port) == 1);

    /* The game keeps sending to a player that never answers: after 10 s the
       transport asks the matchmaker for it again, once until the next send. */
    /* 11 s on, only the peers left with unanswered sends are asked for. */
    const unsigned connects_before = connects;
    recomp_live_transport_pump(netplay_now_ms() + 11000u);
    const unsigned asked = connects - connects_before;
    bool asked_g = false;
    for (unsigned i = connects_before; i < connects; ++i) asked_g = asked_g || connect_log[i % 16u] == ina_g;
    check("a silent player is asked for again", asked != 0u && connected_ina == ina_d);
    check("one that answered is not", !asked_g);
    recomp_live_transport_pump(netplay_now_ms() + 12000u);
    check("but not again without a new send", connects == connects_before + asked);

    /* A pairing without endpoints relays at once. */
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = ina_e;
    recomp_live_transport_peer(&intro);
    recomp_live_transport_pump(netplay_now_ms());
    check("no endpoints relays at once", recomp_live_transport_reachable(ina_e));
    check("and sends there", recomp_live_transport_guest_send(ina_e, port_1000, (const uint8_t *)"none", 4u) == 4);
    size = read_relayed(server_socket, packet, sizeof packet, &client_endpoint);
    check("the pairing's packet is relayed", size > (int)MATCH_ASKED && parse_live_relay(packet, (size_t)size,
        &destination, &from_ina, &from_port, &payload, &payload_size) && destination == ina_e &&
        memcmp(payload, "none", 4u) == 0);

    /* --- a bound rollback session over the same route --- */
    static RecompNetplay net;
    static RecompNetplay peer_net;
    static RecompRollbackSession session, peer_session;
    recomp_rollback_init(&session, 0u);
    recomp_rollback_init(&peer_session, 0u);
    memset(&net, 0, sizeof net);
    memset(&peer_net, 0, sizeof peer_net);
    RecompInputGamepad pad;
    memset(&pad, 0, sizeof pad);
    pad.buttons = 7u;
    pad.analog_buttons[3] = 0xffu;
    recomp_rollback_add_local(&session, &pad);
    check("bind the rollback session", recomp_live_transport_bind_rollback(ina_b, &net));
    check("the bound session names its opponent", net.peer_address == ina_b && net.peer_port == port_1000);
    recomp_netplay_send(&net, &session, 0);
    size = read_socket(peer_socket, packet, sizeof packet, &source, 1000u);
    check("the session's packet crosses as a guest datagram", size > 0 && parse_guest(packet, (size_t)size,
        &from_ina, &from_port, &payload, &payload_size) && from_ina == ina_a && payload_size >= 4u &&
        memcmp(payload, "D2RB", 4u) == 0 &&
        recomp_netplay_decode(&peer_net, &peer_session, payload, payload_size) && peer_session.remote_count == 1u &&
        peer_session.remote[0].buttons == 7u && peer_session.remote[0].analog_buttons[3] == 0xffu);
    memset(&pad, 0, sizeof pad);
    pad.buttons = 9u;
    recomp_rollback_add_local(&peer_session, &pad);
    const size_t peer_packet = recomp_netplay_encode(&peer_net, &peer_session, 0, data);
    size = (int)guest_wire(packet, ina_b, port_1000, data, peer_packet);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    recomp_netplay_receive(&net, &session);
    check("the session receives the peer's pads", session.remote_count == 1u && session.remote[0].buttons == 9u);
    const uint8_t goodbye[4] = {'D', '2', 'B', 'Y'};
    /* A goodbye from a source that is not the path is not the peer's, even when
       it names the bound opponent. */
    size = (int)guest_wire(packet, ina_b, port_1000, goodbye, sizeof goodbye);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    recomp_netplay_receive(&net, &session);
    check("a forged goodbye is dropped", !net.peer_left);
    size = (int)guest_wire(packet, ina_b, port_1000, goodbye, sizeof goodbye);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    recomp_netplay_receive(&net, &session);
    check("the peer's goodbye crosses the route", net.peer_left);
    recomp_live_transport_unbind_rollback();
    /* A goodbye that arrives after this side's session ended belongs to no session and
       must not reach the game's recvfrom as one of its own datagrams. */
    size = (int)guest_wire(packet, ina_b, port_1000, goodbye, sizeof goodbye);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    check("a late goodbye is not handed to the game", guest_read(data, sizeof data, &from_ina, &from_port) < 0);
    check("no session without an opponent", !recomp_live_transport_bind_rollback(ina_b, NULL));
    check("no session without a net", !recomp_live_transport_bind_rollback(0u, &net));

    /* --- the guest's winsock and XNet thunks --- */
    check("closesocket", call(recomp_live_transport_lookup(0x00381024u), args, 1u) == 0u);
    check("ioctlsocket", call(recomp_live_transport_lookup(0x0038103au), args, 3u) == 0u);
    check("shutdown", call(recomp_live_transport_lookup(0x0038102fu), args, 2u) == 0u);
    check("no such API", recomp_live_transport_lookup(0x00dead00u) == NULL);

    uint8_t *to = recomp_memory(TO_ADDR, 16u);
    memset(to, 0, 16u);
    to[0] = 2u;
    memcpy(to + 2u, &port_1000, 2u);
    memcpy(to + 4u, &ina_b, 4u);
    memcpy(recomp_memory(DATA, 5u), "thunk", 5u);
    args[0] = 0x100u; args[1] = DATA; args[2] = 5u; args[3] = 0u; args[4] = TO_ADDR; args[5] = 16u;
    check("sendto reports the length", call(recomp_live_transport_lookup(0x003810c0u), args, 6u) == 5u);
    size = read_socket(peer_socket, packet, sizeof packet, &source, 1000u);
    check("sendto reaches the peer", size > 0 && parse_guest(packet, (size_t)size, &from_ina, &from_port, &payload,
        &payload_size) && memcmp(payload, "thunk", 5u) == 0);

    /* recvfrom skips what the game's handlers would read past their buffers:
       not a game datagram, then a type 15 naming handler 12 of 12. */
    size = (int)guest_wire(packet, ina_b, port_1000, (const uint8_t *)"not", 3u);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    {
        uint8_t hostile[0x15 + 0x14];
        memset(hostile, 0, sizeof hostile);
        hostile[2] = 15u;
        hostile[3] = 12u;
        hostile[0x13] = 0x14u;
        size = (int)guest_wire(packet, ina_b, port_1000, hostile, sizeof hostile);
        send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    }
    const uint8_t via[3] = {1u, 0u, 5u}; /* type 5, as FUN_002a2c00 sends it */
    size = (int)guest_wire(packet, ina_b, port_1000, via, sizeof via);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    memset(recomp_memory(DATA, 8u), 0, 8u);
    memset(recomp_memory(FROM, 16u), 0xff, 16u);
    *recomp_memory_u32(FROM_LENGTH) = 0u;
    args[0] = 0x100u; args[1] = DATA; args[2] = 8u; args[3] = 0u; args[4] = FROM; args[5] = FROM_LENGTH;
    check("recvfrom returns the datagram", call(recomp_live_transport_lookup(0x003810a2u), args, 6u) == 3u);
    const uint8_t *guest_from = recomp_memory(FROM, 16u);
    check("recvfrom names the sender", guest_from[0] == 2u && memcmp(guest_from + 2u, &port_1000, 2u) == 0 &&
        memcmp(guest_from + 4u, &ina_b, 4u) == 0 && *recomp_memory_u32(FROM_LENGTH) == 16u);
    check("recvfrom copies the payload", memcmp(recomp_memory(DATA, 3u), via, 3u) == 0);
    args[0] = 0x100u; args[1] = DATA; args[2] = 8u; args[3] = 0u; args[4] = 0u; args[5] = 0u;
    check("an empty recvfrom would block", call(recomp_live_transport_lookup(0x003810a2u), args, 6u) == 0xffffffffu);
    check("and says WSAEWOULDBLOCK", call(recomp_live_transport_lookup(0x003814d3u), args, 0u) == 10035u);

    uint8_t *xnaddr = recomp_memory(XNADDR, 0x24u);
    memset(xnaddr, 0, 0x24u);
    memcpy(xnaddr, &ina_b, 4u);
    memset(recomp_memory(INADDR_OUT, 4u), 0, 4u);
    args[0] = XNADDR; args[1] = KID_OUT; args[2] = INADDR_OUT;
    check("XNetXnAddrToInAddr", call(recomp_live_transport_lookup(0x00380f9fu), args, 3u) == 0u &&
        memcmp(recomp_memory(INADDR_OUT, 4u), &ina_b, 4u) == 0);
    memset(recomp_memory(XNADDR_OUT, 0x24u), 0, 0x24u);
    args[0] = ina_b; args[1] = XNADDR_OUT; args[2] = KID_OUT;
    check("XNetInAddrToXnAddr", call(recomp_live_transport_lookup(0x00381109u), args, 3u) == 0u);
    {
        const uint8_t *filled = recomp_memory(XNADDR_OUT, 0x24u);
        check("the XNADDR names the address twice", memcmp(filled, &ina_b, 4u) == 0 &&
            memcmp(filled + 4u, &ina_b, 4u) == 0);
        check("and an Xbox NIC address", filled[10] == 0x00u && filled[11] == 0x50u && filled[12] == 0xf2u);
    }
    /* A new LPEER with another pair token or endpoint forgets the dead route. */
    uint8_t fresh_token[8];
    memcpy(fresh_token, PAIR_TOKEN, sizeof fresh_token);
    fresh_token[0] ^= 0xffu;
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = ina_b;
    intro.public_address = htonl(INADDR_LOOPBACK);
    intro.public_port = htons(9u);
    memcpy(intro.token, fresh_token, sizeof intro.token);
    recomp_live_transport_peer(&intro);
    check("a moved introduction drops the old route", !recomp_live_transport_reachable(ina_b));
    size = (int)punch_wire(packet, ina_b, port_1000, fresh_token);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms() + 100u);
    check("the new token connects from any source", recomp_live_transport_reachable(ina_b));

    /* A full table keeps the newest peer: nine introductions evict the stalest. */
    check("bind again for the eviction test", recomp_live_transport_bind_rollback(ina_b, &net));
    uint32_t newest_ina = 0u;
    uint8_t newest_token[8] = {0u};
    for (unsigned i = 0u; i < 9u; ++i) {
        memset(&intro, 0, sizeof intro);
        intro.virtual_ina = 0x0a000010u + i;
        intro.public_address = peer_address.sin_addr.s_addr;
        intro.public_port = peer_address.sin_port;
        for (unsigned b = 0u; b < sizeof intro.token; ++b) intro.token[b] = (uint8_t)(0x40u + i + b);
        recomp_live_transport_peer(&intro);
        newest_ina = intro.virtual_ina;
        memcpy(newest_token, intro.token, sizeof newest_token);
    }
    check("the bound opponent survives the eviction", recomp_live_transport_reachable(ina_b));
    size = (int)punch_wire(packet, newest_ina, port_1000, newest_token);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms() + 200u);
    check("the newest peer still connects", recomp_live_transport_reachable(newest_ina));
    /* Strangers' introductions cannot push out a peer heard just now. */
    for (unsigned i = 0u; i < 8u; ++i) {
        memset(&intro, 0, sizeof intro);
        intro.virtual_ina = 0x0a000050u + i;
        intro.public_address = peer_address.sin_addr.s_addr;
        intro.public_port = peer_address.sin_port;
        for (unsigned b = 0u; b < sizeof intro.token; ++b) intro.token[b] = (uint8_t)(0x90u + i + b);
        recomp_live_transport_peer(&intro);
    }
    check("introductions do not evict a peer heard lately", recomp_live_transport_reachable(newest_ina));
    recomp_live_transport_unbind_rollback();

    recomp_live_transport_forget(ina_b);
    check("a forgotten peer is not reachable", !recomp_live_transport_reachable(ina_b));
    recomp_live_transport_forget(0u);
    check("a logoff forgets the newest peer", !recomp_live_transport_reachable(newest_ina));

    /* A datagram that fails the token check is not the peer being alive: the
       stalest peer, not the one an impostor named, is what a full table gives
       up. */
    uint8_t older_token[8] = {51u, 52u, 53u, 54u, 55u, 56u, 57u, 58u};
    uint8_t newer_token[8] = {61u, 62u, 63u, 64u, 65u, 66u, 67u, 68u};
    memset(&intro, 0, sizeof intro);
    intro.virtual_ina = 0x0a000030u;
    intro.public_address = peer_address.sin_addr.s_addr;
    intro.public_port = peer_address.sin_port;
    memcpy(intro.token, older_token, sizeof intro.token);
    recomp_live_transport_peer(&intro);
    spin_ms(2u);
    intro.virtual_ina = 0x0a000031u;
    memcpy(intro.token, newer_token, sizeof intro.token);
    recomp_live_transport_peer(&intro);
    spin_ms(2u);
    size = (int)punch_wire(packet, 0x0a000030u, port_1000, SOCKET_NONCE);
    send_socket(intruder_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms());
    for (unsigned i = 0u; i < 7u; ++i) {
        memset(&intro, 0, sizeof intro);
        intro.virtual_ina = 0x0a000040u + i;
        intro.public_address = peer_address.sin_addr.s_addr;
        intro.public_port = peer_address.sin_port;
        for (unsigned b = 0u; b < sizeof intro.token; ++b) intro.token[b] = (uint8_t)(0x70u + i + b);
        recomp_live_transport_peer(&intro);
    }
    size = (int)punch_wire(packet, 0x0a000031u, port_1000, newer_token);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    size = (int)punch_wire(packet, 0x0a000030u, port_1000, older_token);
    send_socket(peer_socket, &client_endpoint, packet, (size_t)size);
    recomp_live_transport_pump(netplay_now_ms());
    check("a rejected datagram is not activity", recomp_live_transport_reachable(0x0a000031u) &&
        !recomp_live_transport_reachable(0x0a000030u));

    recomp_live_transport_close();
    check("a closed transport receives nothing", guest_read(data, sizeof data, &from_ina, &from_port) < 0);

    /* --- the matchmaker's name is looked up off the frame thread --- */
    while (read_socket(server_socket, packet, sizeof packet, &client_endpoint, 20u) >= 0) {
    }
    const uint16_t server_port = ntohs(server_address.sin_port);
    const size_t named_size = match_request(request, 12u, 9u);
    recomp_live_transport_set_resolver(slow_resolver);
    check("a named server opens", recomp_live_transport_open("matchmaker.test", server_port));
    const uint64_t asked_at = netplay_now_ms();
    recomp_live_transport_send_server(request, named_size);
    check("a request does not wait for the lookup", netplay_now_ms() - asked_at < 100u);
    recomp_live_transport_send_server(request, named_size);
    check("and none goes out before the name is found", lookups_seen() == 0 &&
        read_socket(server_socket, packet, sizeof packet, &client_endpoint, 50u) < 0);
    spin_ms(600u);
    recomp_live_transport_send_server(request, named_size); /* the client's resend */
    size = read_socket(server_socket, packet, sizeof packet, &client_endpoint, 1000u);
    check("the resend after it goes out", lookups_seen() == 1 && size == (int)named_size &&
        memcmp(packet, "D2MM", 4u) == 0 && packet[MATCH_ASKED + 3u] == 9u);

    /* A name that does not resolve is not asked again at once. */
    fail_lookups(true);
    check("a server that will not resolve opens", recomp_live_transport_open("matchmaker.test", server_port));
    recomp_live_transport_send_server(request, named_size);
    spin_ms(600u);
    recomp_live_transport_send_server(request, named_size);
    recomp_live_transport_send_server(request, named_size);
    check("a failed lookup sends nothing and is not repeated at once", lookups_seen() == 2 &&
        read_socket(server_socket, packet, sizeof packet, &client_endpoint, 50u) < 0);

    /* Closing while a lookup runs leaves it to its thread. */
    fail_lookups(false);
    check("the server opens again", recomp_live_transport_open("matchmaker.test", server_port));
    recomp_live_transport_send_server(request, named_size);
    recomp_live_transport_close();
    check("a lookup outlives the close that dropped it", lookups_after_wait(3) == 3 &&
        read_socket(server_socket, packet, sizeof packet, &client_endpoint, 50u) < 0);
    recomp_live_transport_set_resolver(NULL);

    if (server_socket != INVALID_SOCKET) closesocket(server_socket);
    if (peer_socket != INVALID_SOCKET) closesocket(peer_socket);
    if (intruder_socket != INVALID_SOCKET) closesocket(intruder_socket);
    if (failures != 0) return 1;
    puts("PASS live transport carries the game's datagrams, the session's rewind log and a bound rollback");
    return 0;
}
