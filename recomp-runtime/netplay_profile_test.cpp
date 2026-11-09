/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_profile.h"

#include "sha.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
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

static std::vector<std::uint8_t> read(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
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

// A blob with a correct hash, so only its entries decide.
struct Entry {
    std::uint8_t kind;
    std::string path;
    std::vector<std::uint8_t> bytes;
};
static std::vector<std::uint8_t> blobOf(const std::vector<Entry> &entries)
{
    std::vector<std::uint8_t> body;
    const auto put = [&body](std::size_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) body.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    };
    for (const Entry &entry : entries) {
        body.push_back(entry.kind);
        put(entry.path.size(), 2u);
        body.insert(body.end(), entry.path.begin(), entry.path.end());
        if (entry.kind != 1u) continue;
        put(entry.bytes.size(), 4u);
        body.insert(body.end(), entry.bytes.begin(), entry.bytes.end());
    }
    const std::string hash = doaxbv::sha256Hex(body);
    std::vector<std::uint8_t> blob(hash.begin(), hash.end());
    blob.insert(blob.end(), body.begin(), body.end());
    return blob;
}

static bool unpacks(const std::vector<std::uint8_t> &blob, const fs::path &root, bool joiner = false)
{
    std::vector<std::string> names;
    return recomp_netplay_unpack_profile(blob.data(), blob.size(), root, joiner, names);
}

int main()
{
    const auto root = fs::temp_directory_path() / ("recomp-netplay-profile-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path mine = root / "mine", theirs = root / "theirs";

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
    check("UTF-16 name", recomp_netplay_utf16("Kasumi \xc3\xa9\xe4\xb8\x80 A\xf0\x9f\x98\x80") == u"Kasumi \u00e9\u4e00 A\U0001F600");

    // A profile, a second one, title saves and a file beside the profiles.
    const std::vector<std::uint8_t> data{1u, 2u, 3u}, title{4u}, beside{5u}, other{6u};
    const std::vector<std::uint8_t> ups(0x12148u, 7u); // ups.dat is always this size
    write(mine / "UDATA/54430006/0F515488395E/SaveMeta.xbx", kasumi);
    write(mine / "UDATA/54430006/0F515488395E/ups.dat", ups);
    write(mine / "UDATA/54430006/AAAAAAAAAAAA/SaveMeta.xbx", meta(u"Name=Other\r\n"));
    write(mine / "UDATA/54430006/AAAAAAAAAAAA/ups.dat", other);
    write(mine / "UDATA/54430006/TitleMeta.xbx", beside);
    write(mine / "TDATA/54430006/title.dat", title);
    // A newer save that is not a profile: the other player would refuse it, so it is never the one sent.
    write(mine / "UDATA/54430006/BBBBBBBBBBBB/SaveMeta.xbx", meta(u"Name=Record\r\n"));
    write(mine / "UDATA/54430006/BBBBBBBBBBBB/br.dat", data);
    fs::last_write_time(mine / "UDATA/54430006/BBBBBBBBBBBB/br.dat",
        fs::last_write_time(mine / "UDATA/54430006/0F515488395E/ups.dat") + std::chrono::hours(1));
    fs::last_write_time(mine / "UDATA/54430006/AAAAAAAAAAAA/ups.dat",
        fs::last_write_time(mine / "UDATA/54430006/0F515488395E/ups.dat") - std::chrono::hours(1));
    fs::last_write_time(mine / "UDATA/54430006/AAAAAAAAAAAA/SaveMeta.xbx",
        fs::last_write_time(mine / "UDATA/54430006/0F515488395E/ups.dat") - std::chrono::hours(1));
    const std::string latest = recomp_netplay_latest_profile(mine);
    check("latest profile", latest == "UDATA/54430006/0F515488395E");
    check("own name", recomp_netplay_own_profile_name(mine) == "Kasumi \xc3\xa9\xe4\xb8\x80");

    // A host's blob carries its profile and the title saves; a joiner's only its profile.
    std::vector<std::uint8_t> host, joiner;
    check("pack host", recomp_netplay_pack_profile(mine, latest, true, host));
    check("pack joiner", recomp_netplay_pack_profile(mine, latest, false, joiner));
    std::vector<std::string> names;
    check("unpack host", recomp_netplay_unpack_profile(host.data(), host.size(), theirs / "host", false, names));
    check("host names", names == std::vector<std::string>{"Kasumi \xc3\xa9\xe4\xb8\x80"});
    check("host files", read(theirs / "host/UDATA/54430006/0F515488395E/ups.dat") == ups &&
        read(theirs / "host/TDATA/54430006/title.dat") == title &&
        read(theirs / "host/UDATA/54430006/TitleMeta.xbx") == beside &&
        !fs::exists(theirs / "host/UDATA/54430006/AAAAAAAAAAAA"));
    check("unpack joiner", unpacks(joiner, theirs / "joiner", true));
    check("joiner files", fs::exists(theirs / "joiner/UDATA/54430006/0F515488395E/ups.dat") &&
        !fs::exists(theirs / "joiner/TDATA") && !fs::exists(theirs / "joiner/UDATA/54430006/TitleMeta.xbx"));
    // A joiner sends one profile folder and never title saves.
    check("joiner title saves", !unpacks(host, theirs / "joiner-title", true));
    check("joiner two profiles", !unpacks(blobOf({{1u, "UDATA/54430006/AAAAAAAAAAAA/ups.dat", ups},
        {1u, "UDATA/54430006/BBBBBBBBBBBB/ups.dat", ups}}), theirs / "joiner-two", true));
    // A save folder holds only what the game writes, and ups.dat's framing is the game's to trust.
    const std::string save = "UDATA/54430006/AAAAAAAAAAAA/";
    check("a save as the game writes it", unpacks(blobOf({{1u, save + "SaveMeta.xbx", kasumi},
        {1u, save + "ups.dat", ups}, {1u, save + "SaveImage.xbx", std::vector<std::uint8_t>(4096u)}}),
        theirs / "save-ok", true));
    check("short ups.dat", !unpacks(blobOf({{1u, save + "ups.dat", std::vector<std::uint8_t>(0x14u)}}),
        theirs / "save-short", true));
    check("another file in a save", !unpacks(blobOf({{1u, save + "br.dat", data}}), theirs / "save-other", true));
    check("a folder in a save", !unpacks(blobOf({{0u, save + "sub", {}}}), theirs / "save-folder", true));
    check("joiner title file", !unpacks(blobOf({{1u, "UDATA/54430006/TitleMeta.xbx", beside}}),
        theirs / "joiner-title-file", true));
    check("meta without its mark", !unpacks(blobOf({{1u, save + "SaveMeta.xbx",
        std::vector<std::uint8_t>(kasumi.begin() + 2, kasumi.end())}}), theirs / "save-mark", true));
    check("meta naming twice", !unpacks(blobOf({{1u, save + "SaveMeta.xbx", meta(u"Name=A\r\nName=B\r\n")}}),
        theirs / "save-twice", true));
    // Windows opens SaveMeta.xbx in any case, so the filter reads that name too.
    names.clear();
    const std::vector<std::uint8_t> lower = blobOf({{1u, "UDATA/54430006/AAAAAAAAAAAA/savemeta.XBX", meta(u"Name=Low\r\n")}});
    check("any-case meta name", recomp_netplay_unpack_profile(lower.data(), lower.size(), theirs / "lower", true, names) &&
        names == std::vector<std::string>{"Low"});
    std::vector<std::uint8_t> none;
    check("pack no profile", recomp_netplay_pack_profile(root / "missing", "", false, none) && unpacks(none, theirs / "none"));

    // Damaged blobs.
    std::vector<std::uint8_t> flipped = host;
    flipped.back() ^= 1u;
    check("changed byte", !unpacks(flipped, theirs / "flipped"));
    check("short", !unpacks(std::vector<std::uint8_t>(host.begin(), host.begin() + 63), theirs / "short"));
    std::vector<std::uint8_t> cut = blobOf({{1u, "UDATA/a", data}});
    cut.erase(cut.end() - 1);
    const std::string body(cut.begin() + 64, cut.end());
    const std::string rehash = doaxbv::sha256Hex(std::vector<std::uint8_t>(body.begin(), body.end()));
    std::copy(rehash.begin(), rehash.end(), cut.begin());
    check("file cut short", !unpacks(cut, theirs / "cut"));
    check("unknown kind", !unpacks(blobOf({{2u, "UDATA/a", {}}}), theirs / "kind"));
    check("long path", !unpacks(blobOf({{0u, "UDATA/" + std::string(200u, 'a'), {}}}), theirs / "long"));
    std::vector<Entry> many(513u, Entry{0u, "UDATA/a", {}});
    check("too many entries", !unpacks(blobOf(many), theirs / "many"));

    // Paths that must never be written: outside UDATA and TDATA, not plain names, or Windows devices.
    const char *const unsafe[] = {"../escape", "UDATA/../../escape", "UDATA/./a", "UDATA//a", "/escape",
        "C:/escape", "UDATA\\..\\escape", "SYSTEM/a", "udata/a", "UDATA/a b", "UDATA/con", "UDATA/CON.txt",
        "UDATA/com1", "UDATA/LPT9.dat", "UDATA/a/", "", "UDATA/a:b", "UDATA/a/SaveMeta.xbx."};
    for (const char *path : unsafe) {
        const std::string name = std::string("unsafe \"") + path + '"';
        check(name.c_str(), !unpacks(blobOf({{1u, path, data}}), theirs / "unsafe" / "x"));
    }
    check("nothing escaped", !fs::exists(theirs / "unsafe" / "escape") && !fs::exists(theirs / "escape") &&
        !fs::exists(root / "escape"));
    check("device-like names are fine", unpacks(blobOf({{1u, "UDATA/CONSOLE", data}, {1u, "UDATA/COM10", data}}),
        theirs / "names"));

    std::error_code error;
    fs::remove_all(root, error);
    if (failures == 0) std::puts("netplay profile tests passed");
    return failures == 0 ? 0 : 1;
}
