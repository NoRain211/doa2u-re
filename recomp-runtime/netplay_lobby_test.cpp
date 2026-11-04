#include "netplay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using RawSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
using RawSocket = int;
#endif

static int failures;
static std::atomic<bool> cancelNow;
static bool cancelled()
{
    return cancelNow;
}

// Each side cancels at a fixed point of the exchange from its own thread, so load cannot move the cancel.
static const RecompNetplay *metHost;
static bool hostMet()
{
    return metHost->peer_port != 0u; // set as the host welcomes the joiner, before any profile arrives
}
static const RecompNetplayLobby *welcomed;
static bool joinerWelcomed()
{
    return welcomed->delay != 0xffu; // the host's welcome overwrites the joiner's marker
}

static void check(const char *name, bool passed)
{
    if (!passed) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

// The lobby runs on a socket its caller opens, as a room's is handed over.
static RecompNetplayLobbyResult hostOn(RecompNetplay *net, std::uint16_t port, const RecompNetplayLobby *offered,
    RecompNetplayProfiles *profiles, std::uint32_t timeout, bool (*cancel)())
{
    return recomp_netplay_listen(net, port) ? recomp_netplay_host(net, offered, profiles, timeout, cancel)
                                            : RECOMP_NETPLAY_LOBBY_SOCKET;
}

static RecompNetplayLobbyResult joinTo(RecompNetplay *net, std::uint16_t port, RecompNetplayLobby *asked,
    RecompNetplayProfiles *profiles, std::uint32_t timeout, bool (*cancel)())
{
    return recomp_netplay_open(net, 0u, "127.0.0.1", port) ? recomp_netplay_join(net, asked, profiles, timeout, cancel)
                                                         : RECOMP_NETPLAY_LOBBY_SOCKET;
}

static RecompNetplayLobby lobby(char build, std::uint8_t delay, std::uint8_t flags)
{
    RecompNetplayLobby value;
    std::memset(value.digest, build, sizeof value.digest);
    value.delay = delay;
    value.flags = flags;
    return value;
}

// A socket standing in for tools/matchmaker.py: it pairs two rooms on 127.0.0.1 and relays between them.
static void sendFrom(const RecompNetplay &from, std::uint16_t port, const std::vector<std::uint8_t> &packet)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(0x7f000001u);
    sendto(static_cast<RawSocket>(from.socket), reinterpret_cast<const char *>(packet.data()),
        static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr *>(&to), sizeof to);
}

// netplay.c's lobby packets: "D2LB", LOBBY_VERSION, kind, then the body.
static std::vector<std::uint8_t> helloPacket()
{
    std::vector<std::uint8_t> hello = {'D', '2', 'L', 'B', 4u, 1u};
    hello.insert(hello.end(), RECOMP_NETPLAY_DIGEST_BYTES, static_cast<std::uint8_t>('a'));
    hello.resize(hello.size() + 6u); // delay, flags, no profile
    return hello;
}

// Counts the lobby packets of one kind waiting on a socket.
static int drainKind(const RecompNetplay &net, std::uint8_t kind)
{
    int count = 0;
    std::uint8_t packet[2048];
    for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(net.socket), reinterpret_cast<char *>(packet),
                       sizeof packet, 0))) > 0;)
        count += got > 6 && std::memcmp(packet, "D2LB", 4) == 0 && packet[5] == kind;
    return count;
}

// PEER, to the socket whose nonce it carries.
static std::vector<std::uint8_t> peerPacket(std::uint16_t port, const std::uint8_t *nonce)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 2u, 6u};
    packet.insert(packet.end(), nonce, nonce + RECOMP_NETPLAY_TOKEN_BYTES);
    const std::uint8_t endpoint[6] = {127u, 0u, 0u, 1u, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port)};
    if (port != 0u) packet.insert(packet.end(), endpoint, endpoint + 6);
    else packet.resize(packet.size() + 6u);
    packet.resize(packet.size() + 6u + 1u + RECOMP_NETPLAY_NAME_BYTES); // no LAN endpoint, no name
    return packet;
}

// Pairs a host room on 47620 and a joiner room on 47621, telling each the other's port (0 for none).
static bool matchRooms(std::uint16_t toHost, std::uint16_t toJoiner, int milliseconds, bool &relayed)
{
    RecompNetplay server, host, joiner;
    // Every listen runs, even after one fails, so each socket is set before the closes below.
    const bool listening[3] = {recomp_netplay_listen(&server, 47622u), recomp_netplay_listen(&host, 47620u),
        recomp_netplay_listen(&joiner, 47621u)};
    bool opened = listening[0] && listening[1] && listening[2] &&
        recomp_netplay_match_server(&host, "127.0.0.1", 47622u) &&
        recomp_netplay_match_server(&joiner, "127.0.0.1", 47622u);
    sendFrom(server, 47620u, peerPacket(toHost, host.match.nonce));
    sendFrom(server, 47621u, peerPacket(toJoiner, joiner.match.nonce));
    RecompNetplayRoom hostRoom{}, joinerRoom{};
    hostRoom.hosting = true;
    char digest[RECOMP_NETPLAY_DIGEST_BYTES];
    std::memset(digest, 'a', sizeof digest);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (opened && std::chrono::steady_clock::now() < end && !(hostRoom.pinged && joinerRoom.pinged)) {
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
        std::uint8_t packet[2048];
        sockaddr_in from{};
#ifdef _WIN32
        int size = sizeof from;
#else
        socklen_t size = sizeof from;
#endif
        for (int got; (got = static_cast<int>(recvfrom(static_cast<RawSocket>(server.socket),
                           reinterpret_cast<char *>(packet), sizeof packet, 0, reinterpret_cast<sockaddr *>(&from),
                           &size))) > 0;) {
            // Relay to the other room, as the server does: the header, the receiver's nonce, then what came
            // after the sender's nonce and cookie.
            if (got >= 6 + 2 * RECOMP_NETPLAY_TOKEN_BYTES && packet[5] == 9u) {
                const bool toJoiner = ntohs(from.sin_port) == 47620u;
                const std::uint8_t *nonce = toJoiner ? joiner.match.nonce : host.match.nonce;
                std::vector<std::uint8_t> relayed(packet, packet + 6);
                relayed.insert(relayed.end(), nonce, nonce + RECOMP_NETPLAY_TOKEN_BYTES);
                relayed.insert(relayed.end(), packet + 6 + 2 * RECOMP_NETPLAY_TOKEN_BYTES, packet + got);
                sendFrom(server, toJoiner ? 47621u : 47620u, relayed);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    relayed = host.match.relay && joiner.match.relay;
    bool met = hostRoom.pinged && joinerRoom.pinged && std::strcmp(hostRoom.name, "JOINER") == 0 &&
        std::strcmp(joinerRoom.name, "HOST") == 0 && !host.match.punching && !joiner.match.punching;
    // A late pairing for another player leaves the host's talking peer alone.
    const std::uint16_t peer = host.peer_port;
    const bool hostRelay = host.match.relay;
    sendFrom(server, 47620u, peerPacket(47630u, host.match.nonce));
    for (int i = 0; i < 5; ++i) {
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    met = met && host.peer_port == peer && host.match.relay == hostRelay && !host.match.punching;
    if (!met) // one failure under load has not been explained; this says which side stalled next time
        std::fprintf(stderr, "rooms %u/%u: opened %d, pinged %d/%d, names %s/%s, punching %d/%d, relay %d/%d\n",
            toHost, toJoiner, (int)opened, (int)hostRoom.pinged, (int)joinerRoom.pinged, hostRoom.name,
            joinerRoom.name, (int)host.match.punching, (int)joiner.match.punching, (int)host.match.relay,
            (int)joiner.match.relay);
    recomp_netplay_close(&server);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);
    return met;
}

int main()
{
    // The joiner adopts the host's settings, the two swap profiles, and the host learns the joiner's address.
    RecompNetplay host, joiner;
    RecompNetplayLobbyResult hosted = RECOMP_NETPLAY_LOBBY_SOCKET;
    const RecompNetplayLobby offered = lobby('a', 2u, 3u);
    std::vector<std::uint8_t> hostSent(70000u), joinerSent(5000u), hostGot(RECOMP_NETPLAY_MAX_PROFILE),
        joinerGot(RECOMP_NETPLAY_MAX_PROFILE);
    for (std::size_t i = 0; i < hostSent.size(); ++i) hostSent[i] = static_cast<std::uint8_t>(i * 7u + i / 1024u);
    for (std::size_t i = 0; i < joinerSent.size(); ++i) joinerSent[i] = static_cast<std::uint8_t>(i * 13u + 1u);
    RecompNetplayProfiles hostProfiles{hostSent.data(), static_cast<std::uint32_t>(hostSent.size()), hostGot.data(), 0u};
    RecompNetplayProfiles joinerProfiles{joinerSent.data(), static_cast<std::uint32_t>(joinerSent.size()),
        joinerGot.data(), 0u};
    std::thread wait([&] { hosted = hostOn(&host, 47611u, &offered, &hostProfiles, 5000u, nullptr); });
    RecompNetplayLobby asked = lobby('a', 0u, 0u);
    const RecompNetplayLobbyResult joined =
        joinTo(&joiner, 47611u, &asked, &joinerProfiles, 5000u, nullptr);
    wait.join();
    check("join", joined == RECOMP_NETPLAY_LOBBY_OK && hosted == RECOMP_NETPLAY_LOBBY_OK);
    check("settings adopted", asked.delay == 2u && asked.flags == 3u);
    check("host profile copied", joinerProfiles.theirs_size == hostSent.size() &&
        std::memcmp(joinerGot.data(), hostSent.data(), hostSent.size()) == 0);
    check("joiner profile copied", hostProfiles.theirs_size == joinerSent.size() &&
        std::memcmp(hostGot.data(), joinerSent.data(), joinerSent.size()) == 0);
    check("host knows joiner", host.peer_port != 0u);

    // Pads then cross both ways on the lobby's sockets.
    RecompRollbackSession a, b;
    recomp_rollback_init(&a, 0u);
    recomp_rollback_init(&b, 0u);
    RecompInputGamepad pad{};
    pad.buttons = 5u;
    recomp_rollback_add_local(&a, &pad);
    recomp_rollback_add_local(&b, &pad);
    for (int i = 0; i < 50 && (a.remote_count == 0u || b.remote_count == 0u); ++i) {
        recomp_netplay_send(&host, &a, 0);
        recomp_netplay_send(&joiner, &b, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        recomp_netplay_receive(&host, &a);
        recomp_netplay_receive(&joiner, &b);
    }
    check("pads cross", a.remote_count == 1u && b.remote_count == 1u);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Players without profiles send none.
    RecompNetplayProfiles none{nullptr, 0u, hostGot.data(), 1u};
    RecompNetplayProfiles noneJoiner{nullptr, 0u, joinerGot.data(), 1u};
    wait = std::thread([&] { hosted = hostOn(&host, 47614u, &offered, &none, 5000u, nullptr); });
    asked = lobby('a', 0u, 0u);
    check("no profile", joinTo(&joiner, 47614u, &asked, &noneJoiner, 5000u, nullptr) ==
        RECOMP_NETPLAY_LOBBY_OK && noneJoiner.theirs_size == 0u);
    wait.join();
    check("no profile host", hosted == RECOMP_NETPLAY_LOBBY_OK && none.theirs_size == 0u);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A different build is refused, and the host waits on for the right one.
    const RecompNetplayLobby other = lobby('b', 2u, 0u);
    wait = std::thread([&] { hosted = hostOn(&host, 47612u, &offered, &none, 5000u, nullptr); });
    asked = other;
    const RecompNetplayLobbyResult refused =
        joinTo(&joiner, 47612u, &asked, &noneJoiner, 5000u, nullptr);
    recomp_netplay_close(&joiner);
    asked = offered;
    const RecompNetplayLobbyResult retried =
        joinTo(&joiner, 47612u, &asked, &noneJoiner, 5000u, nullptr);
    wait.join();
    check("mismatch", refused == RECOMP_NETPLAY_LOBBY_MISMATCH && retried == RECOMP_NETPLAY_LOBBY_OK &&
        hosted == RECOMP_NETPLAY_LOBBY_OK);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Nobody hosting: the joiner gives up.
    asked = offered;
    check("timeout", joinTo(&joiner, 47613u, &asked, &noneJoiner, 300u, nullptr) ==
        RECOMP_NETPLAY_LOBBY_TIMEOUT);
    recomp_netplay_close(&joiner);

    // A host cancelling alone ends its wait; one cancelling mid-exchange sends the joiner back too.
    cancelNow = true;
    check("host cancels alone", hostOn(&host, 47617u, &offered, &none, 5000u, cancelled) ==
        RECOMP_NETPLAY_LOBBY_CANCELLED && !host.peer_left);
    recomp_netplay_close(&host);
    cancelNow = false;
    std::vector<std::uint8_t> big(RECOMP_NETPLAY_MAX_PROFILE, 7u);
    RecompNetplayProfiles bigJoiner{big.data(), static_cast<std::uint32_t>(big.size()), joinerGot.data(), 0u};
    // The host cancels as it meets the joiner, while the 1 MiB profile is in flight.
    metHost = &host;
    wait = std::thread([&] { hosted = hostOn(&host, 47617u, &offered, &none, 5000u, hostMet); });
    asked = offered;
    const RecompNetplayLobbyResult sentBack =
        joinTo(&joiner, 47617u, &asked, &bigJoiner, 5000u, nullptr);
    wait.join();
    check("host cancels mid-exchange", hosted == RECOMP_NETPLAY_LOBBY_CANCELLED && !host.peer_left &&
        sentBack == RECOMP_NETPLAY_LOBBY_CANCELLED && joiner.peer_left);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A joiner cancelling as the host's welcome arrives, with its profile in flight, sends the host back.
    wait = std::thread([&] { hosted = hostOn(&host, 47618u, &offered, &none, 5000u, nullptr); });
    asked = offered;
    asked.delay = 0xffu;
    welcomed = &asked;
    check("joiner cancels", joinTo(&joiner, 47618u, &asked, &bigJoiner, 5000u,
        joinerWelcomed) == RECOMP_NETPLAY_LOBBY_CANCELLED);
    wait.join();
    check("host hears the cancel", hosted == RECOMP_NETPLAY_LOBBY_CANCELLED && host.peer_left);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A host back in its room sends a joiner still calling the cancelled match back to its room.
    RecompNetplayRoom backRoom{};
    backRoom.hosting = true;
    std::atomic<bool> calling{true};
    check("room for a late joiner", recomp_netplay_listen(&host, 47619u));
    wait = std::thread([&] {
        while (calling) {
            recomp_netplay_room_tick(&host, &backRoom, offered.digest, "HOST", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    asked = offered;
    check("late joiner sent back", joinTo(&joiner, 47619u, &asked, &noneJoiner, 2000u,
        nullptr) == RECOMP_NETPLAY_LOBBY_CANCELLED && joiner.peer_left);
    calling = false;
    wait.join();
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // In a room the two hear each other's names, readiness and ping, and the joiner hears the start.
    char digest[RECOMP_NETPLAY_DIGEST_BYTES], otherDigest[RECOMP_NETPLAY_DIGEST_BYTES];
    std::memset(digest, 'a', sizeof digest);
    std::memset(otherDigest, 'b', sizeof otherDigest);
    RecompNetplayRoom hostRoom{}, joinerRoom{};
    hostRoom.hosting = true;
    check("room opens", recomp_netplay_listen(&host, 47615u) &&
        recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47615u));
    for (int i = 0; i < 100 && !(joinerRoom.start && hostRoom.pinged && joinerRoom.pinged); ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, hostRoom.ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("room names", std::strcmp(hostRoom.name, "JOINER") == 0 && std::strcmp(joinerRoom.name, "HOST") == 0);
    check("room ready and start", hostRoom.ready && joinerRoom.start && !joinerRoom.ready);
    check("room ping", hostRoom.pinged && joinerRoom.pinged && hostRoom.ping_ms < 1000u &&
        joinerRoom.ping_ms < 1000u);
    check("room same build", hostRoom.joined && joinerRoom.joined && !hostRoom.mismatch && !joinerRoom.mismatch);

    // A chat line arrives once, and one that is not printable ASCII never does.
    recomp_netplay_room_say(&joinerRoom, "gg, one more?");
    for (int i = 0; i < 50 && !hostRoom.heard_new; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("chat arrives", hostRoom.heard_new && std::strcmp(hostRoom.heard, "gg, one more?") == 0);
    hostRoom.heard_new = false;
    // The next line waits out the receiver's second, then is read and dropped as not printable.
    recomp_netplay_room_say(&joinerRoom, "bell\a");
    const auto bellSaid = std::chrono::steady_clock::now();
    for (int i = 0; i < 100 && hostRoom.heard_id != joinerRoom.say_id; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("chat a line a second", hostRoom.heard_id == joinerRoom.say_id &&
        std::chrono::steady_clock::now() - bellSaid >= std::chrono::milliseconds(900));
    check("chat once, printable only", !hostRoom.heard_new);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    hostRoom = RecompNetplayRoom{};
    joinerRoom = RecompNetplayRoom{};
    hostRoom.hosting = true;
    check("room reopens", recomp_netplay_listen(&host, 47616u) &&
        recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47616u));
    for (int i = 0; i < 100 && !(hostRoom.joined && joinerRoom.joined); ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, otherDigest, "JOINER", false, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("room other build", hostRoom.mismatch && joinerRoom.mismatch);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A room packet from an address that never heard this host draws one room packet, not ten a second.
    {
        RecompNetplay forger;
        RecompNetplayRoom doubtedRoom{};
        doubtedRoom.hosting = true;
        check("doubted room opens", recomp_netplay_listen(&host, 47643u) &&
            recomp_netplay_open(&forger, 0u, "127.0.0.1", 47643u));
        std::vector<std::uint8_t> roomPacket = {'D', '2', 'L', 'B', 4u, 5u};
        roomPacket.insert(roomPacket.end(), RECOMP_NETPLAY_DIGEST_BYTES, static_cast<std::uint8_t>('a'));
        roomPacket.resize(159u); // flags, name, clocks with none heard, no chat
        sendFrom(forger, 47643u, roomPacket);
        int answers = 0;
        for (int i = 0; i < 25; ++i) {
            recomp_netplay_room_tick(&host, &doubtedRoom, digest, "HOST", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            answers += drainKind(forger, 5u);
        }
        check("doubted peer answered one for one", doubtedRoom.joined && answers == 1);
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
    }

    // A flood of chunk requests draws one 16-chunk burst per 50 ms, not 16 chunks per request.
    {
        RecompNetplay flooder;
        RecompNetplayProfiles served{hostSent.data(), static_cast<std::uint32_t>(hostSent.size()), hostGot.data(), 0u};
        cancelNow = false;
        wait = std::thread([&] { hosted = hostOn(&host, 47640u, &offered, &served, 5000u, cancelled); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check("flood socket", recomp_netplay_open(&flooder, 0u, "127.0.0.1", 47640u));
        const std::vector<std::uint8_t> hello = helloPacket();
        const std::vector<std::uint8_t> request = {'D', '2', 'L', 'B', 4u, 3u, 0u, 0u, 0u, 0u};
        int chunks = 0;
        const auto drain = [&] {
            std::uint8_t packet[2048];
            for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(flooder.socket),
                               reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
                chunks += got > 6 && std::memcmp(packet, "D2LB", 4) == 0 && packet[5] == 4u;
        };
        // Say hello until the host welcomes it, so a slow host thread cannot drop the only one.
        bool welcomed = false;
        for (int i = 0; i < 100 && !welcomed; ++i) {
            sendFrom(flooder, 47640u, hello);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::uint8_t packet[2048];
            for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(flooder.socket),
                               reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
                welcomed = welcomed || (got > 6 && std::memcmp(packet, "D2LB", 4) == 0 && packet[5] == 2u);
        }
        check("flood welcomed", welcomed);
        for (int i = 0; i < 200; ++i) {
            sendFrom(flooder, 47640u, request);
            drain();
        }
        for (int i = 0; i < 15; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            drain();
        }
        cancelNow = true;
        wait.join();
        cancelNow = false;
        check("request flood throttled", chunks > 0 && chunks <= 160);
        if (chunks == 0 || chunks > 160) std::fprintf(stderr, "flood drew %d chunks\n", chunks);
        recomp_netplay_close(&flooder);
        recomp_netplay_close(&host);
    }

    // A room's socket arrives with its peer, so a hello from any other address cannot take the match.
    {
        RecompNetplay forger;
        check("handed-over socket", recomp_netplay_listen(&host, 47641u) &&
            recomp_netplay_open(&forger, 0u, "127.0.0.1", 47641u));
        host.peer_address = htonl(0x7f000001u);
        host.peer_port = htons(47642u);
        wait = std::thread([&] { hosted = recomp_netplay_host(&host, &offered, &none, 5000u, nullptr); });
        int welcomes = 0;
        for (int i = 0; i < 10; ++i) {
            sendFrom(forger, 47641u, helloPacket());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            welcomes += drainKind(forger, 2u);
        }
        asked = offered;
        const bool joinedPeer = recomp_netplay_open(&joiner, 47642u, "127.0.0.1", 47641u) &&
            recomp_netplay_join(&joiner, &asked, &noneJoiner, 5000u, nullptr) == RECOMP_NETPLAY_LOBBY_OK;
        wait.join();
        welcomes += drainKind(forger, 2u);
        check("forged hello ignored", welcomes == 0 && joinedPeer && hosted == RECOMP_NETPLAY_LOBBY_OK &&
            host.peer_port == htons(47642u));
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
    }

    // A packet from the matchmaker's address that does not echo the socket's nonce is forged, and ignored.
    {
        RecompNetplay server, room;
        const bool listening[2] = {recomp_netplay_listen(&server, 47644u), recomp_netplay_listen(&room, 47645u)};
        const bool opened = listening[0] && listening[1] && recomp_netplay_match_server(&room, "127.0.0.1", 47644u);
        std::uint8_t wrong[RECOMP_NETPLAY_TOKEN_BYTES];
        std::memcpy(wrong, room.match.nonce, sizeof wrong);
        wrong[0] ^= 1u;
        sendFrom(server, 47645u, peerPacket(47630u, wrong));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        const bool ignored = !room.match.met;
        sendFrom(server, 47645u, peerPacket(47630u, room.match.nonce));
        for (int i = 0; i < 50 && !room.match.met; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            recomp_netplay_match_poll(&room);
        }
        check("forged matchmaker packet ignored", opened && ignored && room.match.met);

        // A HOST without a cookie draws one, and the room says HOST again with it.
        const auto heardHost = [&] {
            std::vector<std::uint8_t> got(2048);
            for (int i = 0; i < 50; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(10))) {
                const int size = static_cast<int>(recv(static_cast<RawSocket>(server.socket),
                    reinterpret_cast<char *>(got.data()), static_cast<int>(got.size()), 0));
                if (size > 6 + 2 * RECOMP_NETPLAY_TOKEN_BYTES && got[5] == 1u) return got;
            }
            return std::vector<std::uint8_t>();
        };
        recomp_netplay_match_host(&room, digest, 2u, true, "HOST");
        const std::vector<std::uint8_t> first = heardHost();
        std::vector<std::uint8_t> cookie = {'D', '2', 'M', 'M', 2u, 10u};
        cookie.insert(cookie.end(), room.match.nonce, room.match.nonce + RECOMP_NETPLAY_TOKEN_BYTES);
        for (std::uint8_t i = 1u; i <= RECOMP_NETPLAY_TOKEN_BYTES; ++i) cookie.push_back(i);
        sendFrom(server, 47645u, cookie);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        const std::vector<std::uint8_t> again = heardHost();
        check("cookie sent again", !first.empty() && !again.empty() &&
            std::all_of(first.begin() + 14, first.begin() + 22, [](std::uint8_t b) { return b == 0u; }) &&
            std::equal(cookie.begin() + 14, cookie.end(), again.begin() + 14));
        recomp_netplay_close(&server);
        recomp_netplay_close(&room);
    }

    // Matched rooms punch through to each other, or meet through the relay without
    // endpoints, or after 5 s of punching at endpoints that never answer.
    bool relayed = true;
    // Each waits only until both rooms meet; punching must win inside the 5 s before the relay takes over.
    check("matched rooms punch through", matchRooms(47621u, 47620u, 4500, relayed) && !relayed);
    check("matched rooms relay without endpoints", matchRooms(0u, 0u, 5000, relayed) && relayed);
    check("matched rooms relay when punching fails", matchRooms(47629u, 47628u, 10000, relayed) && relayed);

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, swaps profiles, refuses another and runs a room");
    return 0;
}
