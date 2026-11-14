/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hit_voice.h"
#include "audio_output.h"
#include "kernel_abi.h"
#include "ko_replay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

/* apply_pending_life (0x119420) runs once a frame in a fight. For each side
   s, the block at 0x87ae54 + 0x94 s holds the 16-bit life, then at +2 the
   damage that landed since the last frame, which it subtracts (Sparring and
   Training keep the life) and clears. The side's character id is at
   0x87ae31 + 0x94 s; 0xd6ad0 builds a character's file names from the
   three-letter prefix at 0x694210[0x69425c[id]], the same prefixes DOA4 uses
   for its voice banks, except Ein (kte here, ein there) and Helena (hik,
   hel). Leon (24, sam), Tengu (17) and Hitomi (21) have no prefix in DOA2U;
   17 and 21 follow from the save order at 0x6959dc, where Hitomi comes last.
   DOA4 calls Hitomi kor (hak is Kokoro, hyt Hayate).

   A hit's damage is scaled before it lands: 0x1159f0 adds to side s's
   pending damage 0x10ce60(s), the move's base damage (a byte), times
   0x1130d0's scale for combos and juggles. The hook on 0x10ce60 keeps the
   base for the frame's 0x119420, so a heavy move grunts as heavy however
   little it takes off. The stage's hazards (0x107170) use 0x10ce60 too.

   Each side plays on its own host output slot. Grunts follow the base damage
   taken: each hit fills a meter, and a grunt plays when it passes a
   threshold drawn anew after each grunt (so a combo grunts once or twice,
   not on a fixed hit); a heavy hit always grunts; grunts are at least gap_ms
   apart. The meter starts partly full after HIT_MEMORY_MS without hits. The
   KO always cries, cutting any grunt, and nothing cuts the cry.

   DOA2U's KO replay plays back recorded snapshots and lands no damage, so
   recomp_hit_voice_frame cries as it shows the KO (see there). */
enum {
    APPLY_LIFE = 0x00119420u,
    BASE_DAMAGE = 0x0010ce60u,
    SIDE_LIFE = 0x0087ae54u,
    SIDE_CHARACTER = 0x0087ae31u,
    SIDE_STRIDE = 0x94u,

    RATE = 48000u,
    LEAD = RATE / 12u,     /* ~83 ms queued ahead; the output drops past 250 */
    CHUNK = RATE / 100u,   /* 10 ms at least per submission: the output holds few buffers */
    HIT_MEMORY_MS = 2500u,
    RECHECK_MS = 1000u,
    CLIPS = 16u,
    LEVELS = 3u,
    CHARACTERS = 25u,
};

/* Hundredths of a dB for Off, Quiet, Normal and Loud. The clips peak near
   full scale, well above the game's own voices. */
static const int32_t VOLUMES[4] = {0, -1800, -1200, -600};

extern void sub_00119420(void);
extern void sub_0010CE60(void);

static const char *const PREFIX[CHARACTERS] = {
    [0] = "zac", [1] = "tin", [2] = "jan", [3] = "ein", [4] = "ryu", [5] = "kas", [6] = "gen",
    [7] = "hel", [8] = "bay", [9] = "bas", [12] = "lei", [13] = "aya", [17] = "tng",
    [21] = "kor", [24] = "leo",
};

typedef struct Clip {
    int16_t *pcm;
    uint32_t frames;
} Clip;

typedef struct Bank {
    bool loaded;
    double checked;          /* when the folder was last looked at */
    long long stamp;         /* the folder's and voice.ini's times */
    RecompHitVoiceTuning tuning;
    unsigned hits[LEVELS + 1u], kos;
    Clip hit[LEVELS + 1u][CLIPS], ko[CLIPS];
} Bank;

typedef struct Voice {
    const Clip *clip;
    const Bank *bank;
    uint32_t sent;
    double start, last_grunt, last_hit_time;
    uint32_t meter, threshold;
    bool crying;
    int last_hit[LEVELS + 1u], last_ko;
} Voice;

static Bank banks[CHARACTERS];
static Voice voices[2];
static char root[1024];
static unsigned choice = 2u;
static bool online_session;
static uint32_t seed = 1u;
static uint32_t base_damage[2];  /* this frame's, from 0x10ce60 */

void recomp_hit_voice_default_tuning(RecompHitVoiceTuning *tuning)
{
    tuning->medium_from = 22u;
    tuning->heavy_from = 35u;
    tuning->meter_low = 28u;
    tuning->meter_high = 46u;
    tuning->gap_ms = 700u;
    tuning->heavy_always = true;
    tuning->volume = 0;
}

void recomp_hit_voice_parse_tuning(const char *text, RecompHitVoiceTuning *tuning)
{
    while (*text != '\0') {
        char key[32] = {0};
        size_t k = 0u;
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
        while (*text != '\0' && *text != '=' && *text != '\n' && k + 1u < sizeof key)
            if (*text != ' ' && *text != '\t') key[k++] = (char)tolower((unsigned char)*text++);
            else ++text;
        if (*text != '=') {
            while (*text != '\0' && *text != '\n') ++text;
            continue;
        }
        char *end = NULL;
        const double value = strtod(text + 1, &end);
        const bool ok = end != text + 1;
        text = end != NULL && end > text ? end : text + 1;
        while (*text != '\0' && *text != '\n') ++text;
        if (!ok) continue;
        const uint32_t whole = value < 0.0 ? 0u : value > 100000.0 ? 100000u : (uint32_t)(value + 0.5);
        if (strcmp(key, "medium_from") == 0) tuning->medium_from = whole;
        else if (strcmp(key, "heavy_from") == 0) tuning->heavy_from = whole;
        else if (strcmp(key, "meter_low") == 0) tuning->meter_low = whole;
        else if (strcmp(key, "meter_high") == 0) tuning->meter_high = whole;
        else if (strcmp(key, "gap_ms") == 0) tuning->gap_ms = whole;
        else if (strcmp(key, "heavy_always") == 0) tuning->heavy_always = value != 0.0;
        else if (strcmp(key, "volume_db") == 0 && value >= -40.0 && value <= 20.0)
            tuning->volume = (int32_t)(value * 100.0 + (value < 0.0 ? -0.5 : 0.5));
    }
    if (tuning->meter_high < tuning->meter_low) tuning->meter_high = tuning->meter_low;
    if (tuning->heavy_from < tuning->medium_from) tuning->heavy_from = tuning->medium_from;
}

const char *recomp_hit_voice_prefix(uint8_t character)
{
    return character < CHARACTERS ? PREFIX[character] : NULL;
}

unsigned recomp_hit_voice_level(const RecompHitVoiceTuning *tuning, uint32_t base)
{
    return base < tuning->medium_from ? 1u : base < tuning->heavy_from ? 2u : 3u;
}

bool recomp_hit_voice_should_grunt(const RecompHitVoiceTuning *tuning, uint32_t meter, uint32_t threshold,
    unsigned level, double since_grunt)
{
    if (level < 1u || level > LEVELS || since_grunt < tuning->gap_ms) return false;
    return (level == 3u && tuning->heavy_always) || meter >= threshold;
}

unsigned recomp_hit_voice_pick(unsigned count, uint32_t roll, int last)
{
    if (count <= 1u) return 0u;
    unsigned pick = roll % count;
    if ((int)pick == last) pick = (pick + 1u + (roll >> 8) % (count - 1u)) % count;
    return pick;
}

/* Case-blind search for `word` (in capitals) in `name`. */
static const char *find(const char *name, const char *word)
{
    const size_t n = strlen(word);
    for (; *name != '\0'; ++name) {
        size_t i = 0u;
        while (i < n && name[i] != '\0' && toupper((unsigned char)name[i]) == word[i]) ++i;
        if (i == n) return name;
    }
    return NULL;
}

unsigned recomp_hit_voice_kind(const char *name)
{
    const size_t length = strlen(name);
    if (length < 4u || find(name + length - 4u, ".WAV") == NULL) return 0u;
    const char *hit = find(name, "HIT_LV");
    if (hit != NULL) return hit[6] >= '1' && hit[6] <= '3' ? (unsigned)(hit[6] - '0') : 0u;
    /* KO as its own word: at the start or after a separator. */
    for (const char *ko = find(name, "KO"); ko != NULL; ko = find(ko + 1, "KO"))
        if ((ko == name || ko[-1] == '_' || ko[-1] == '-' || ko[-1] == ' ') &&
            (ko[2] == '_' || ko[2] == '-' || ko[2] == '.' || ko[2] == ' '))
            return 4u;
    return 0u;
}

static uint32_t next_random(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static double clock_offset;

void recomp_hit_voice_advance_clock(double ms) { clock_offset += ms; }

static double now_ms(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6 + clock_offset;
}

static uint32_t draw_threshold(const RecompHitVoiceTuning *tuning)
{
    return tuning->meter_low + next_random() % (tuning->meter_high - tuning->meter_low + 1u);
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
    clip->pcm = NULL;
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

static void add_file(Bank *bank, const char *folder, const char *name)
{
    const unsigned kind = recomp_hit_voice_kind(name);
    if (kind == 0u) return;
    unsigned *count = kind == 4u ? &bank->kos : &bank->hits[kind];
    Clip *pool = kind == 4u ? bank->ko : bank->hit[kind];
    if (*count >= CLIPS) return;
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", folder, name);
    if (load_clip(path, &pool[*count])) ++*count;
    else fprintf(stderr, "recomp hit voices: %s is not a 16-bit mono 48 kHz WAV\n", path);
}

static long long modified(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (long long)info.st_mtime : -1;
}

static void folder_of(const char *prefix, char *folder, size_t size)
{
    snprintf(folder, size, "%s/%s/selections", root, prefix);
}

static long long bank_stamp(const char *prefix)
{
    char folder[1100], ini[1120];
    folder_of(prefix, folder, sizeof folder);
    snprintf(ini, sizeof ini, "%s/voice.ini", folder);
    return modified(folder) * 1000003LL + modified(ini);
}

static void free_bank(Bank *bank)
{
    for (unsigned side = 0u; side < 2u; ++side)
        if (voices[side].bank == bank) {
            if (voices[side].clip != NULL) recomp_audio_output_reset_voice(RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT + side);
            voices[side].clip = NULL;
            voices[side].bank = NULL;
            voices[side].crying = false;
        }
    for (unsigned level = 0u; level <= LEVELS; ++level)
        for (unsigned i = 0u; i < bank->hits[level]; ++i) free(bank->hit[level][i].pcm);
    for (unsigned i = 0u; i < bank->kos; ++i) free(bank->ko[i].pcm);
    memset(bank, 0, sizeof *bank);
}

static void load_bank(Bank *bank, const char *prefix, unsigned character)
{
    char folder[1100];
    folder_of(prefix, folder, sizeof folder);
    recomp_hit_voice_default_tuning(&bank->tuning);
    char ini[1120];
    snprintf(ini, sizeof ini, "%s/voice.ini", folder);
    FILE *file = fopen(ini, "rb");
    if (file != NULL) {
        char text[2048];
        const size_t got = fread(text, 1, sizeof text - 1u, file);
        text[got] = '\0';
        fclose(file);
        recomp_hit_voice_parse_tuning(text, &bank->tuning);
    }
#ifdef _WIN32
    char pattern[1110];
    snprintf(pattern, sizeof pattern, "%s/*", folder);
    struct _finddata_t entry;
    const intptr_t handle = _findfirst(pattern, &entry);
    if (handle != -1) {
        do {
            if ((entry.attrib & _A_SUBDIR) == 0) add_file(bank, folder, entry.name);
        } while (_findnext(handle, &entry) == 0);
        _findclose(handle);
    }
#else
    DIR *dir = opendir(folder);
    if (dir != NULL) {
        for (struct dirent *entry; (entry = readdir(dir)) != NULL;) add_file(bank, folder, entry->d_name);
        closedir(dir);
    }
#endif
    fprintf(stderr, "recomp hit voices: character %u is %s, %u/%u/%u hit and %u KO clips\n",
        character, prefix, bank->hits[1], bank->hits[2], bank->hits[3], bank->kos);
}

static Bank *bank_for(uint8_t character)
{
    const char *prefix = recomp_hit_voice_prefix(character);
    if (prefix == NULL || root[0] == '\0') return NULL;
    Bank *bank = &banks[character];
    const double now = now_ms();
    /* Reloaded when the editor changes the folder, a second at most later. */
    if (!bank->loaded || now - bank->checked >= RECHECK_MS) {
        const long long stamp = bank_stamp(prefix);
        if (!bank->loaded || stamp != bank->stamp) {
            free_bank(bank);
            load_bank(bank, prefix, character);
            bank->loaded = true;
            bank->stamp = stamp;
        }
        bank->checked = now;
    }
    return bank->hits[1] + bank->hits[2] + bank->hits[3] + bank->kos > 0u ? bank : NULL;
}

static void stop(unsigned side)
{
    if (voices[side].clip != NULL) recomp_audio_output_reset_voice(RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT + side);
    voices[side].clip = NULL;
    voices[side].crying = false;
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
        /* Pumped every frame, and unpaced frames come fast: send whole chunks. */
        if (target > voice->sent && (target - voice->sent >= CHUNK || target == voice->clip->frames)) {
            int32_t volume = VOLUMES[choice < 4u ? choice : 2u] + (voice->bank != NULL ? voice->bank->tuning.volume : 0);
            if (volume > 0) volume = 0;
            recomp_audio_output_submit(RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT + side,
                (const uint8_t *)(voice->clip->pcm + voice->sent), (target - voice->sent) * 2u, RATE, 1u, 16u, volume);
            voice->sent = target;
        }
        /* Let the queued tail play out; the slot is reset by the next clip. */
        if (voice->sent == voice->clip->frames && played > voice->clip->frames + RATE / 4u) {
            voice->clip = NULL;
            voice->crying = false;
        }
    }
}

static void play(unsigned side, const Bank *bank, const Clip *clip, bool cry)
{
    stop(side);
    Voice *voice = &voices[side];
    voice->clip = clip;
    voice->bank = bank;
    voice->sent = 0u;
    voice->start = now_ms();
    voice->crying = cry;
}

/* The nearest level to `level` with clips, preferring lighter ones. */
static unsigned level_with_clips(const Bank *bank, unsigned level)
{
    for (unsigned d = 0u; d < LEVELS; ++d) {
        if (level > d && bank->hits[level - d] > 0u) return level - d;
        if (level + d <= LEVELS && bank->hits[level + d] > 0u) return level + d;
    }
    return 0u;
}

static void cry(unsigned side, Bank *bank);
static void note_event(unsigned side, const Bank *bank, unsigned level, unsigned clip);

/* life: the side's life before the hit; after: once it landed; base: the
   hit's base damage (its landed damage when 0x10ce60 gave none). */
static void voice_hit(unsigned side, uint32_t base, uint16_t life, uint16_t after)
{
    Bank *bank = bank_for(*recomp_memory(SIDE_CHARACTER + side * SIDE_STRIDE, 1u));
    if (bank == NULL) return;
    const RecompHitVoiceTuning *tuning = &bank->tuning;
    Voice *voice = &voices[side];
    const double now = now_ms();
    if (voice->threshold < tuning->meter_low || voice->threshold > tuning->meter_high)
        voice->threshold = draw_threshold(tuning);
    if (now - voice->last_hit_time >= HIT_MEMORY_MS) {
        /* A fresh exchange: the meter starts partly full, so its first hit may grunt. */
        voice->meter = next_random() % (voice->threshold + 1u);
    }
    voice->last_hit_time = now;
    if (life == 0u || voice->crying) return; /* down already, or crying */
    if (after == 0u) {
        cry(side, bank);
        return;
    }
    voice->meter += base;
    const unsigned asked = recomp_hit_voice_level(tuning, base);
    if (!recomp_hit_voice_should_grunt(tuning, voice->meter, voice->threshold, asked, now - voice->last_grunt)) return;
    const unsigned level = level_with_clips(bank, asked);
    if (level == 0u) return;
    const unsigned pick = recomp_hit_voice_pick(bank->hits[level], next_random(), voice->last_hit[level]);
    voice->last_hit[level] = (int)pick;
    voice->last_grunt = now;
    voice->meter = 0u;
    voice->threshold = draw_threshold(tuning);
    play(side, bank, &bank->hit[level][pick], false);
    note_event(side, bank, level, pick);
}

static void cry(unsigned side, Bank *bank)
{
    Voice *voice = &voices[side];
    voice->meter = 0u;
    if (bank->kos == 0u) return;
    const unsigned pick = recomp_hit_voice_pick(bank->kos, next_random(), voice->last_ko);
    voice->last_ko = (int)pick;
    play(side, bank, &bank->ko[pick], true);
    note_event(side, bank, 0u, pick);
}

/* ---- DOA2U's replay ----------------------------------------------------
   The game keeps the fight's last RING_LENGTH (0x8a7eec, 300) frames as
   snapshots in a ring, writing frame after frame at RING_WRITE (0x8a7ef8)
   and counting the frames held at RING_HELD (0x8a7f00, which starts again
   from 0 each round). It keeps on for about two seconds after a KO, through
   the fall. The replay then plays the ring back from oldest to newest: its
   frame shown (REPLAY_FRAME, 0x87ae2c, 0..length-1, from the float position
   at 0x87d25c) walks forward from ~60 while the ring stands still. A frame
   written at ring index k is replay frame (k - newest - 1) mod length.

   Every clip a fighter voices is noted with the ring frame it played on;
   as the replay passes that frame the same clip plays again, so the replay
   sounds as the fight did, grunts and KO cry alike. */
enum {
    RING_WRITE = 0x008a7ef8u,
    RING_LENGTH = 0x008a7eecu,
    RING_HELD = 0x008a7f00u,
    REPLAY_FRAME = 0x0087ae2cu,
    EVENTS = 64u,
};

typedef struct Event {
    uint32_t frame;          /* the ring frame it played on, counted from the round's start */
    uint8_t side, level;     /* level 1..3 a grunt, 0 the KO cry */
    uint8_t clip;
    long long stamp;         /* the bank it came from */
} Event;

static Event events[EVENTS];
static unsigned event_count;
static uint32_t ring_frames;     /* frames written this round */
static uint16_t last_write, last_shown, last_held;

static uint32_t ring_now(void)
{
    const uint16_t length = *recomp_memory_u16(RING_LENGTH);
    const uint16_t write = *recomp_memory_u16(RING_WRITE);
    return length == 0u ? ring_frames : ring_frames + (uint32_t)((write + length - last_write) % length);
}

static void note_event(unsigned side, const Bank *bank, unsigned level, unsigned clip)
{
    if (event_count == EVENTS) {
        memmove(events, events + 1, sizeof events - sizeof events[0]);
        --event_count;
    }
    events[event_count++] = (Event){ring_now(), (uint8_t)side, (uint8_t)level, (uint8_t)clip, bank->stamp};
}

void recomp_hit_voice_frame(void)
{
    const uint16_t write = *recomp_memory_u16(RING_WRITE);
    const uint16_t shown = *recomp_memory_u16(REPLAY_FRAME);
    const uint16_t length = *recomp_memory_u16(RING_LENGTH);
    const uint16_t held = *recomp_memory_u16(RING_HELD);
    if (held < last_held) event_count = ring_frames = 0u; /* a new round's recording */
    else if (length != 0u) ring_frames += (uint32_t)((write + length - last_write) % length);
    /* The replay runs while the ring stands still and the shown frame steps on. */
    const bool replaying = write == last_write && shown > last_shown && shown - last_shown <= 3;
    if (replaying && choice != 0u && !online_session && length != 0u) {
        for (unsigned i = 0u; i < event_count; ++i) {
            const Event *event = &events[i];
            if (ring_frames - event->frame >= length) continue; /* no longer in the ring */
            const unsigned at = length - 1u - (ring_frames - event->frame);
            if (!(last_shown < at && shown >= at)) continue;
            Bank *bank = bank_for(*recomp_memory(SIDE_CHARACTER + event->side * SIDE_STRIDE, 1u));
            if (bank == NULL || bank->stamp != event->stamp) continue;
            if (event->level == 0u && event->clip < bank->kos)
                play(event->side, bank, &bank->ko[event->clip], true);
            else if (event->level >= 1u && event->level <= LEVELS && event->clip < bank->hits[event->level])
                play(event->side, bank, &bank->hit[event->level][event->clip], false);
        }
    }
    last_write = write;
    last_shown = shown;
    last_held = held;
    recomp_hit_voice_pump();
}

/* 0x10ce60(side): the base damage of the hit side takes (in al). */
static void note_base_damage(void)
{
    const uint32_t side = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    sub_0010CE60();
    if (side < 2u) base_damage[side] += recomp_runtime.registers.eax & 0xffu;
}

static void apply_life(void)
{
    uint16_t life[2], pending[2];
    uint32_t base[2];
    for (unsigned side = 0u; side < 2u; ++side) {
        life[side] = *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE);
        pending[side] = *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE + 2u);
        base[side] = base_damage[side];
        base_damage[side] = 0u;
    }
    sub_00119420();
    for (unsigned side = 0u; side < 2u; ++side)
        if (pending[side] != 0u && life[side] != 0u && *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE) == 0u)
            recomp_ko_replay_note_ko(side);
    if (choice == 0u || online_session) return;
    for (unsigned side = 0u; side < 2u; ++side) {
        if (pending[side] == 0u) continue;
        voice_hit(side, base[side] != 0u ? base[side] : pending[side], life[side],
            *recomp_memory_u16(SIDE_LIFE + side * SIDE_STRIDE));
    }
    recomp_hit_voice_pump();
}

void recomp_hit_voice_set_choice(unsigned value)
{
    choice = value < 4u ? value : 2u;
    if (choice == 0u) {
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
    base_damage[0] = base_damage[1] = 0u;
    for (unsigned side = 0u; side < 2u; ++side) {
        Voice *voice = &voices[side];
        voice->clip = NULL;
        voice->bank = NULL;
        voice->crying = false;
        voice->last_grunt = voice->last_hit_time = -1e9;
        voice->meter = voice->threshold = 0u;
        voice->last_ko = -1;
        for (unsigned level = 0u; level <= LEVELS; ++level) voice->last_hit[level] = -1;
    }
}

RecompFunction recomp_hit_voice_lookup_manual(uint32_t guest_address)
{
    return guest_address == APPLY_LIFE ? apply_life : guest_address == BASE_DAMAGE ? note_base_damage : NULL;
}
