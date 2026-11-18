/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Live client's start-up, which reads the netplay profile's name for the
   gamertag. That lives behind a C++ header, so this file is C++ and
   live_client.c stays plain C, which its test links without the runner. */
#include "live_client.h"

#include "live_transport.h"
#include "netplay.h"
#include "netplay_profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

/* The built-in matchmaker's host:port, XORed with 0x5a so it is not plain
   text in the source or the binary.
   shortcut: this only keeps the name out of a casual search; DNS lookups
   still show it. */
static std::string recomp_live_default_matchmaker()
{
    static const unsigned char coded[] = {0x37, 0x37, 0x74, 0x34, 0x3f, 0x3c, 0x3c, 0x39, 0x35, 0x28, 0x3e,
        0x74, 0x39, 0x35, 0x37, 0x60, 0x6e, 0x6d, 0x6d, 0x6a, 0x6a};
    std::string server;
    for (const unsigned char c : coded) server.push_back(static_cast<char>(c ^ 0x5au));
    return server;
}

namespace {

/* "host:port" as RECOMP_LIVE_SERVER and the built-in server give it. */
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

/* The name the game shows for this player: the profile's, cut to 15 bytes and
   refused when it fails the name check (netplay_names.c). */
std::string ownGamertag(const std::filesystem::path &storage)
{
    const std::string name = recomp_netplay_own_profile_name(storage / "partition1");
    std::string ascii;
    for (const char c : name) {
        if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7eu) continue;
        if (ascii.size() == 15u) break;
        ascii.push_back(c);
    }
    // Checked as shown: dropping bytes can join a banned word.
    return ascii.empty() || recomp_netplay_name_banned(ascii.c_str()) ? std::string("PLAYER") : ascii;
}

} // namespace

const RecompLiveBackend *recomp_live_client_start(const char *storage, const char *digest)
{
    if (storage == nullptr || storage[0] == '\0') return nullptr;
    const std::filesystem::path folder(storage);
    // Live is on by default, so the first run creates its storage before the first save does.
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    RecompLiveIdentity identity;
    if (!recomp_live_identity_load(storage, &identity)) {
        std::fprintf(stderr, "recomp live: no identity in %s\n", storage);
        return nullptr;
    }
    const std::string builtin = recomp_live_default_matchmaker();
    std::string builtinHost;
    std::uint16_t fallback = 0u;
    if (!splitServer(builtin, builtinHost, fallback)) {
        std::fprintf(stderr, "recomp live: the built-in matchmaker is not host:port\n");
        return nullptr;
    }
    std::string server = builtin;
    if (const char *override = std::getenv("RECOMP_LIVE_SERVER"); override != nullptr && override[0] != '\0')
        server = override;
    std::string host;
    std::uint16_t port = 0u;
    if (!splitServer(server, host, port)) {
        /* A bare host (RECOMP_LIVE_SERVER=host) takes the built-in server's port. */
        host = server;
        port = fallback;
    }
    if (!recomp_live_transport_open(host.c_str(), port)) {
        std::fprintf(stderr, "recomp live: the matchmaker name is too long\n");
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
