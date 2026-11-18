/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_FREE_CAMERA_ADAPTER_H
#define DOA2U_RECOMP_FREE_CAMERA_ADAPTER_H

#include "runtime.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* With RECOMP_FREE_CAMERA=1, hooks the input samples and the fiber switches
   for the free camera and the scene freeze; returns false when it is off.
   Online (logged on to DOA ONLINE, or a synctest) removes the hooks. */
bool recomp_free_camera_install(bool online);
/* PC Settings: turns the free camera on or off while the game runs. */
void recomp_free_camera_set_enabled(bool on);
/* DOA ONLINE's logon state: online removes the hooks and gives the camera
   back; leaving restores them for the player's setting. */
void recomp_free_camera_set_online(bool online);
bool recomp_free_camera_enabled(void);
/* The camera build (0x000399D0); it checks the setting and the online state. */
RecompFunction recomp_free_camera_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
