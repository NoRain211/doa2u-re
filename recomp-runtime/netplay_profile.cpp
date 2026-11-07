/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_profile.h"

#include "netplay.h"
#include "sha.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

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

std::u16string recomp_netplay_utf16(const std::string &name)
{
    std::u16string wide;
    for (std::size_t i = 0u; i < name.size();) {
        const unsigned char lead = static_cast<unsigned char>(name[i++]);
        const unsigned more = lead >= 0xf0u ? 3u : lead >= 0xe0u ? 2u : lead >= 0xc0u ? 1u : 0u;
        std::uint32_t code = more == 0u ? lead : lead & (0x7fu >> (more + 1u));
        for (unsigned k = 0u; k < more && i < name.size() && (name[i] & 0xc0) == 0x80; ++k)
            code = code << 6 | (name[i++] & 0x3fu);
        if (code >= 0x10000u) {
            wide += static_cast<char16_t>(0xd800u + ((code - 0x10000u) >> 10));
            code = 0xdc00u + (code & 0x3ffu);
        }
        wide += static_cast<char16_t>(code);
    }
    return wide;
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
            for (const auto &file : fs::directory_iterator(save.path(), error))
                changed = std::max(changed, fs::last_write_time(file.path(), error));
            if (latest.empty() || changed > newest) {
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

bool recomp_netplay_pack_profile(const std::filesystem::path &root, const std::string &profile, bool title,
    std::vector<std::uint8_t> &blob)
{
    namespace fs = std::filesystem;
    std::vector<std::uint8_t> body;
    const auto put = [&body](std::size_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) body.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    };
    const auto keep = [&](const std::string &name, bool folder) {
        if (!profile.empty() && (name == profile || name.rfind(profile + "/", 0u) == 0u)) return true;
        if (!title) return false;
        if (name == "TDATA" || name.rfind("TDATA/", 0u) == 0u) return true;
        const auto depth = std::count(name.begin(), name.end(), '/');
        return (name == "UDATA" || name.rfind("UDATA/", 0u) == 0u) && (depth < 2 || (depth == 2 && !folder));
    };
    std::error_code error;
    unsigned entries = 0u;
    if (fs::exists(root, error)) {
        for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            const fs::file_status status = it->symlink_status(error);
            const bool folder = fs::is_directory(status);
            if (error || (!folder && !fs::is_regular_file(status))) return false;
            const std::string name = it->path().lexically_relative(root).generic_string();
            if (!keep(name, folder)) continue;
            // The limits recomp_netplay_unpack_profile enforces, caught here at the sender.
            if (name.size() > 200u || ++entries > 512u) return false;
            body.push_back(folder ? 0u : 1u);
            put(name.size(), 2u);
            body.insert(body.end(), name.begin(), name.end());
            if (folder) continue;
            const std::uintmax_t size = fs::file_size(it->path(), error);
            if (error || size > RECOMP_NETPLAY_MAX_PROFILE) return false;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::ifstream file(it->path(), std::ios::binary);
            if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
            put(bytes.size(), 4u);
            body.insert(body.end(), bytes.begin(), bytes.end());
        }
    }
    if (error) return false;
    const std::string hash = doaxbv::sha256Hex(body);
    blob.assign(hash.begin(), hash.end());
    blob.insert(blob.end(), body.begin(), body.end());
    return true;
}

// A path from the host: relative, below UDATA or TDATA, plain names, no Windows device names.
static bool safeProfilePath(const std::string &name)
{
    std::size_t start = 0u;
    for (bool first = true;; first = false) {
        const std::size_t slash = name.find('/', start);
        const std::string part = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        // Windows drops a trailing dot, so "SaveMeta.xbx." would open as SaveMeta.xbx.
        if (part.back() == '.') return false;
        for (const char c : part)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
        if (first && part != "UDATA" && part != "TDATA") return false;
        std::string stem = part.substr(0u, part.find('.'));
        for (char &c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4u && (stem.rfind("COM", 0u) == 0u || stem.rfind("LPT", 0u) == 0u) &&
                std::isdigit(static_cast<unsigned char>(stem[3]))))
            return false;
        if (slash == std::string::npos) return true;
        start = slash + 1u;
    }
}

/* A save folder holds only what the game writes there. Its loader trusts the
   framing of ups.dat (save_blob_decrypt underflows below 0x18 bytes), so the
   size is pinned; SaveMeta.xbx must name the save exactly once, as the game
   takes the first Name= anywhere in the file. */
static bool profileEntryOk(const std::string &name, bool folder, const std::uint8_t *data, std::size_t size)
{
    const auto depth = std::count(name.begin(), name.end(), '/');
    if (name.rfind("UDATA/", 0u) != 0u || depth < 3) return true;
    if (depth > 3 || folder) return false;
    std::string file = name.substr(name.rfind('/') + 1u);
    for (char &c : file) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (file == "UPS.DAT") return size == 0x12148u;
    if (file == "SAVEIMAGE.XBX") return size <= 0x4000u;
    if (file != "SAVEMETA.XBX" || size < 2u || size > 0x1000u || size % 2u != 0u || data[0] != 0xffu ||
        data[1] != 0xfeu)
        return false;
    static const char key[] = "Name=";
    unsigned keys = 0u;
    for (std::size_t at = 2u; at + 10u <= size; at += 2u) {
        bool match = true;
        for (std::size_t i = 0u; i < 5u && match; ++i)
            match = data[at + 2u * i] == static_cast<std::uint8_t>(key[i]) && data[at + 2u * i + 1u] == 0u;
        keys += match ? 1u : 0u;
    }
    return keys == 1u;
}

bool recomp_netplay_unpack_profile(const std::uint8_t *blob, std::size_t size, const std::filesystem::path &root,
    bool joiner, std::vector<std::string> &names)
{
    namespace fs = std::filesystem;
    if (size < 64u) return false;
    const std::vector<std::uint8_t> body(blob + 64, blob + size);
    if (doaxbv::sha256Hex(body) != std::string(reinterpret_cast<const char *>(blob), 64u)) return false;
    std::size_t at = 0u;
    const auto get = [&](unsigned bytes, std::uint32_t &value) {
        if (body.size() - at < bytes) return false;
        value = 0u;
        for (unsigned i = 0; i < bytes; ++i) value |= static_cast<std::uint32_t>(body[at++]) << (8u * i);
        return true;
    };
    std::error_code error;
    fs::create_directories(root, error);
    std::string folder;
    for (unsigned entries = 0u; !error && at < body.size(); ++entries) {
        std::uint32_t kind, length, bytes;
        if (entries == 512u || !get(1u, kind) || kind > 1u || !get(2u, length) || length > 200u ||
            body.size() - at < length)
            return false;
        const std::string name(body.begin() + static_cast<std::ptrdiff_t>(at),
            body.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (!safeProfilePath(name)) return false;
        if (joiner) {
            // Files only inside the one save folder, so none can replace the host's title files.
            const auto depth = std::count(name.begin(), name.end(), '/');
            if (name.rfind("UDATA/", 0u) != 0u || depth < 2 || (kind == 1u && depth < 3)) return false;
            const std::string top = name.substr(0u, name.find('/', name.find('/', 6u) + 1u));
            if (folder.empty()) folder = top;
            if (top != folder) return false;
        }
        const fs::path path = root / fs::path(name);
        if (kind == 0u) {
            if (!profileEntryOk(name, true, nullptr, 0u)) return false;
            fs::create_directories(path, error);
            continue;
        }
        if (!get(4u, bytes) || body.size() - at < bytes) return false;
        if (!profileEntryOk(name, false, body.data() + at, bytes)) return false;
        fs::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char *>(body.data() + at), bytes)) return false;
        file.close();
        if (!file) return false;
        std::string file_name = path.filename().string();
        for (char &c : file_name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        // Windows opens any case of the name, so the filter reads every one.
        if (file_name == "SAVEMETA.XBX") names.push_back(recomp_netplay_profile_name(body.data() + at, bytes));
        at += bytes;
    }
    return !error;
}
