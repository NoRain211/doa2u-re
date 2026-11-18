/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_NETPLAY_GUEST_H
#define DOA2U_RECOMP_NETPLAY_GUEST_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True while this boot runs a session, a replay or a SyncTest: guest RAM is
   hashed or replayed there, so host-shaped values must stay out of it. */
bool recomp_netplay_online(void);

/* Guest hooks a netplay session sets up (netplay_session.cpp). */

/* The names shown above each life bar, local_side's in yellow; null or empty draws none. */
void recomp_netplay_guest_set_fight_names(const char *side0, const char *side1, uint32_t local_side);
/* Each side's profile name (UTF-16, null-terminated), whose controller 1
   settings that side's port takes when the game sets up its default
   profile; null or empty keeps the defaults. */
void recomp_netplay_guest_set_side_profiles(const uint16_t *side0, const uint16_t *side1);
RecompFunction recomp_netplay_guest_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
