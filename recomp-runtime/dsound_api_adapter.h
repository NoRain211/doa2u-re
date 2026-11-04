/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_DSOUND_API_ADAPTER_H
#define DOA2U_RECOMP_DSOUND_API_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
RecompFunction recomp_dsound_api_lookup_manual(uint32_t address);
uint32_t recomp_dsound_current_device(void);
void recomp_dsound_require_device(uint32_t handle);
void recomp_dsound_device_add_owner(void);
void recomp_dsound_device_release_owner(void);
void recomp_dsound_api_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
