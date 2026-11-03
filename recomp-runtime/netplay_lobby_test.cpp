#include "netplay.h"

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

static std::vector<std::uint8_t> peerPacket(std::uint16_t port)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 1u, 6u};
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
    bool opened = recomp_netplay_listen(&server, 47622u) && recomp_netplay_listen(&host, 47620u) &&
        recomp_netplay_listen(&joiner, 47621u) && recomp_netplay_match_server(&host, "127.0.0.1", 47622u) &&
        recomp_netplay_match_server(&joiner, "127.0.0.1", 47622u);
    sendFrom(server, 47620u, peerPacket(toHost));
    sendFrom(server, 47621u, peerPacket(toJoiner));
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
            if (got > 6 && packet[5] == 9u) // relay to the other room, as the server does
                sendFrom(server, ntohs(from.sin_port) == 47620u ? 47621u : 47620u,
                    std::vector<std::uint8_t>(packet, packet + got));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    relayed = host.match.relay && joiner.match.relay;
    const bool met = hostRoom.pinged && joinerRoom.pinged && std::strcmp(hostRoom.name, "JOINER") == 0 &&
        std::strcmp(joinerRoom.name, "HOST") == 0 && !host.match.punching && !joiner.match.punching;
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

    // A different build is refused on both sides.
    const RecompNetplayLobby other = lobby('b', 2u, 0u);
    wait = std::thread([&] { hosted = hostOn(&host, 47612u, &offered, &none, 5000u, nullptr); });
    asked = other;
    const RecompNetplayLobbyResult refused =
        joinTo(&joiner, 47612u, &asked, &noneJoiner, 5000u, nullptr);
    wait.join();
    check("mismatch", refused == RECOMP_NETPLAY_LOBBY_MISMATCH && hosted == RECOMP_NETPLAY_LOBBY_MISMATCH);
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
    wait = std::thread([&] { hosted = hostOn(&host, 47617u, &offered, &none, 5000u, cancelled); });
    std::thread watch([&] { // cancel once the host has met the joiner, while the 1 MiB profile is in flight
        for (int i = 0; i < 500 && host.peer_port == 0u; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cancelNow = true;
    });
    asked = offered;
    const RecompNetplayLobbyResult sentBack =
        joinTo(&joiner, 47617u, &asked, &bigJoiner, 5000u, nullptr);
    wait.join();
    watch.join();
    check("host cancels mid-exchange", hosted == RECOMP_NETPLAY_LOBBY_CANCELLED && !host.peer_left &&
        sentBack == RECOMP_NETPLAY_LOBBY_CANCELLED && joiner.peer_left);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A joiner cancelling sends the host back.
    cancelNow = false;
    wait = std::thread([&] { hosted = hostOn(&host, 47618u, &offered, &none, 5000u, nullptr); });
    watch = std::thread([&] {
        for (int i = 0; i < 500 && host.peer_port == 0u; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cancelNow = true;
    });
    asked = offered;
    check("joiner cancels", joinTo(&joiner, 47618u, &asked, &bigJoiner, 5000u,
        cancelled) == RECOMP_NETPLAY_LOBBY_CANCELLED);
    wait.join();
    watch.join();
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
    recomp_netplay_room_say(&joinerRoom, "bell\a");
    for (int i = 0; i < 25; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
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

    // Matched rooms punch through to each other, or meet through the relay without
    // endpoints, or after 5 s of punching at endpoints that never answer.
    bool relayed = true;
    check("matched rooms punch through", matchRooms(47621u, 47620u, 2000, relayed) && !relayed);
    check("matched rooms relay without endpoints", matchRooms(0u, 0u, 2000, relayed) && relayed);
    check("matched rooms relay when punching fails", matchRooms(47629u, 47628u, 7000, relayed) && relayed);

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, swaps profiles, refuses another and runs a room");
    return 0;
}
