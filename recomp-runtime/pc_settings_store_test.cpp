// SPDX-License-Identifier: GPL-3.0-or-later
#include "pc_settings_store.h"
#include "combo_hud.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int fails = 0;

void expect(const char *what, bool ok)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL pc settings store: %s\n", what);
        ++fails;
    }
}

void write(const std::string &path, const std::string &text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string read(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

int main()
{
    const auto folder = std::filesystem::temp_directory_path() / "recomp_pc_settings_store_test";
    std::filesystem::create_directories(folder);
    const std::string path = (folder / "launcher.json").string();
    std::filesystem::remove(path);
    RecompPcSettingsStore store;
    store.primaryHeight = 1440;
    store.ultrawideShape = "3440:1440";
    std::uint8_t c[RECOMP_PC_SETTING_COUNT];

    // No file: the launchers' defaults (the display's height, 32x, SMAA on, 16:9, Everything).
    expect("missing file", !recomp_pc_settings_read(path, store, c));
    expect("defaults", c[RECOMP_PC_RESOLUTION] == 3u && c[RECOMP_PC_MSAA] == 5u && c[RECOMP_PC_SMAA] == 1u &&
        c[RECOMP_PC_SCREEN] == 1u && c[RECOMP_PC_RENDERER] == 0u && c[RECOMP_PC_INTRO] == 0u &&
        c[RECOMP_PC_UNLOCKS] == 0u && c[RECOMP_PC_CPU_COSTUMES] == 0u);

    // A launcher's file, with keys the page does not own.
    write(path, "{\"height\":2160,\"msaa\":8,\"smaa\":false,\"widescreen\":true,\"screen\":\"2560:1080\","
        "\"vulkan\":true,\"vrr\":false,\"skipIntro\":true,\"surround\":true,\"unlockAll\":false,"
        "\"freeCamera\":true,\"randomCpuCostumes\":true,\"checkUpdates\":true,"
        "\"game_source\":\"D:\\\\Games\\\\doa2.iso\",\"window\":{\"x\":1,\"y\":[2,3]}}");
    expect("read", recomp_pc_settings_read(path, store, c));
    expect("values", c[RECOMP_PC_RESOLUTION] == 4u && c[RECOMP_PC_MSAA] == 3u && c[RECOMP_PC_SMAA] == 0u &&
        c[RECOMP_PC_SCREEN] == 3u && c[RECOMP_PC_RENDERER] == 1u && c[RECOMP_PC_VRR] == 0u &&
        c[RECOMP_PC_AUDIO] == 1u && c[RECOMP_PC_INTRO] == 1u && c[RECOMP_PC_UNLOCKS] == 1u &&
        c[RECOMP_PC_CPU_COSTUMES] == 1u && c[RECOMP_PC_FREE_CAMERA] == 1u);

    // Saving with 21:9 still chosen keeps that shape and every other key.
    c[RECOMP_PC_MSAA] = 0u;
    expect("write", recomp_pc_settings_write(path, store, c));
    std::string saved = read(path);
    expect("keeps shape", saved.find("\"screen\":\"2560:1080\"") != std::string::npos);
    expect("keeps others", saved.find("\"game_source\":\"D:\\\\Games\\\\doa2.iso\"") != std::string::npos &&
        saved.find("\"checkUpdates\":true") != std::string::npos &&
        saved.find("\"window\":{\"x\":1,\"y\":[2,3]}") != std::string::npos);
    expect("msaa off", saved.find("\"msaa\":1") != std::string::npos);
    expect("no temporary file", !std::filesystem::exists(path + ".tmp"));
    std::uint8_t again[RECOMP_PC_SETTING_COUNT];
    expect("round trip", recomp_pc_settings_read(path, store, again) && again[RECOMP_PC_MSAA] == 0u &&
        again[RECOMP_PC_SCREEN] == 3u && again[RECOMP_PC_UNLOCKS] == 1u);

    // A life bar colour saves by name and reads back.
    c[RECOMP_PC_LIFE_BAR] = RECOMP_LIFE_BAR_BLUE;
    recomp_pc_settings_write(path, store, c);
    expect("life bar saved", read(path).find("\"lifeBarColour\":\"blue\"") != std::string::npos);
    expect("life bar read", recomp_pc_settings_read(path, store, again) && again[RECOMP_PC_LIFE_BAR] == RECOMP_LIFE_BAR_BLUE);

    // 32:9, then 21:9 again: the monitor's own shape.
    c[RECOMP_PC_SCREEN] = 4u;
    recomp_pc_settings_write(path, store, c);
    expect("32:9", read(path).find("\"screen\":\"32:9\"") != std::string::npos);
    c[RECOMP_PC_SCREEN] = 3u;
    recomp_pc_settings_write(path, store, c);
    expect("monitor shape", read(path).find("\"screen\":\"3440:1440\"") != std::string::npos);
    c[RECOMP_PC_SCREEN] = 0u;
    recomp_pc_settings_write(path, store, c);
    saved = read(path);
    expect("4:3", saved.find("\"screen\":\"4:3\"") != std::string::npos &&
        saved.find("\"widescreen\":false") != std::string::npos);

    // A file it cannot read is left alone.
    write(path, "{\"height\": 720, broken");
    expect("unreadable", !recomp_pc_settings_write(path, store, c) && read(path) == "{\"height\": 720, broken");
    // An older launcher's widescreen flag alone.
    write(path, "{\"widescreen\":false}");
    recomp_pc_settings_read(path, store, c);
    expect("old flag", c[RECOMP_PC_SCREEN] == 0u);
    write(path, "{}");
    expect("empty", recomp_pc_settings_read(path, store, c) && c[RECOMP_PC_SCREEN] == 1u);
    std::filesystem::remove_all(folder);
    return fails == 0 ? 0 : 1;
}
