/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Live client against the byte vectors in tools/live_vectors.json: the
   Python server generated each request and answer, so this test proves the C
   client sends the same bytes and reads the same answers. The transport
   (live_transport.c) stands in here, so the test needs no socket. */
#include "live_client.h"
#include "live_identity.h"
#include "live_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the transport stand-in --- */

enum { CAPTURED = 8u, MAX_ANSWERS = 8u, MAX_REQUESTS = 4u };

static uint8_t captured[CAPTURED][RECOMP_LIVE_PACKET_BYTES];
static size_t captured_size[CAPTURED];
static unsigned captured_count;
static RecompLiveServerHandler server_handler;
static uint8_t test_nonce[8];
static uint8_t test_cookie[8];
static uint8_t asked[RECOMP_LIVE_PACKET_BYTES]; /* the transport's last request, for its one retry */
static size_t asked_size;
static uint32_t local_ina;
static RecompLivePeerIntro last_intro;
static unsigned intro_count;

/* What live_transport.c does with a request: write this socket's nonce and the
   server's cookie, keep it, and send it. The client leaves those 16 bytes zero. */
void recomp_live_transport_send_server(const uint8_t *packet, size_t size)
{
    if (size < 22u || size > RECOMP_LIVE_PACKET_BYTES) return;
    memcpy(asked, packet, size);
    memcpy(asked + 6u, test_nonce, 8u);
    memcpy(asked + 14u, test_cookie, 8u);
    asked_size = size;
    if (captured_count >= CAPTURED) return;
    memcpy(captured[captured_count], asked, size);
    captured_size[captured_count++] = size;
}

void recomp_live_transport_set_server_handler(RecompLiveServerHandler handler)
{
    server_handler = handler;
}

/* What live_transport.c does with an answer: only the server knows this
   socket's nonce, a COOKIE answer is its to handle (the last request goes out
   again once with the new cookie), and the rest reach the Live client. */
static void feed(const uint8_t *packet, size_t size)
{
    if (size < 22u || memcmp(packet, "D2MM", 4u) != 0 || packet[4] != 2u) return;
    if (memcmp(packet + 6u, test_nonce, 8u) != 0) return;
    if (packet[5] == 10u) { /* COOKIE */
        if (size != 22u) return;
        memcpy(test_cookie, packet + 14u, 8u);
        if (asked_size == 0u) return;
        if (captured_count < CAPTURED) {
            memcpy(captured[captured_count], asked, asked_size);
            memcpy(captured[captured_count] + 14u, test_cookie, 8u);
            captured_size[captured_count++] = asked_size;
        }
        asked_size = 0u; /* once */
        return;
    }
    if (server_handler != NULL) server_handler(packet, size);
}

void recomp_live_transport_peer(const RecompLivePeerIntro *intro)
{
    if (intro == NULL) return;
    last_intro = *intro;
    ++intro_count;
}

void recomp_live_transport_set_local(uint32_t virtual_ina)
{
    local_ina = virtual_ina;
}

static unsigned forget_calls;
static uint32_t forget_ina;

/* The players the transport has heard from, which end a CONNECT's repeats. */
static uint32_t heard_inas[4];

bool recomp_live_transport_heard(uint32_t virtual_ina)
{
    for (unsigned i = 0u; i < 4u; ++i)
        if (heard_inas[i] == virtual_ina && virtual_ina != 0u) return true;
    return false;
}

void recomp_live_transport_forget(uint32_t virtual_ina)
{
    ++forget_calls;
    forget_ina = virtual_ina;
}

/* --- the vectors --- */

typedef struct Step {
    char call[24];
    unsigned kind;
    uint8_t requests[MAX_REQUESTS][RECOMP_LIVE_PACKET_BYTES];
    size_t request_sizes[MAX_REQUESTS];
    unsigned request_count;
    uint8_t answers[MAX_ANSWERS][RECOMP_LIVE_PACKET_BYTES];
    size_t answer_sizes[MAX_ANSWERS];
    unsigned answer_count;
    unsigned slots[RECOMP_LIVE_SESSION_SLOTS];
    uint8_t attrs[64];
    size_t attrs_size;
    uint8_t kid[8];
    bool has_kid;
    unsigned long long xuid;
    bool has_xuid;
    unsigned long long xuids[8];
    unsigned xuid_count;
    unsigned long long ina;
    bool has_ina;
    unsigned long long pivot;
    RecompLiveStat stats[RECOMP_LIVE_MAX_STAT_ATTRS];
    unsigned stat_count;
    unsigned board, first, count, state, op, answer, type, board_total;
    unsigned long long destination, port;
} Step;

static const char *document;
static int failures;

static void check(const char *name, bool ok)
{
    if (ok) return;
    fprintf(stderr, "FAIL %s\n", name);
    ++failures;
}

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *findKey(const char *object, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *at = strstr(object, pattern);
    if (at == NULL) return NULL;
    at = strchr(at + strlen(pattern), ':');
    if (at == NULL) return NULL;
    ++at;
    while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t') ++at;
    return at;
}

static size_t readHex(const char *value, uint8_t *out, size_t capacity)
{
    if (value == NULL || *value != '"') return (size_t)-1;
    ++value;
    size_t count = 0u;
    while (*value != '"' && *value != '\0') {
        const int high = hexDigit(value[0]), low = hexDigit(value[1]);
        if (high < 0 || low < 0 || count == capacity) return (size_t)-1;
        out[count++] = (uint8_t)(high << 4 | low);
        value += 2;
    }
    return count;
}

static unsigned long long readNumber(const char *object, const char *key, unsigned long long fallback)
{
    const char *value = findKey(object, key);
    return value != NULL && *value != 'n' ? strtoull(value, NULL, 10) : fallback;
}

static size_t readString(const char *object, const char *key, char *out, size_t capacity)
{
    const char *value = findKey(object, key);
    if (value == NULL || *value != '"') return 0u;
    ++value;
    size_t count = 0u;
    while (*value != '"' && *value != '\0' && count + 1u < capacity) out[count++] = *value++;
    out[count] = '\0';
    return count;
}

static unsigned readHexList(const char *object, const char *key, uint8_t list[][RECOMP_LIVE_PACKET_BYTES],
    size_t *sizes, unsigned capacity)
{
    const char *at = findKey(object, key);
    unsigned count = 0u;
    if (at == NULL || *at != '[') return 0u;
    ++at;
    for (;;) {
        while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t' || *at == ',') ++at;
        if (*at != '"' || count == capacity) return count;
        const size_t size = readHex(at, list[count], RECOMP_LIVE_PACKET_BYTES);
        if (size == (size_t)-1) return count;
        sizes[count++] = size;
        at = strchr(at + 1u, '"'); /* the value's closing quote */
        if (at == NULL) return count;
        ++at;
    }
}

/* A step's [[id, type, value], ...] list, the same stats the Python server
   wrote and answered with. */
static unsigned readStatList(const char *object, const char *key, RecompLiveStat *out, unsigned capacity)
{
    const char *at = findKey(object, key);
    unsigned count = 0u;
    if (at == NULL || *at != '[') return 0u;
    ++at;
    for (;;) {
        while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t' || *at == ',') ++at;
        if (*at != '[' || count == capacity) return count;
        ++at;
        unsigned long long values[3] = {0u, 0u, 0u};
        for (unsigned i = 0u; i < 3u; ++i) {
            while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t') ++at;
            char *end = NULL;
            values[i] = strtoull(at, &end, 10);
            if (end == at) return count;
            at = end;
            while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t') ++at;
            if (*at == ',') ++at;
        }
        while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t') ++at;
        if (*at != ']') return count;
        ++at;
        out[count].id = (uint16_t)values[0];
        out[count].type = (uint8_t)values[1];
        out[count].value = (int64_t)values[2];
        ++count;
    }
}

static unsigned readNumberList(const char *object, const char *key, unsigned long long *out, unsigned capacity)
{
    const char *at = findKey(object, key);
    unsigned count = 0u;
    if (at == NULL || *at != '[') return 0u;
    ++at;
    for (;;) {
        while (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t' || *at == ',') ++at;
        if (*at == ']' || *at == '\0' || count == capacity) return count;
        char *end = NULL;
        out[count++] = strtoull(at, &end, 10);
        at = end;
    }
}

/* The next object in the steps array, or NULL at its end. */
static const char *nextStep(const char **cursor, char *copy, size_t capacity)
{
    const char *at = *cursor;
    while (*at != '\0' && *at != '{' && *at != ']') ++at;
    if (*at != '{') {
        *cursor = at;
        return NULL;
    }
    unsigned depth = 0u;
    size_t length = 0u;
    for (; *at != '\0'; ++at) {
        if (length + 1u >= capacity) return NULL;
        copy[length++] = *at;
        if (*at == '{') ++depth;
        else if (*at == '}' && --depth == 0u) {
            ++at;
            break;
        }
    }
    copy[length] = '\0';
    *cursor = at;
    return copy;
}

static void parseStep(const char *object, Step *step)
{
    memset(step, 0, sizeof *step);
    readString(object, "call", step->call, sizeof step->call);
    step->kind = (unsigned)readNumber(object, "kind", 0u);
    step->request_count = readHexList(object, "requests", step->requests, step->request_sizes, MAX_REQUESTS);
    step->answer_count = readHexList(object, "answers", step->answers, step->answer_sizes, MAX_ANSWERS);
    unsigned long long numbers[RECOMP_LIVE_SESSION_SLOTS];
    const unsigned slot_count = readNumberList(object, "slots", numbers, RECOMP_LIVE_SESSION_SLOTS);
    for (unsigned i = 0u; i < slot_count && i < RECOMP_LIVE_SESSION_SLOTS; ++i) step->slots[i] = (unsigned)numbers[i];
    const size_t attrs = readHex(findKey(object, "attrs"), step->attrs, sizeof step->attrs);
    step->attrs_size = attrs == (size_t)-1 ? 0u : attrs;
    step->has_kid = readHex(findKey(object, "kid"), step->kid, sizeof step->kid) == 8u;
    step->has_xuid = findKey(object, "xuid") != NULL;
    step->xuid = readNumber(object, "xuid", 0u);
    step->xuid_count = readNumberList(object, "xuids", step->xuids, 8u);
    step->has_ina = findKey(object, "ina") != NULL;
    step->ina = readNumber(object, "ina", 0u);
    step->pivot = readNumber(object, "pivot", 0u);
    step->stat_count = readStatList(object, "stats", step->stats, RECOMP_LIVE_MAX_STAT_ATTRS);
    step->board = (unsigned)readNumber(object, "board", 0u);
    step->board_total = (unsigned)readNumber(object, "board_total", 0u);
    step->first = (unsigned)readNumber(object, "first", 0u);
    step->count = (unsigned)readNumber(object, "count", 0u);
    step->state = (unsigned)readNumber(object, "state", 0u);
    step->op = (unsigned)readNumber(object, "op", 0u);
    step->answer = (unsigned)readNumber(object, "answer", 0u);
    step->type = (unsigned)readNumber(object, "type", 0u);
    step->destination = readNumber(object, "destination", 0u);
    step->port = readNumber(object, "port", 0u);
}

/* --- the transcript --- */

static const RecompLiveBackend *backend;
static char transcript_digest[65];
static char transcript_tag[16];

/* Feeds each answer, then checks every packet the client sent against its vector. */
static bool sameRequest(const Step *step, unsigned index)
{
    if (captured_size[index] != step->request_sizes[index]) {
        fprintf(stderr, "  request %u is %zu bytes, the vector %zu\n", index, captured_size[index],
            step->request_sizes[index]);
        return false;
    }
    for (size_t i = 0u; i < captured_size[index]; ++i) {
        if (captured[index][i] == step->requests[index][i]) continue;
        fprintf(stderr, "  request %u differs at byte %zu: %02x, the vector %02x\n", index, i, captured[index][i],
            step->requests[index][i]);
        return false;
    }
    return true;
}

static bool replay(const Step *step)
{
    bool ok = true;
    for (unsigned i = 0u; i < step->answer_count; ++i) feed(step->answers[i], step->answer_sizes[i]);
    if (captured_count < step->request_count) return false;
    /* The last step's push makes the client ask for its friend list again. */
    if (captured_count > step->request_count && strcmp(step->call, "push") != 0) ok = false;
    for (unsigned i = 0u; i < step->request_count; ++i)
        if (!sameRequest(step, i)) ok = false;
    return ok;
}

static bool done(RecompLiveRequest request)
{
    uint32_t hresult = 0u;
    return backend->poll(backend->self, request, &hresult) == RECOMP_LIVE_DONE && hresult == 0u;
}

static uint32_t read32be(const uint8_t *in);

/* The CONNECT request's header, nonce and cookie, which a LIVE_RELAY repeats. */
static uint8_t connect_asked[22];

/* A LIVE_RELAY and the RELAY it becomes, at the offsets live_transport.c uses:
   the cookie ends at 22 (MATCH_ASKED), a relay's datagram starts at 14 (MATCH_BODY),
   and the guest header is "D2GD", version, then the sender's ina and port, big-endian. */
static void checkRelayVector(const Step *step)
{
    if (!strcmp(step->call, "live_relay")) {
        const uint8_t *packet = step->requests[0];
        check("live_relay is one request", step->request_count == 1u && step->answer_count == 0u &&
            step->request_sizes[0] > 22u + 4u + 11u);
        check("live_relay carries the client's header and cookie", packet[5] == 34u &&
            memcmp(packet, connect_asked, 5u) == 0 && memcmp(packet + 6u, connect_asked + 6u, 16u) == 0);
        check("live_relay names its destination", read32be(packet + 22u) == (uint32_t)step->destination);
        check("live_relay carries a guest datagram", memcmp(packet + 26u, "D2GD", 4u) == 0 &&
            (packet[35] << 8 | packet[36]) == (int)step->port);
    } else {
        const uint8_t *packet = step->answers[0];
        check("the forward is one answer", step->answer_count == 1u && step->request_count == 0u &&
            step->answer_sizes[0] > 14u + 11u);
        check("the forward is a RELAY", memcmp(packet, "D2MM", 4u) == 0 && packet[4] == 2u && packet[5] == 9u);
        check("the forward stamps the sender", memcmp(packet + 14u, "D2GD", 4u) == 0 &&
            read32be(packet + 19u) == (uint32_t)step->ina && (packet[23] << 8 | packet[24]) == (int)step->port);
    }
}

static void runStep(Step *step, uint64_t now)
{
    const char *call = step->call;
    RecompLiveRequest request = 0u;
    captured_count = 0u;
    if (!strcmp(call, "live_relay") || !strcmp(call, "live_relay_forward")) {
        checkRelayVector(step);
        return;
    }
    if (!strcmp(call, "connect") && step->request_count != 0u && step->request_sizes[0] >= sizeof connect_asked)
        memcpy(connect_asked, step->requests[0], sizeof connect_asked);
    if (!strcmp(call, "logon")) {
        request = backend->logon(backend->self);
    } else if (!strcmp(call, "session_create")) {
        request = backend->session_create(backend->self, step->slots, step->attrs, (uint32_t)step->attrs_size);
    } else if (!strcmp(call, "session_update")) {
        request = backend->session_update(backend->self, step->kid, step->slots, step->attrs,
            (uint32_t)step->attrs_size);
    } else if (!strcmp(call, "session_delete")) {
        request = backend->session_delete(backend->self, step->kid);
    } else if (!strcmp(call, "search")) {
        request = backend->search(backend->self, 0u, NULL, 0u);
    } else if (!strcmp(call, "find")) {
        request = backend->find(backend->self, step->kid);
    } else if (!strcmp(call, "connect")) {
        backend->connect(backend->self, (uint32_t)step->ina);
    } else if (!strcmp(call, "friends")) {
        request = backend->friends_enumerate(backend->self);
    } else if (!strcmp(call, "friend_request")) {
        request = backend->friend_request(backend->self, step->xuid);
    } else if (!strcmp(call, "friend_answer")) {
        request = backend->friend_answer(backend->self, step->xuid, (RecompLiveAnswer)step->answer);
    } else if (!strcmp(call, "friend_remove")) {
        request = backend->friend_remove(backend->self, step->xuid);
    } else if (!strcmp(call, "game_invite")) {
        request = backend->game_invite(backend->self, step->kid, step->xuids, step->xuid_count);
    } else if (!strcmp(call, "revoke_invite")) {
        request = backend->revoke_invite(backend->self, step->kid, step->xuids, step->xuid_count);
    } else if (!strcmp(call, "answer_invite")) {
        request = backend->answer_invite(backend->self, step->xuid, (RecompLiveAnswer)step->answer);
    } else if (!strcmp(call, "presence")) {
        backend->presence(backend->self, step->state, step->kid);
        recomp_live_client_tick(now + 1u); /* the heartbeat goes out at once */
    } else if (!strcmp(call, "mutes")) {
        request = backend->mutelist(backend->self);
    } else if (!strcmp(call, "feedback")) {
        request = backend->feedback(backend->self, step->xuid, step->type);
    } else if (!strcmp(call, "stat_write") || !strcmp(call, "stat_write_group7")) {
        request = backend->stat_write(backend->self, step->board, step->xuid, step->stats, step->stat_count);
    } else if (!strcmp(call, "stat_read") || !strcmp(call, "stat_read_group7")) {
        uint64_t xuids[8];
        for (unsigned i = 0u; i < step->xuid_count; ++i) xuids[i] = step->xuids[i];
        request = backend->stat_read(backend->self, step->board, xuids, step->xuid_count);
    } else if (!strcmp(call, "leaders") || !strcmp(call, "leaders_group7")) {
        request = backend->leader_enumerate(backend->self, step->board, step->pivot, step->first, step->count);
    } else if (!strcmp(call, "push")) {
        /* only the answers matter: a push is the server's */
    } else {
        check("a known step", false);
        return;
    }
    check(call, replay(step));
    if (strcmp(call, "connect") == 0) {
        check("connect introduces the player", intro_count == 1u && last_intro.virtual_ina == (uint32_t)step->ina);
        return;
    }
    if (strcmp(call, "presence") == 0) return; /* its heartbeats are not requests the game polls */
    if (strcmp(call, "push") == 0) return;
    check(call, done(request));
    if (!strcmp(call, "logon")) {
        RecompLiveUser user;
        check("logon keeps the address", backend->local_user(backend->self, &user) && local_ina == 0x0a010003u &&
            user.address.virtual_ina == 0x0a010003u && user.address.port == 1000u && user.address.mac[1] == 0x50u);
    } else if (!strcmp(call, "session_create") || !strcmp(call, "session_update")) {
        uint8_t kid[8] = {0u};
        if (step->has_kid) memcpy(kid, step->kid, 8u);
        else if (step->answer_count != 0u && step->answer_sizes[0] >= 30u)
            memcpy(kid, step->answers[0] + 22u, 8u); /* a SESSION_OK body: request, hresult, kid */
        RecompLiveSessionId id;
        check("session id", backend->session_id(backend->self, request, &id) && memcmp(id.kid, kid, 8u) == 0);
    } else if (!strcmp(call, "search") || !strcmp(call, "find")) {
        RecompLiveSession sessions[RECOMP_LIVE_MAX_RESULTS];
        check("search rows", backend->results(backend->self, request, sessions, RECOMP_LIVE_MAX_RESULTS) ==
            step->answer_count);
    } else if (!strcmp(call, "friends")) {
        RecompLiveFriend friends[RECOMP_LIVE_MAX_FRIENDS];
        check("friend rows", backend->friends(backend->self, friends, RECOMP_LIVE_MAX_FRIENDS) == step->answer_count);
    } else if (!strcmp(call, "mutes")) {
        uint64_t mutes[RECOMP_LIVE_MAX_MUTES];
        check("mute rows", backend->mutes(backend->self, mutes, RECOMP_LIVE_MAX_MUTES) == 2u);
    } else if (!strcmp(call, "stat_read") || !strcmp(call, "stat_read_group7") ||
        !strcmp(call, "leaders") || !strcmp(call, "leaders_group7")) {
        static RecompLiveLeaderRow rows[RECOMP_LIVE_MAX_LEADER_ROWS];
        const uint32_t count = backend->stat_results(backend->self, request, rows, RECOMP_LIVE_MAX_LEADER_ROWS);
        check("stat rows", count >= 1u && count <= step->answer_count);
        check("board total", backend->leader_total(backend->self, request) == step->board_total);
        if (!strcmp(call, "stat_read_group7") || !strcmp(call, "leaders_group7")) {
            check("group 7 row is whole", count == 1u && rows[0].stat_count == 61u &&
                rows[0].stats[0].id == 0u && rows[0].stats[59].id == 59u &&
                rows[0].stats[60].id == 0xfffeu && rows[0].stats[60].value == 1234567890123ll);
        }
    } else if (!strcmp(call, "answer_invite")) {
        RecompLiveFriend from;
        check("the accepted invite", backend->accepted_invite(backend->self, &from) && from.xuid == step->xuid &&
            memcmp(from.session_kid, step->kid, 8u) == 0);
    }
    backend->release(backend->self, request);
}

/* --- pushes, heartbeats and hostile answers --- */

static void put32be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t read32be(const uint8_t *in)
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

/* An answer's body, as the matchmaker's packet carries it after the nonce. */
static void feed_body(uint8_t kind, const uint8_t *body, size_t size)
{
    uint8_t packet[RECOMP_LIVE_PACKET_BYTES];
    if (14u + size > sizeof packet) return;
    memcpy(packet, "D2MM", 4u);
    packet[4] = 2u;
    packet[5] = kind;
    memcpy(packet + 6u, test_nonce, 8u);
    if (size != 0u) memcpy(packet + 14u, body, size);
    feed(packet, 14u + size);
}

/* A push: type, the player it is about, and an invite's session id. */
static void push_body(uint8_t body[17], uint8_t type, uint64_t xuid)
{
    memset(body, 0, 17u);
    body[0] = type;
    for (unsigned i = 0u; i < 8u; ++i) body[1u + i] = (uint8_t)(xuid >> (56u - 8u * i));
}

/* A FRIEND answer holding no row: request id, index, total. */
static void friend_empty_body(uint8_t body[46], uint32_t id)
{
    memset(body, 0, 46u);
    put32be(body, id);
    body[5] = 0u; /* total */
}

static void checkPushSlots(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the push test", backend != NULL);
    recomp_live_client_tick(1000u);
    unsigned answered = 0u;
    for (unsigned push = 0u; push < 20u; ++push) {
        uint8_t body[17];
        push_body(body, 1u, 0x0009000000000010ull + push);
        captured_count = 0u;
        feed_body(26u, body, sizeof body); /* PUSH */
        if (captured_count != 1u) continue; /* the re-enumeration is on its way */
        uint8_t answer[46];
        friend_empty_body(answer, read32be(captured[0] + 22u));
        feed_body(23u, answer, sizeof answer); /* FRIEND, no rows: the slot is free again */
        ++answered;
        recomp_live_client_tick(1100u + push * 100u);
    }
    check("every push asked for the friend list", answered == 20u);
    captured_count = 0u;
    const RecompLiveRequest request = backend->logon(backend->self);
    check("a logon still finds a slot after 20 pushes", request != 0u && captured_count == 1u);
    backend->release(backend->self, request);
    recomp_live_client_detach();
}

static void checkHeartbeat(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the heartbeat test", backend != NULL);
    recomp_live_client_tick(1000u);
    captured_count = 0u;
    const RecompLiveRequest request = backend->logon(backend->self);
    check("the heartbeat test logs on", request != 0u && captured_count == 1u);
    uint8_t body[28];
    memset(body, 0, sizeof body);
    put32be(body, read32be(captured[0] + 22u));
    put32be(body + 8u, 0x0a010009u);
    feed_body(13u, body, sizeof body); /* LOGGED */
    check("the logon answer names the player", done(request) && local_ina == 0x0a010009u);
    backend->release(backend->self, request);
    /* A heartbeat's LOGGED takes no request slot; a moved address is adopted. */
    put32be(body, 0x7fffffffu);
    put32be(body + 8u, 0x0a01000au);
    feed_body(13u, body, sizeof body);
    check("a heartbeat answer is adopted", local_ina == 0x0a01000au);
    const unsigned before = forget_calls;
    backend->logoff(backend->self);
    check("a logoff forgets every peer", forget_calls == before + 1u && forget_ina == 0u);
    recomp_live_client_detach();
}

static void checkStatTypes(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the stat test", backend != NULL);
    recomp_live_client_tick(1000u);
    captured_count = 0u;
    const RecompLiveRequest request = backend->leader_enumerate(backend->self, 1u, 0u, 0u, 5u);
    check("the stat test asks for a board", request != 0u && captured_count == 1u);
    uint8_t body[43u + 3u * 11u];
    memset(body, 0, sizeof body);
    put32be(body, read32be(captured[0] + 22u));
    body[9] = 1u;                     /* total rows */
    put32be(body + 10u, 1u);          /* board total */
    body[42] = 3u;                    /* stats */
    const uint8_t stats[3][11] = {
        {0x00u, 0x01u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 7u},
        {0x00u, 0x02u, 4u, 0u, 0u, 0u, 0u, 0u, 0u, 0x12u, 0x34u}, /* a string: not a number */
        {0x00u, 0x03u, 2u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 9u},
    };
    memcpy(body + 43u, stats, sizeof stats);
    feed_body(32u, body, sizeof body); /* STATROW */
    static RecompLiveLeaderRow rows[RECOMP_LIVE_MAX_LEADER_ROWS];
    check("the stat test has its row", done(request) &&
        backend->stat_results(backend->self, request, rows, RECOMP_LIVE_MAX_LEADER_ROWS) == 1u);
    check("the numeric stats cross", rows[0].stat_count == 3u && rows[0].stats[0].type == 1u &&
        rows[0].stats[0].value == 7 && rows[0].stats[2].type == 2u && rows[0].stats[2].value == 9);
    check("a string stat is zeroed", rows[0].stats[1].type == 0u && rows[0].stats[1].value == 0);
    backend->release(backend->self, request);
    recomp_live_client_detach();
}

static void checkGroupChildren(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the group test", backend != NULL);
    recomp_live_client_tick(1000u);
    captured_count = 0u;
    const uint8_t kid[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const uint64_t xuids[8] = {0x0009000000000101ull, 0x0009000000000102ull, 0x0009000000000103ull,
        0x0009000000000104ull, 0x0009000000000105ull, 0x0009000000000106ull, 0x0009000000000107ull,
        0x0009000000000108ull};
    const RecompLiveRequest head = backend->game_invite(backend->self, kid, xuids, 8u);
    check("a group sends one invite per friend", head != 0u && captured_count == 8u);
    for (unsigned i = 0u; i < captured_count; ++i) {
        uint8_t body[8];
        put32be(body, read32be(captured[i] + 22u));
        put32be(body + 4u, 0u);
        feed_body(25u, body, sizeof body); /* DONE */
    }
    check("the group settles", done(head));
    backend->release(backend->self, head);
    /* Eight slots were held by the group; the table is whole again. */
    RecompLiveRequest extra[16];
    unsigned available = 0u;
    for (unsigned i = 0u; i < 16u; ++i) {
        extra[i] = backend->mutelist(backend->self);
        if (extra[i] != 0u) ++available;
    }
    check("releasing a group frees its children", available == 16u);
    for (unsigned i = 0u; i < 16u; ++i)
        if (extra[i] != 0u) backend->release(backend->self, extra[i]);
    recomp_live_client_detach();
}

/* --- retries, timeouts, cookies and the identity file --- */

static void checkRetriesAndTimeout(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the timing test", backend != NULL);
    recomp_live_client_tick(1000u); /* the runner's frame clock has run by now */
    captured_count = 0u;
    const RecompLiveRequest request = backend->logon(backend->self);
    captured_count = 0u;
    recomp_live_client_tick(1500u); /* the first 500 ms: sent again */
    check("a request goes out every 500 ms", captured_count == 1u);
    uint32_t hresult = 0u;
    check("a request waits for its answer",
        backend->poll(backend->self, request, &hresult) == RECOMP_LIVE_PENDING);
    recomp_live_client_tick(6000u); /* 5 s from the first send */
    check("a request fails after 5 s",
        backend->poll(backend->self, request, &hresult) == RECOMP_LIVE_FAILED && hresult == 0x800705B4u);
    backend->release(backend->self, request);
    recomp_live_client_detach();
}

/* How many of the packets the client sent in this step were one kind. */
static unsigned captured_kind(uint8_t kind)
{
    unsigned count = 0u;
    for (unsigned i = 0u; i < captured_count; ++i)
        if (captured[i][5] == kind) ++count;
    return count;
}

/* A CONNECT repeats with its first request id until the pair's LPEER arrives,
   and a host that learns a joiner from an unasked LPEER answers with one. */
static void checkConnectRetry(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the connect test", backend != NULL);
    recomp_live_client_tick(1000u);
    captured_count = 0u;
    const RecompLiveRequest logon = backend->logon(backend->self);
    uint8_t body[28];
    memset(body, 0, sizeof body);
    put32be(body, read32be(captured[0] + 22u));
    put32be(body + 8u, 0x0a010001u);
    feed_body(13u, body, sizeof body); /* LOGGED */
    check("the connect test logs on", done(logon));
    backend->release(backend->self, logon);

    captured_count = 0u;
    backend->connect(backend->self, 0x0a010002u);
    check("the first datagram asks for a pairing", captured_count == 1u && captured[0][5] == 20u &&
        read32be(captured[0] + 22u) != 0u && read32be(captured[0] + 26u) == 0x0a010002u);
    const uint32_t id = read32be(captured[0] + 22u);
    const size_t size = captured_size[0];
    captured_count = 0u;
    recomp_live_client_tick(1500u);
    check("half a second does not repeat the connect", captured_kind(20u) == 0u);
    recomp_live_client_tick(2000u);
    check("a second repeats the connect with its id", captured_count == 1u && captured_kind(20u) == 1u &&
        read32be(captured[0] + 22u) == id && captured_size[0] == size &&
        memcmp(captured[0] + 6u, test_nonce, 8u) == 0);

    /* The pair's introduction does not end the asks: the other side's LPEER
       may be the one lost, and each repeat has the server send it again. */
    const unsigned intros = intro_count;
    memset(body, 0, sizeof body);
    put32be(body, 0x0a010002u);
    feed_body(21u, body, 25u); /* LPEER */
    check("the pair's LPEER reaches the transport",
        intro_count == intros + 1u && last_intro.virtual_ina == 0x0a010002u);
    captured_count = 0u;
    recomp_live_client_tick(4000u);
    check("the LPEER alone does not end the connect", captured_kind(20u) == 1u &&
        read32be(captured[0] + 22u) == id);
    /* Hearing the other player does. */
    heard_inas[0] = 0x0a010002u;
    captured_count = 0u;
    recomp_live_client_tick(5000u);
    check("hearing the player ends the connect", captured_kind(20u) == 0u);
    /* The other side's own CONNECT sends this pairing's LPEER again: this side
       already asked, so answering would start a ping-pong of CONNECTs. */
    captured_count = 0u;
    feed_body(21u, body, 25u);
    check("a repeated LPEER does not connect back", captured_kind(20u) == 0u);

    /* The joiner's LPEER reaches this side without a CONNECT of its own. */
    memset(body, 0, sizeof body);
    put32be(body, 0x0a010004u);
    feed_body(21u, body, 25u); /* LPEER */
    check("an unasked LPEER answers with a connect", captured_count == 1u && captured_kind(20u) == 1u &&
        read32be(captured[0] + 26u) == 0x0a010004u && read32be(captured[0] + 22u) != id);
    captured_count = 0u;
    recomp_live_client_tick(6100u);
    check("and repeats it while it is unanswered", captured_kind(20u) == 1u);
    feed_body(21u, body, 25u); /* the answer to this side's CONNECT: the same introduction */
    heard_inas[1] = 0x0a010004u;
    captured_count = 0u;
    recomp_live_client_tick(8000u);
    check("an answered host does not ask again", captured_kind(20u) == 0u);

    /* The transport asks again when a player it sends to falls silent: a fresh
       request id, since the old answer may name a dead endpoint. */
    captured_count = 0u;
    backend->connect(backend->self, 0x0a010002u);
    check("a silent player is asked for again", captured_count == 1u && captured_kind(20u) == 1u &&
        read32be(captured[0] + 26u) == 0x0a010002u && read32be(captured[0] + 22u) != id);

    /* A rematch: the old pairing lapsed and the joiner's CONNECT made a new
       one. The host answers that once, so the new pairing is two-sided too. */
    memset(body, 0, sizeof body);
    put32be(body, 0x0a010004u);
    memset(body + 16u, 0x5a, 8u); /* a new pair token */
    captured_count = 0u;
    feed_body(21u, body, 25u);
    check("a new pairing with a known player connects back", captured_kind(20u) == 1u &&
        read32be(captured[0] + 26u) == 0x0a010004u);
    captured_count = 0u;
    feed_body(21u, body, 25u);
    check("and the same pairing again does not", captured_kind(20u) == 0u);
    memset(heard_inas, 0, sizeof heard_inas);
    captured_count = 0u;
    recomp_live_client_detach();
    static const uint8_t no_kid[8] = {0u};
    check("detaching logs off with a zero session id", captured_count == 1u && captured[0][5] == 16u &&
        memcmp(captured[0] + 26u, no_kid, 8u) == 0);
}

/* A heartbeat's error answer signs the account out, as a refused logon leaves
   it signed out. */
static void checkHeartbeatRefused(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the refused heartbeat test", backend != NULL);
    recomp_live_client_tick(1000u);
    captured_count = 0u;
    const RecompLiveRequest request = backend->logon(backend->self);
    uint8_t body[28];
    memset(body, 0, sizeof body);
    put32be(body, read32be(captured[0] + 22u));
    put32be(body + 8u, 0x0a010009u);
    feed_body(13u, body, sizeof body); /* LOGGED */
    check("the refused heartbeat test logs on", done(request) && local_ina == 0x0a010009u);
    backend->release(backend->self, request);
    const unsigned before = forget_calls;
    put32be(body, 0x7fffffffu); /* the heartbeat's own id: no slot to find */
    put32be(body + 4u, 0x80070005u); /* E_ACCESSDENIED */
    feed_body(13u, body, sizeof body);
    check("a refused heartbeat signs the account out",
        forget_calls == before + 1u && forget_ina == 0u && local_ina == 0u);
    recomp_live_client_detach();
}

static void checkCookie(const RecompLiveIdentity *identity)
{
    static RecompLiveIdentity copy;
    copy = *identity;
    memset(test_cookie, 0, sizeof test_cookie);
    asked_size = 0u;
    backend = recomp_live_client_attach(&copy, "PLAYER", transcript_digest);
    check("attach for the cookie test", backend != NULL);
    recomp_live_client_tick(1000u);
    captured_count = 0u;
    const RecompLiveRequest request = backend->logon(backend->self);
    check("a first request carries no cookie", captured_count == 1u && captured[0][14] == 0u);
    /* The transport wrote the nonce and kept the request; the client left both zero. */
    check("the transport writes the nonce", memcmp(captured[0] + 6u, test_nonce, 8u) == 0);
    uint8_t cookie[22];
    memcpy(cookie, captured[0], 14u);
    cookie[5] = 10u; /* the COOKIE answer */
    for (unsigned i = 0u; i < 8u; ++i) cookie[14u + i] = (uint8_t)(0xa0u + i);
    feed(cookie, sizeof cookie); /* the COOKIE answer, header and nonce and all */
    check("a cookie answer sends the ask again", captured_count == 2u &&
        memcmp(captured[1] + 14u, cookie + 14u, 8u) == 0 &&
        memcmp(captured[1] + 6u, test_nonce, 8u) == 0 &&
        captured_size[1] == captured_size[0]);
    feed(cookie, sizeof cookie);
    check("the cookie retry happens once", captured_count == 2u);
    /* An answer for another socket's nonce is not the server's. */
    uint8_t forged[22];
    memcpy(forged, captured[0], 14u);
    forged[4] = 2u;
    forged[5] = 25u; /* a DONE with request 0 */
    forged[6] ^= 0xffu;
    memset(forged + 14, 0, 8u);
    feed(forged, sizeof forged);
    uint32_t hresult = 0u;
    check("a packet without this nonce is dropped",
        backend->poll(backend->self, request, &hresult) == RECOMP_LIVE_PENDING &&
        captured_count == 2u);
    backend->release(backend->self, request);
}

static bool writeFile(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    const bool wrote = fwrite(bytes, 1, size, file) == size;
    fclose(file);
    return wrote;
}

static void checkIdentityFile(void)
{
    const char *path = "live-identity-test.bin";
    remove(path);
#ifdef _WIN32
    _putenv_s("RECOMP_LIVE_IDENTITY", path);
#else
    setenv("RECOMP_LIVE_IDENTITY", path, 1);
#endif
    RecompLiveIdentity first, second;
    check("a first load makes a secret", recomp_live_identity_load(NULL, &first) && first.xuid != 0u);
    check("the secret is kept", recomp_live_identity_load(NULL, &second) &&
        memcmp(first.secret, second.secret, sizeof first.secret) == 0 && first.xuid == second.xuid);
    uint8_t short_file[31] = {0u};
    check("a short file is written", writeFile(path, short_file, sizeof short_file));
    check("a damaged file is refused", !recomp_live_identity_load(NULL, &second));
    remove(path);
#ifdef _WIN32
    _putenv_s("RECOMP_LIVE_IDENTITY", "");
#else
    unsetenv("RECOMP_LIVE_IDENTITY");
#endif
}

static char *readFile(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return NULL;
    }
    char *text = (char *)malloc((size_t)length + 1u);
    if (text == NULL || fread(text, 1, (size_t)length, file) != (size_t)length) {
        free(text);
        fclose(file);
        return NULL;
    }
    text[length] = '\0';
    fclose(file);
    *size = (size_t)length;
    return text;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : getenv("RECOMP_LIVE_VECTORS");
    size_t size = 0u;
    char *file = path != NULL ? readFile(path, &size) : NULL;
    if (file == NULL) {
        fprintf(stderr, "FAIL cannot read %s\n", path != NULL ? path : "tools/live_vectors.json");
        return 1;
    }
    document = file;
    check("the vectors are protocol 2", readNumber(document, "protocol", 0u) == 2u);
    uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    const char *identity = findKey(document, "identity");
    check("the vectors carry a secret", identity != NULL &&
        readHex(findKey(identity, "secret"), secret, sizeof secret) == sizeof secret);
    RecompLiveIdentity mine;
    recomp_live_identity_from_secret(secret, &mine);
    check("the XUID comes from the secret", mine.xuid == readNumber(identity, "xuid", 0u));
    uint8_t proof[RECOMP_LIVE_IDENTITY_PROOF_BYTES];
    check("the proof comes from the secret",
        readHex(findKey(identity, "proof"), proof, sizeof proof) == sizeof proof &&
        memcmp(proof, mine.proof, sizeof proof) == 0);
    check("the vectors carry a nonce", readHex(findKey(document, "nonce"), test_nonce, sizeof test_nonce) == 8u);
    check("the vectors carry a digest",
        readString(document, "digest", transcript_digest, sizeof transcript_digest) == 64u);
    check("the vectors carry a gamertag", readString(document, "gamertag", transcript_tag, sizeof transcript_tag) != 0u);
    backend = recomp_live_client_attach(&mine, transcript_tag, transcript_digest);
    check("attach", backend != NULL);
    check("the backend is attached", recomp_live_client_attached());

    char object[16384]; /* a group-7 step carries a 61-stat request, answer and stats list */
    const char *cursor = findKey(document, "steps");
    uint64_t now = 0u;
    unsigned steps = 0u;
    while (cursor != NULL && nextStep(&cursor, object, sizeof object) != NULL) {
        Step step;
        parseStep(object, &step);
        now += 16u;
        recomp_live_client_tick(now);
        runStep(&step, now);
        ++steps;
    }
    check("every kind is in the vectors", steps == 26u);

    checkRetriesAndTimeout(&mine);
    checkConnectRetry(&mine);
    checkHeartbeatRefused(&mine);
    checkCookie(&mine);
    checkPushSlots(&mine);
    checkHeartbeat(&mine);
    checkStatTypes(&mine);
    checkGroupChildren(&mine);
    checkIdentityFile();

    recomp_live_client_detach();
    check("detach", !recomp_live_client_attached() && server_handler == NULL);
    free(file);
    if (failures != 0) fprintf(stderr, "%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
