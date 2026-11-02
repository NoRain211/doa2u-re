#include "netplay.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static RecompNetplayLobby lobby(char build, std::uint8_t delay, std::uint8_t flags)
{
    RecompNetplayLobby value;
    std::memset(value.digest, build, sizeof value.digest);
    value.delay = delay;
    value.flags = flags;
    value.profile_size = 0u;
    return value;
}

int main()
{
    // The joiner adopts the host's settings and profile, and the host learns the joiner's address.
    RecompNetplay host, joiner;
    RecompNetplayLobbyResult hosted = RECOMP_NETPLAY_LOBBY_SOCKET;
    RecompNetplayLobby offered = lobby('a', 2u, 3u);
    std::vector<std::uint8_t> sent(70000u), received(RECOMP_NETPLAY_MAX_PROFILE);
    for (std::size_t i = 0; i < sent.size(); ++i) sent[i] = static_cast<std::uint8_t>(i * 7u + i / 1024u);
    offered.profile_size = static_cast<std::uint32_t>(sent.size());
    std::thread wait([&] { hosted = recomp_netplay_host(&host, 47611u, &offered, sent.data(), 5000u); });
    RecompNetplayLobby asked = lobby('a', 0u, 0u);
    const RecompNetplayLobbyResult joined =
        recomp_netplay_join(&joiner, "127.0.0.1", 47611u, &asked, received.data(), 5000u);
    wait.join();
    check("join", joined == RECOMP_NETPLAY_LOBBY_OK && hosted == RECOMP_NETPLAY_LOBBY_OK);
    check("settings adopted", asked.delay == 2u && asked.flags == 3u);
    check("profile copied", asked.profile_size == sent.size() &&
        std::memcmp(received.data(), sent.data(), sent.size()) == 0);
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

    // A host without a profile sends none.
    offered.profile_size = 0u;
    wait = std::thread([&] { hosted = recomp_netplay_host(&host, 47614u, &offered, nullptr, 5000u); });
    asked = lobby('a', 0u, 0u);
    asked.profile_size = 1u;
    check("no profile", recomp_netplay_join(&joiner, "127.0.0.1", 47614u, &asked, received.data(), 5000u) ==
        RECOMP_NETPLAY_LOBBY_OK && asked.profile_size == 0u);
    wait.join();
    check("no profile host", hosted == RECOMP_NETPLAY_LOBBY_OK);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A different build is refused on both sides.
    const RecompNetplayLobby other = lobby('b', 2u, 0u);
    wait = std::thread([&] { hosted = recomp_netplay_host(&host, 47612u, &offered, nullptr, 5000u); });
    asked = other;
    const RecompNetplayLobbyResult refused =
        recomp_netplay_join(&joiner, "127.0.0.1", 47612u, &asked, received.data(), 5000u);
    wait.join();
    check("mismatch", refused == RECOMP_NETPLAY_LOBBY_MISMATCH && hosted == RECOMP_NETPLAY_LOBBY_MISMATCH);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Nobody hosting: the joiner gives up.
    asked = offered;
    check("timeout", recomp_netplay_join(&joiner, "127.0.0.1", 47613u, &asked, received.data(), 300u) ==
        RECOMP_NETPLAY_LOBBY_TIMEOUT);
    recomp_netplay_close(&joiner);

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, copies its profile and refuses another");
    return 0;
}
