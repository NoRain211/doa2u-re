/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_FREE_CAMERA_ADAPTER_H
#define DOA2U_RECOMP_FREE_CAMERA_ADAPTER_H

#include "runtime.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* With RECOMP_FREE_CAMERA=1, hooks the input samples and the fiber switches
   for the free camera and the scene freeze; returns false (and installs
   nothing) when it is off. */
bool recomp_free_camera_install(void);
/* The camera build (0x000399D0) while the free camera is allowed. */
RecompFunction recomp_free_camera_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
