/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Live client's start-up, which reads the netplay side: the profile's
   name for the gamertag and the matchmaker NETPLAY SETTINGS keeps. Both live
   behind C++ headers, so this file is C++ and live_client.c stays plain C,
   which its test links without the netplay menu. */
#include "live_client.h"

#include "live_transport.h"
#include "netplay.h"
#include "netplay_profile.h"
#include "netplay_settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

/* netplay_menu.cpp holds the built-in matchmaker, coded so it is not plain
   text in the source; the menu and Live must name the same server. */
bool recomp_netplay_default_matchmaker(std::string &server);

namespace {

/* "host:port" as the settings and RECOMP_LIVE_SERVER give it. */
bool splitServer(const std::string &text, std::string &host, std::uint16_t &port)
{
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0u || colon + 1u == text.size()) return false;
    const unsigned long number = std::strtoul(text.c_str() + colon + 1u, nullptr, 10);
    if (number == 0u || number > 65535u) return false;
    host = text.substr(0u, colon);
    port = static_cast<std::uint16_t>(number);
    return true;
}

/* The port MATCHMAKER names, or the built-in server's when the settings name
   none. A bare RECOMP_LIVE_SERVER host takes this port, not a fixed one. */
bool matchmakerPort(const std::string &configured, std::uint16_t &port)
{
    std::string host;
    if (splitServer(configured, host, port)) return true;
    std::string builtin;
    if (!recomp_netplay_default_matchmaker(builtin)) return false;
    return splitServer(builtin, host, port);
}

/* The name the game shows for this player: the profile's, cut to 15 bytes and
   refused when it fails the same check the room names pass. */
std::string ownGamertag(const std::filesystem::path &storage)
{
    std::string name = recomp_netplay_own_profile_name(storage / "partition1");
    if (name.empty() || recomp_netplay_name_banned(name.c_str())) return "PLAYER";
    std::string ascii;
    for (const char c : name) {
        if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7eu) continue;
        if (ascii.size() == 15u) break;
        ascii.push_back(c);
    }
    return ascii.empty() ? std::string("PLAYER") : ascii;
}

} // namespace

const RecompLiveBackend *recomp_live_client_start(const char *storage, const char *digest)
{
    if (storage == nullptr || storage[0] == '\0') return nullptr;
    const std::filesystem::path folder(storage);
    RecompLiveIdentity identity;
    if (!recomp_live_identity_load(storage, &identity)) {
        std::fprintf(stderr, "recomp live: no identity in %s\n", storage);
        return nullptr;
    }
    RecompNetplaySettings settings;
    recomp_netplay_settings_load(folder / "netplay.txt", settings);
    const std::string configured = settings.matchmaker;
    std::uint16_t fallback = 0u;
    if (!matchmakerPort(configured, fallback)) {
        std::fprintf(stderr, "recomp live: the matchmaker is not host:port\n");
        return nullptr;
    }
    std::string server = configured;
    const char *override = std::getenv("RECOMP_LIVE_SERVER");
    if (override != nullptr && override[0] != '\0') {
        server = override;
    } else if (server.empty() && !recomp_netplay_default_matchmaker(server)) {
        std::fprintf(stderr, "recomp live: no matchmaker\n");
        return nullptr;
    }
    std::string host;
    std::uint16_t port = 0u;
    if (!splitServer(server, host, port)) {
        /* A bare host (RECOMP_LIVE_SERVER=host) takes the matchmaker's port:
           the one NETPLAY SETTINGS keep, or the built-in server's. */
        if (server.empty()) {
            std::fprintf(stderr, "recomp live: the matchmaker is not host:port\n");
            return nullptr;
        }
        host = server;
        port = fallback;
    }
    if (!recomp_live_transport_open(host.c_str(), port)) {
        std::fprintf(stderr, "recomp live: cannot open the socket\n");
        return nullptr;
    }
    const std::string gamertag = ownGamertag(folder);
    const std::string named = host + ":" + std::to_string(port);
    const RecompLiveBackend *backend = recomp_live_client_attach(&identity, gamertag.c_str(), digest, named.c_str());
    if (backend == nullptr) recomp_live_transport_close();
    return backend;
}

void recomp_live_client_stop(void)
{
    recomp_live_client_detach();
    recomp_live_transport_close();
}
