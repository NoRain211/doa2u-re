/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_HIT_VOICE_H
#define DOA2U_RECOMP_HIT_VOICE_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hit grunts and KO cries for the fighters, which DOA2U leaves silent. The
   clips are the player's own, decoded from their copy of Dead or Alive 4 into
   <directory>/<prefix>/hit_NN.wav and ko_NN.wav (16-bit mono PCM), and are
   never part of the repository. A character without clips stays silent.

   Call install at each boot; online sessions keep the fighters silent, since
   a rollback replays the frames that land hits. */
void recomp_hit_voice_install(bool online, const char *directory);
void recomp_hit_voice_set_enabled(bool enabled);
/* Keeps a playing clip fed; called by the hooks every frame. */
void recomp_hit_voice_pump(void);
RecompFunction recomp_hit_voice_lookup_manual(uint32_t guest_address);

/* The DOA4 voice prefix of a DOA2U character id (0x87ae31), or NULL. */
const char *recomp_hit_voice_prefix(uint8_t character);
/* Damage level 1..5 for a hit's damage. */
unsigned recomp_hit_voice_level(uint32_t damage);
/* The clip for a level from a pool of `count` clips sorted shortest first:
   light hits draw from the short end, heavy ones from the long end. `roll`
   picks within the window; `last` (or -1) is avoided when the window has
   another clip. */
unsigned recomp_hit_voice_pick(unsigned count, unsigned level, uint32_t roll, int last);

#ifdef __cplusplus
}
#endif

#endif
