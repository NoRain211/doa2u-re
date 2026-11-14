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
   clips are the player's own, chosen from their copy of Dead or Alive 4 and
   placed in <directory>/<prefix>/selections/ (16-bit mono 48 kHz WAV; the
   VoiceEditor tool converts and places them); they are never part of the
   repository. A file's name picks its use, whatever comes before it:
   ..._HIT_LV1_NN.wav, _HIT_LV2_ and _HIT_LV3_ for light, medium and heavy
   hits, ..._KO_NN.wav for the KO. voice.ini beside them tunes the fighter
   (RecompHitVoiceTuning's keys). A character without a selections folder
   stays silent. A changed folder is picked up within a second, mid-game.

   Call install at each boot; online sessions keep the fighters silent, since
   a rollback replays the frames that land hits. */
void recomp_hit_voice_install(bool online, const char *directory);
/* The PC Settings row: 0 Off, 1 Quiet, 2 Normal, 3 Loud. */
void recomp_hit_voice_set_choice(unsigned choice);
/* Keeps a playing clip fed; called by the hooks every frame. */
void recomp_hit_voice_pump(void);
RecompFunction recomp_hit_voice_lookup_manual(uint32_t guest_address);
/* Once a presented frame: cries as the KO replay shows the KO, and keeps
   the clips fed. */
void recomp_hit_voice_frame(void);

/* A fighter's tuning, voice.ini's keys in brackets. */
typedef struct RecompHitVoiceTuning {
    uint32_t medium_from;   /* [medium_from] base damage of a medium hit (22) */
    uint32_t heavy_from;    /* [heavy_from] base damage of a heavy hit (35) */
    uint32_t meter_low;     /* [meter_low], [meter_high]: base damage taken */
    uint32_t meter_high;    /*   between grunts, drawn in this range (28..46) */
    uint32_t gap_ms;        /* [gap_ms] least time between grunts (700) */
    bool heavy_always;      /* [heavy_always] a heavy hit always grunts (1) */
    int32_t volume;         /* [volume_db] added to the row's volume, in 1/100 dB (0) */
} RecompHitVoiceTuning;

void recomp_hit_voice_default_tuning(RecompHitVoiceTuning *tuning);
/* Applies voice.ini text over `tuning`; unknown keys and bad values are skipped. */
void recomp_hit_voice_parse_tuning(const char *text, RecompHitVoiceTuning *tuning);

/* The DOA4 voice prefix of a DOA2U character id (0x87ae31), or NULL. */
const char *recomp_hit_voice_prefix(uint8_t character);
/* Hit level 1..3 (light, medium, heavy) for a hit's base damage. */
unsigned recomp_hit_voice_level(const RecompHitVoiceTuning *tuning, uint32_t base);
/* The pain meter: base damage builds it, a grunt empties it. `meter` is the
   base damage taken since the side last grunted, `threshold` the amount
   that brings the next grunt (drawn after each grunt) and `since_grunt` ms
   since that grunt began. */
bool recomp_hit_voice_should_grunt(const RecompHitVoiceTuning *tuning, uint32_t meter, uint32_t threshold,
    unsigned level, double since_grunt);
/* A clip from a pool of `count`, avoiding `last` (or -1) when it can. */
unsigned recomp_hit_voice_pick(unsigned count, uint32_t roll, int last);
/* The use a selections file's name gives it: 1..3 a hit level, 4 the KO, 0 none. */
unsigned recomp_hit_voice_kind(const char *name);
/* Tests: moves the voices' clock on by `ms`. */
void recomp_hit_voice_advance_clock(double ms);

#ifdef __cplusplus
}
#endif

#endif
