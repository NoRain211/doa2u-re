/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_DSOUND_EFFECTS_ADAPTER_H
#define DOA2U_RECOMP_DSOUND_EFFECTS_ADAPTER_H
#include "runtime.h"
RecompFunction recomp_dsound_effects_lookup_manual(uint32_t address);
void recomp_dsound_effects_reset(void);
uint32_t recomp_dsound_effects_headroom(uint32_t bin);
/* Listener position then velocity, as last set (committed or deferred). */
void recomp_dsound_listener_vectors(float vectors[6]);
#endif
