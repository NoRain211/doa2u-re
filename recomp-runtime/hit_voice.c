/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hit_voice.h"
#include "audio_output.h"
#include "kernel_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* apply_pending_life (0x119420) runs once a frame in a fight. For each side
   s, the block at 0x87ae54 + 0x94 s holds the 16-bit life, then at +2 the
   damage that landed since the last frame, which it subtracts (Sparring and
   Training keep the life) and clears. The side's character id is at
   0x87ae31 + 0x94 s; 0xd6ad0 builds a character's file names from the
   three-letter prefix at 0x694210[0x69425c[id]], the same prefixes DOA4 uses
   for its voice banks, except Ein (kte here, ein there) and Helena (hik,
   hel). Leon (24, sam), Tengu (17) and Hitomi (21) have no prefix in DOA2U;
   17 and 21 follow from the save order at 0x6959dc, where Hitomi comes last.

   Each side plays on its own host output slot. A hit grunts unless the
   side's last grunt started under GAP ms ago at the same or a heavier level;
   a KO always cries and the cry is not cut by later hits. */
enum {
    APPLY_LIFE = 0x00119420u,
    SIDE_LIFE = 0x0087ae54u,
    SIDE_CHARACTER = 0x0087ae31u,
    SIDE_STRIDE = 0x94u,

    RATE = 48000u,
    LEAD = RATE / 12u,     /* ~83 ms queued ahead; the output drops past 250 */
    GAP_MS = 220u,
    CLIPS = 32u,
    CHARACTERS = 25u,
    VOLUME = -200,         /* hundredths of a dB */
};

extern void sub_00119420(void);

static const char *const PREFIX[CHARACTERS] = {
    [0] = "zac", [1] = "tin", [2] = "jan", [3] = "ein", [4] = "ryu", [5] = "kas", [6] = "gen",
    [7] = "hel", [8] = "bay", [9] = "bas", [12] = "lei", [13] = "aya", [17] = "tng",
    [21] = "hak", [24] = "leo",
};

typedef struct Clip {
    int16_t *pcm;
    uint32_t frames;
} Clip;

typedef struct Bank {
    bool loaded;
    unsigned hits, kos;
    Clip hit[CLIPS], ko[CLIPS];  /* each sorted shortest first */
} Bank;

typedef struct Voice {
    const Clip *clip;
    uint32_t sent;
    double start;
    unsigned level;  /* 6 for a KO cry */
    int last_hit, last_ko;
} Voice;

static Bank banks[CHARACTERS];
static Voice voices[2];
static char root[1024];
static bool enabled = true, online_session;
static uint32_t seed = 1u;
static bool reported[CHARACTERS];

const char *recomp_hit_voice_prefix(uint8_t character)
{
    return character < CHARACTERS ? PREFIX[character] : NULL;
}

unsigned recomp_hit_voice_level(uint32_t damage)
{
    return damage < 12u ? 1u : damage < 22u ? 2u : damage < 35u ? 3u : damage < 50u ? 4u : 5u;
}

unsigned recomp_hit_voice_pick(unsigned count, unsigned level, uint32_t roll, int last)
{
    if (count <= 1u) return 0u;
    if (level < 1u) level = 1u;
    if (level > 5u) level = 5u;
    /* Level 1 spans the shortest half of the pool, level 5 the longest. */
    const unsigned width = (count + 1u) / 2u;
    const unsigned first = (count - width) * (level - 1u) / 4u;
    unsigned pick = first + roll % width;
    if ((int)pick == last && width > 1u) pick = first + (pick - first + 1u + (roll >> 8) % (width - 1u)) % width;
    return pick;
}

static uint32_t next_random(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static double now_ms(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

/* A 16-bit mono 48 kHz WAV, or false. */
static bool load_clip(const char *path, Clip *clip)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t header[12];
    bool ok = fread(header, 1, 12, file) == 12 && memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0;
    bool format = false;
    while (ok) {
        uint8_t chunk[8];
        if (fread(chunk, 1, 8, file) != 8) { ok = false; break; }
        const uint32_t size = le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16u) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, file) != 16) { ok = false; break; }
            format = fmt[0] == 1 && fmt[1] == 0 && fmt[2] == 1 && fmt[3] == 0 && le32(fmt + 4) == RATE &&
                fmt[14] == 16 && fmt[15] == 0;
            if (fseek(file, (long)(size - 16u + (size & 1u)), SEEK_CUR) != 0) ok = false;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!format || size < 2u) { ok = false; break; }
            clip->frames = size / 2u;
            clip->pcm = malloc((size_t)clip->frames * 2u);
            ok = clip->pcm != NULL && fread(clip->pcm, 2, clip->frames, file) == clip->frames;
            /* WAV is little-endian, as every host this runs on. */
            break;
        } else if (fseek(file, (long)(size + (size & 1u)), SEEK_CUR) != 0) {
            ok = false;
        }
    }
    fclose(file);
    if (!ok) {
        free(clip->pcm);
        clip->pcm = NULL;
        clip->frames = 0u;
    }
    return ok;
}

static int by_length(const void *a, const void *b)
{
    const uint32_t x = ((const Clip *)a)->frames, y = ((const Clip *)b)->frames;
    return x < y ? -1 : x > y;
}

static unsigned load_pool(const char *prefix, const char *kind, Clip *pool)
{
    unsigned count = 0u;
    for (unsigned i = 0u; i < CLIPS; ++i) {
        char path[1100];
        snprintf(path, sizeof path, "%s/%s/%s_%02u.wav", root, prefix, kind, i);
        if (!load_clip(path, &pool[count])) break;
        ++count;
    }
    qsort(pool, count, sizeof *pool, by_length);
    return count;
}

static Bank *bank_for(uint8_t character)
{
    const char *prefix = recomp_hit_voice_prefix(character);
    if (prefix == NULL || root[0] == '\0') return NULL;
    Bank *bank = &banks[character];
    if (!bank->loaded) {
        bank->loaded = true;
        bank->hits = load_pool(prefix, "hit", bank->hit);
        bank->kos = load_pool(prefix, "ko", bank->ko);
    }
    if (!reported[character]) {
        reported[character] = true;
        fprintf(stderr, "recomp hit voices: character %u is %s, %u hit and %u KO clips\n",
            character, prefix, bank->hits, bank->kos);
    }
    return bank->hits + bank->kos > 0u ? bank : NULL;
}

static void stop(unsigned side)
{
    if (voices[side].clip != NULL) recomp_audio_output_reset_voice(RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT + side);
    voices[side].clip = NULL;
}

void recomp_hit_voice_pump(void)
{
    const double now = now_ms();
    for (unsigned side = 0u; side < 2u; ++side) {
        Voice *voice = &voices[side];
        if (voice->clip == NULL) continue;
        const double played = (now - voice->start) * (RATE / 1000.0);
        uint32_t target = played <= 0.0 ? LEAD : (uint32_t)played + LEAD;
        if (target > voice->clip->frames) target = voice->clip->frames;
        if (target > voice->sent) {
            recomp_audio_output_submit(RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT + side,
                (const uint8_t *)(voice->clip->pcm + voice->sent), (target - voice->sent) * 2u, RATE, 1u, 16u, VOLUME);
            voice->sent = target;
        }
        /* Let the queued tail play out; the slot is reset by the next clip. */
        if (voice->sent == voice->clip->frames && played > voice->clip->frames + RATE / 4u) voice->clip = NULL;
    }
}

static void play(unsigned side, const Clip *clip, unsigned level)
{
    stop(side);
    voices[side].clip = clip;
    voices[side].sent = 0u;
    voices[side].start = now_ms();
    voices[side].level = level;
}

static void voice_hit(unsigned side, uint32_t damage, bool ko)
{
    Bank *bank = bank_for(*recomp_memory(SIDE_CHARACTER + side * SIDE_STRIDE, 1u));
    if (bank == NULL) return;
    Voice *voice = &voices[side];
    const double age = now_ms() - voice->start;
    if (ko) {
        if (voice->clip != NULL && voice->level == 6u) return;
        /* No KO clips: the longest grunt. */
        if (bank->kos == 0u) {
            play(side, &bank->hit[bank->hits - 1u], 6u);
            return;
        }
        const unsigned pick = recomp_hit_voice_pick(bank->kos, 3u, next_random(), voice->last_ko);
        voice->last_ko = (int)pick;
        play(side, &bank->ko[pick], 6u);
        return;
    }
    if (bank->hits == 0u) return;
    const unsigned level = recomp_hit_voice_level(damage);
    if (voice->clip != NULL && (voice->level == 6u || (age < GAP_MS && level <= voice->level))) return;
    const unsigned pick = recomp_hit_voice_pick(bank->hits, level, next_random(), voice->last_hit);
    voice->last_hit = (int)pick;
    play(side, &bank->hit[pick], level);
}

static void apply_life(void)
{
    uint16_t life[2], pending[2];
    for (unsigned side = 0u; side < 2u; ++side) {
        life[side] = *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE);
        pending[side] = *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE + 2u);
    }
    sub_00119420();
    if (!enabled || online_session) return;
    for (unsigned side = 0u; side < 2u; ++side) {
        if (pending[side] == 0u) continue;
        const uint16_t after = *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE);
        voice_hit(side, pending[side], life[side] > 0u && after == 0u);
    }
    recomp_hit_voice_pump();
}

void recomp_hit_voice_set_enabled(bool on)
{
    enabled = on;
    if (!on) {
        stop(0u);
        stop(1u);
    }
}

void recomp_hit_voice_install(bool online, const char *directory)
{
    online_session = online;
    snprintf(root, sizeof root, "%s", directory != NULL ? directory : "");
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    seed = ((uint32_t)t.tv_sec * 2654435761u ^ (uint32_t)t.tv_nsec) | 1u;
    for (unsigned side = 0u; side < 2u; ++side) {
        voices[side].clip = NULL;
        voices[side].last_hit = voices[side].last_ko = -1;
    }
}

RecompFunction recomp_hit_voice_lookup_manual(uint32_t guest_address)
{
    return guest_address == APPLY_LIFE ? apply_life : NULL;
}
