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
   Online (a session, synctest or replay) removes the hooks for that boot. */
bool recomp_free_camera_install(bool online);
/* PC Settings: turns the free camera on or off while the game runs (not online). */
void recomp_free_camera_set_enabled(bool on);
bool recomp_free_camera_enabled(void);
/* The camera build (0x000399D0) for every offline boot; it checks the setting. */
RecompFunction recomp_free_camera_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
