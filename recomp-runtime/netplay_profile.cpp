/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

// File names here are ASCII, and Windows opens any case of one.
static std::string upper(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string recomp_netplay_profile_name(const std::uint8_t *bytes, std::size_t size)
{
    std::string line;
    for (std::size_t i = 0u; i + 1u < size; i += 2u) {
        const std::uint32_t c = bytes[i] | bytes[i + 1u] << 8;
        const std::uint32_t low = i + 3u < size ? bytes[i + 2u] | bytes[i + 3u] << 8 : 0u;
        if (c == '\r' || c == '\n') {
            if (line.rfind("Name=", 0u) == 0u) break;
            line.clear();
        } else if (c == 0xfeffu || c == 0x200bu || c < 0x20u) {
            continue; // byte order mark, the game's zero-width space, controls
        } else if (c < 0x80u) {
            line += static_cast<char>(c);
        } else if (c < 0x800u) {
            line += static_cast<char>(0xc0u | c >> 6);
            line += static_cast<char>(0x80u | (c & 0x3fu));
        } else if (c >= 0xd800u && c < 0xdc00u && low >= 0xdc00u && low < 0xe000u) {
            const std::uint32_t code = 0x10000u + ((c - 0xd800u) << 10) + (low - 0xdc00u);
            line += static_cast<char>(0xf0u | code >> 18);
            line += static_cast<char>(0x80u | (code >> 12 & 0x3fu));
            line += static_cast<char>(0x80u | (code >> 6 & 0x3fu));
            line += static_cast<char>(0x80u | (code & 0x3fu));
            i += 2u;
        } else if (c >= 0xd800u && c < 0xe000u) {
            line += "\xef\xbf\xbd"; // a lone surrogate, as U+FFFD
        } else {
            line += static_cast<char>(0xe0u | c >> 12);
            line += static_cast<char>(0x80u | (c >> 6 & 0x3fu));
            line += static_cast<char>(0x80u | (c & 0x3fu));
        }
    }
    return line.rfind("Name=", 0u) == 0u ? line.substr(5u) : std::string();
}

std::string recomp_netplay_latest_profile(const std::filesystem::path &root)
{
    namespace fs = std::filesystem;
    std::string latest;
    fs::file_time_type newest{};
    std::error_code error;
    for (const auto &title : fs::directory_iterator(root / "UDATA", error)) {
        for (const auto &save : fs::directory_iterator(title.path(), error)) {
            if (!fs::is_regular_file(save.path() / "SaveMeta.xbx", error)) continue;
            // min(), not {}: libstdc++'s file clock starts in 2174, so {} is later than every file.
            auto changed = fs::file_time_type::min();
            // Other saves have a SaveMeta.xbx too; a profile also has ups.dat.
            bool profile = false;
            for (const auto &file : fs::directory_iterator(save.path(), error)) {
                changed = std::max(changed, fs::last_write_time(file.path(), error));
                profile = profile || upper(file.path().filename().string()) == "UPS.DAT";
            }
            if (profile && (latest.empty() || changed > newest)) {
                latest = save.path().lexically_relative(root).generic_string();
                newest = changed;
            }
        }
    }
    return latest;
}

std::string recomp_netplay_own_profile_name(const std::filesystem::path &partition1)
{
    const std::string own = recomp_netplay_latest_profile(partition1);
    if (own.empty()) return {};
    std::ifstream meta(partition1 / own / "SaveMeta.xbx", std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(meta), {}};
    return recomp_netplay_profile_name(bytes.data(), bytes.size());
}
