/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_PC_SETTINGS_H
#define DOA2U_RECOMP_PC_SETTINGS_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PC SETTINGS: a row the runner adds to the game's Options list, after Video,
   and the page it opens, drawn by the game's own option pages. The rows are
   the launcher's settings; their choices are indexes into these lists. */
enum RecompPcSetting {
    RECOMP_PC_RESOLUTION,    /* 480p, 720p, 1080p, 1440p, 4K */
    RECOMP_PC_MSAA,          /* Off, 2x, 4x, 8x, 16x, 32x */
    RECOMP_PC_SMAA,          /* Off, On */
    RECOMP_PC_SCREEN,        /* 4:3, 16:9, 16:10, Ultrawide, 32:9 */
    RECOMP_PC_RENDERER,      /* Direct3D 11, Vulkan */
    RECOMP_PC_VRR,           /* Off, On */
    RECOMP_PC_AUDIO,         /* Stereo, 5.1 Surround */
    RECOMP_PC_INTRO,         /* Play, Skip */
    RECOMP_PC_UNLOCKS,       /* Everything, Earn */
    RECOMP_PC_CPU_COSTUMES,  /* Default, Random */
    RECOMP_PC_FREE_CAMERA,   /* Off, On */
    RECOMP_PC_SETTING_COUNT
};

/* The launcher's values behind the Resolution, MSAA and Screen choices, in
   the rows' order. Ultrawide (NULL) saves the monitor's own shape. */
static const uint16_t RECOMP_PC_HEIGHTS[] = {480u, 720u, 1080u, 1440u, 2160u};
static const uint8_t RECOMP_PC_SAMPLES[] = {1u, 2u, 4u, 8u, 16u, 32u};
static const char *const RECOMP_PC_SCREENS[] = {"4:3", "16:9", "16:10", NULL, "32:9"};
enum { RECOMP_PC_ULTRAWIDE = 3 };

/* The number of choices a setting has. */
unsigned recomp_pc_settings_choices(unsigned setting);
/* True when a change takes effect at once; the others apply at the next launch.
   Unlocks, CPU costumes and the free camera do unless the host turns them off;
   the host marks others its renderer applies live, after recomp_pc_settings_install. */
bool recomp_pc_settings_applies_now(unsigned setting);
void recomp_pc_settings_set_live(unsigned setting, bool live);

typedef struct RecompPcSettingsHost {
    /* A choice the player changed; the host applies it if it applies now. */
    void (*changed)(unsigned setting, unsigned choice, void *context);
    /* The page closed: the host saves the choices. */
    void (*closed)(void *context);
    /* REBIND, the Controller page's row, opens the game's Button Config
       screen on the keyboard's and host pads' bindings (input_host_win32.h):
       what presses a binding (NULL for nothing), listening for a new key
       (pad false) or pad button, giving them all their defaults, and the
       screen closing, when the host saves. rebind.h has these. */
    const char *(*bound)(unsigned binding, bool pad);
    void (*listen)(unsigned binding, bool pad);
    bool (*listening)(void);
    void (*reset)(bool pad);
    void (*rebind_closed)(void);
    void *context;
} RecompPcSettingsHost;

/* Call at each boot, after the XBE is loaded and before the game runs: puts
   the page, its text and the current choices in the runner's guest asset
   area. */
void recomp_pc_settings_install(const RecompPcSettingsHost *host,
    const uint8_t choices[RECOMP_PC_SETTING_COUNT]);
uint8_t recomp_pc_settings_choice(unsigned setting);
RecompFunction recomp_pc_settings_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
