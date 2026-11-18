/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_profile.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static void write(const fs::path &path, const std::vector<std::uint8_t> &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

// A SaveMeta.xbx as the game writes it: UTF-16 with a byte order mark.
static std::vector<std::uint8_t> meta(const std::u16string &text)
{
    std::vector<std::uint8_t> bytes{0xffu, 0xfeu};
    for (const char16_t c : text) {
        bytes.push_back(static_cast<std::uint8_t>(c));
        bytes.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    return bytes;
}

int main()
{
    const auto root = fs::temp_directory_path() / ("recomp-netplay-profile-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path mine = root / "mine";

    // Names: the Name= line wherever it is, without the mark, zero-width spaces or controls, as UTF-8.
    const std::vector<std::uint8_t> kasumi = meta(u"Title=DOA2U\r\nName=Ka\u200bsumi \u00e9\u4e00\r\n");
    check("save name", recomp_netplay_profile_name(kasumi.data(), kasumi.size()) == "Kasumi \xc3\xa9\xe4\xb8\x80");
    check("no name line", recomp_netplay_profile_name(kasumi.data(), 8u).empty());
    // A surrogate pair is one 4-byte character; a lone surrogate becomes U+FFFD.
    std::u16string surrogates = u"Name=A\U0001F600";
    surrogates += static_cast<char16_t>(0xd800u);
    surrogates += u"B";
    const std::vector<std::uint8_t> emoji = meta(surrogates);
    check("surrogates", recomp_netplay_profile_name(emoji.data(), emoji.size()) == "A\xf0\x9f\x98\x80\xef\xbf\xbd" "B");

    // A profile, a second one, title saves and a file beside the profiles.
    const std::vector<std::uint8_t> data{1u, 2u, 3u}, title{4u}, beside{5u}, other{6u};
    const std::vector<std::uint8_t> ups(0x12148u, 7u); // ups.dat is always this size
    write(mine / "UDATA/54430006/0F515488395E/SaveMeta.xbx", kasumi);
    write(mine / "UDATA/54430006/0F515488395E/ups.dat", ups);
    write(mine / "UDATA/54430006/AAAAAAAAAAAA/SaveMeta.xbx", meta(u"Name=Other\r\n"));
    write(mine / "UDATA/54430006/AAAAAAAAAAAA/ups.dat", other);
    write(mine / "UDATA/54430006/TitleMeta.xbx", beside);
    write(mine / "TDATA/54430006/title.dat", title);
    // A newer save that is not a profile: the game never writes the files it would need, so it is never the one read.
    write(mine / "UDATA/54430006/BBBBBBBBBBBB/SaveMeta.xbx", meta(u"Name=Record\r\n"));
    write(mine / "UDATA/54430006/BBBBBBBBBBBB/br.dat", data);
    fs::last_write_time(mine / "UDATA/54430006/BBBBBBBBBBBB/br.dat",
        fs::last_write_time(mine / "UDATA/54430006/0F515488395E/ups.dat") + std::chrono::hours(1));
    fs::last_write_time(mine / "UDATA/54430006/AAAAAAAAAAAA/ups.dat",
        fs::last_write_time(mine / "UDATA/54430006/0F515488395E/ups.dat") - std::chrono::hours(1));
    fs::last_write_time(mine / "UDATA/54430006/AAAAAAAAAAAA/SaveMeta.xbx",
        fs::last_write_time(mine / "UDATA/54430006/0F515488395E/ups.dat") - std::chrono::hours(1));
    check("latest profile", recomp_netplay_latest_profile(mine) == "UDATA/54430006/0F515488395E");
    check("own name", recomp_netplay_own_profile_name(mine) == "Kasumi \xc3\xa9\xe4\xb8\x80");
    check("no profile", recomp_netplay_latest_profile(root / "missing").empty() &&
        recomp_netplay_own_profile_name(root / "missing").empty());

    std::error_code error;
    fs::remove_all(root, error);
    if (failures == 0) std::puts("netplay profile tests passed");
    return failures == 0 ? 0 : 1;
}
