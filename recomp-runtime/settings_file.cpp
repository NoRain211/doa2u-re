/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "settings_file.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct Parser {
    const std::string &text;
    std::size_t at = 0u;

    void space()
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    }
    bool string(std::string *out)
    {
        if (at >= text.size() || text[at] != '"') return false;
        const std::size_t start = at++;
        while (at < text.size() && text[at] != '"') at += text[at] == '\\' ? 2u : 1u;
        if (at >= text.size()) return false;
        ++at;
        if (out != nullptr) *out = text.substr(start, at - start);
        return true;
    }
    /* A value's source text: a string, a nested object or array, or a scalar. */
    bool value(std::string *out)
    {
        const std::size_t start = at;
        if (at < text.size() && text[at] == '"') return string(out);
        int depth = 0;
        while (at < text.size()) {
            const char c = text[at];
            if (c == '"') {
                if (!string(nullptr)) return false;
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') {
                if (depth == 0) break;
                --depth;
            } else if (c == ',' && depth == 0) break;
            ++at;
        }
        std::size_t end = at;
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1u]))) --end;
        *out = text.substr(start, end - start);
        return end > start;
    }
};

} // namespace

bool recomp_settings_read(const std::filesystem::path &path, std::string *out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    *out = s.str();
    return true;
}

bool recomp_settings_parse(const std::string &text, RecompSettingsEntries *entries)
{
    Parser p{text};
    p.space();
    if (p.at >= text.size() || text[p.at] != '{') return false;
    ++p.at;
    p.space();
    if (p.at < text.size() && text[p.at] == '}') return true;
    for (;;) {
        std::string key, value;
        p.space();
        if (!p.string(&key)) return false;
        p.space();
        if (p.at >= text.size() || text[p.at] != ':') return false;
        ++p.at;
        p.space();
        if (!p.value(&value)) return false;
        entries->emplace_back(key.substr(1u, key.size() - 2u), value);
        p.space();
        if (p.at < text.size() && text[p.at] == ',') {
            ++p.at;
            continue;
        }
        return p.at < text.size() && text[p.at] == '}';
    }
}

bool recomp_settings_string(const std::string &value, std::string *out)
{
    if (value.size() < 2u || value.front() != '"' || value.back() != '"' ||
        value.find('\\') != std::string::npos) return false;
    *out = value.substr(1u, value.size() - 2u);
    return true;
}

bool recomp_settings_write(const std::filesystem::path &path, const std::string &content)
{
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary, error); // a link left there is removed, not written through
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.close();
        if (!file) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
#else
    std::filesystem::rename(temporary, path, error);
    if (!error) return true;
#endif
    std::filesystem::remove(temporary, error);
    return false;
}
