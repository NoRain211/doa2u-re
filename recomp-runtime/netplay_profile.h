/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_NETPLAY_PROFILE_H
#define DOA2U_RECOMP_NETPLAY_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

/* The player's profile name, which Live shows as the gamertag. */

// The save name on a profile's SaveMeta.xbx "Name=" line (UTF-16), as UTF-8.
std::string recomp_netplay_profile_name(const std::uint8_t *bytes, std::size_t size);
// The profile this player saved last, as UDATA/<title>/<save>, or empty without one.
std::string recomp_netplay_latest_profile(const std::filesystem::path &root);
// The name on the profile this player saved last, or empty without one.
std::string recomp_netplay_own_profile_name(const std::filesystem::path &partition1);

#endif
