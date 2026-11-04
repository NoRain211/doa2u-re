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
    return value;
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
    std::thread wait([&] { hosted = recomp_netplay_host(&host, 47611u, &offered, &hostProfiles, 5000u); });
    RecompNetplayLobby asked = lobby('a', 0u, 0u);
    const RecompNetplayLobbyResult joined =
        recomp_netplay_join(&joiner, "127.0.0.1", 47611u, &asked, &joinerProfiles, 5000u);
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
    wait = std::thread([&] { hosted = recomp_netplay_host(&host, 47614u, &offered, &none, 5000u); });
    asked = lobby('a', 0u, 0u);
    check("no profile", recomp_netplay_join(&joiner, "127.0.0.1", 47614u, &asked, &noneJoiner, 5000u) ==
        RECOMP_NETPLAY_LOBBY_OK && noneJoiner.theirs_size == 0u);
    wait.join();
    check("no profile host", hosted == RECOMP_NETPLAY_LOBBY_OK && none.theirs_size == 0u);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A different build is refused, and the host waits on for the right one.
    const RecompNetplayLobby other = lobby('b', 2u, 0u);
    wait = std::thread([&] { hosted = recomp_netplay_host(&host, 47612u, &offered, &none, 5000u); });
    asked = other;
    const RecompNetplayLobbyResult refused =
        recomp_netplay_join(&joiner, "127.0.0.1", 47612u, &asked, &noneJoiner, 5000u);
    recomp_netplay_close(&joiner);
    asked = offered;
    const RecompNetplayLobbyResult retried =
        recomp_netplay_join(&joiner, "127.0.0.1", 47612u, &asked, &noneJoiner, 5000u);
    wait.join();
    check("mismatch", refused == RECOMP_NETPLAY_LOBBY_MISMATCH && retried == RECOMP_NETPLAY_LOBBY_OK &&
        hosted == RECOMP_NETPLAY_LOBBY_OK);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Nobody hosting: the joiner gives up.
    asked = offered;
    check("timeout", recomp_netplay_join(&joiner, "127.0.0.1", 47613u, &asked, &noneJoiner, 300u) ==
        RECOMP_NETPLAY_LOBBY_TIMEOUT);
    recomp_netplay_close(&joiner);

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, swaps profiles and refuses another");
    return 0;
}
