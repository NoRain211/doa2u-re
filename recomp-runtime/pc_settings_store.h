/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_PC_SETTINGS_STORE_H
#define DOA2U_RECOMP_PC_SETTINGS_STORE_H

#include "pc_settings.h"

#include <cstdint>
#include <string>

/* The PC Settings page's choices in the launchers' private/launcher.json,
   read with the launchers' defaults and written back with every key the page
   does not own kept as it was. primaryHeight picks the default resolution
   as the launchers do; ultrawideShape is what choosing Ultrawide saves. */
struct RecompPcSettingsStore {
    int primaryHeight = 1080;
    std::string ultrawideShape = "64:27";
};

/* Missing or unreadable keys take the launchers' defaults; false when the
   file could not be read at all. */
bool recomp_pc_settings_read(const std::string &path, const RecompPcSettingsStore &store,
    std::uint8_t choices[RECOMP_PC_SETTING_COUNT]);
bool recomp_pc_settings_write(const std::string &path, const RecompPcSettingsStore &store,
    const std::uint8_t choices[RECOMP_PC_SETTING_COUNT]);
/* launcher.json's lifeBarCustom, an RRGGBB hex colour ("#" optional), as
   0xRRGGBB; white when missing or not a colour. Only the file sets it. */
std::uint32_t recomp_pc_settings_custom_life_bar(const std::string &path);

#endif
