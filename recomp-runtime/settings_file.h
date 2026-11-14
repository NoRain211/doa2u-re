/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_SETTINGS_FILE_H
#define DOA2U_RECOMP_SETTINGS_FILE_H

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

/* The settings files the launchers and the runtime share, read and written whole. */

/* One flat JSON object: each key with its value's source text, in order.
   Nested values are kept whole; these files only ever hold scalars. */
using RecompSettingsEntries = std::vector<std::pair<std::string, std::string>>;

bool recomp_settings_read(const std::filesystem::path &path, std::string *out);
bool recomp_settings_parse(const std::string &text, RecompSettingsEntries *entries);
/* A JSON string value's text, for the plain strings these files hold; false
   for any other value or an escaped string. */
bool recomp_settings_string(const std::string &value, std::string *out);
/* Written beside the file and renamed over it, so a failed save keeps the
   old file. */
bool recomp_settings_write(const std::filesystem::path &path, const std::string &content);

#endif
