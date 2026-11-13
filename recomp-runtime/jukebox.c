/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "jukebox.h"
#include "adx_decoder.h"
#include "audio_output.h"
#include "dsound_buffer_adapter.h"
#include "kernel_abi.h"
#include "mod_loader.h"
#include "runner_assets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define make_folder(path) _mkdir(path)
#else
#include <dirent.h>
#define make_folder(path) mkdir(path, 0755)
#endif

enum {
    PATH_SIZE = 1024, NAME_SIZE = 72,
    LEAD_MS = 120,         /* queued ahead of the clock */
    CHUNK_MS = 20,         /* at least this much a submission */
    FADE_MS = 4000,        /* Play All and Shuffle fade a looping track out */
    PASSES = 2,            /* ... after it has looped this many times */
    MAX_ALBUMS = 8,
    ROW_ALBUM = 0, ROW_TRACK, ROW_MODE, ROW_TARGET, ROW_ASSIGNED,
};

#define BGM "doa2/bgm.afs"

/* ---- Albums --------------------------------------------------------------- */

typedef struct Track {
    char name[NAME_SIZE];
    char file[64];              /* its name in tracks.txt, or in bgm.afs for DOA2U's */
    char path[PATH_SIZE];
    uint32_t offset, size;      /* in the file: an AFS entry, or all of it (size 0) */
    int64_t loop_start, loop_end; /* samples; -1 the ADX header's own, -2 none */
} Track;

typedef struct Album {
    char tab[16];               /* DOA2U, DOA3, DOA4 */
    char title[NAME_SIZE];
    Track *tracks;
    unsigned count;
    const char *missing;        /* why it is empty */
} Album;

static Album albums[MAX_ALBUMS];
static unsigned album_count;
static char disc_root[PATH_SIZE], jukebox_root[PATH_SIZE], probe_root[PATH_SIZE];

/* DOA2U's music table (0x6a0b2c: label, ?, file; 68 entries of 16 bytes) by
   file, in its order, with the label made readable; files it lists twice
   or as dummy.adx are left out. */
static const struct { const char *file, *name; } DOA2U_NAMES[] = {
    {"bgm_doa2_ks.adx", "Kasumi"}, {"bgm_doa2_ln.adx", "Leon"}, {"bgm_doa2_gn.adx", "Gen Fu"},
    {"bgm_doa2_zk.adx", "Zack"}, {"bgm_doa2_tg.adx", "Tengu"}, {"bgm_doa2_le.adx", "Leifang"},
    {"bgm_doa2_jn.adx", "Jann Lee"}, {"bgm_doa2_tn.adx", "Tina"}, {"bgm_doa2_ba.adx", "Bass"},
    {"bgm_doa2_ry.adx", "Ryu Hayabusa"}, {"bgm_doa2_ay.adx", "Ayane"}, {"bgm_doa2_he.adx", "Helena"},
    {"bgm_doa2_ei.adx", "Ein"}, {"bgm_doa2_by.adx", "Bayman"}, {"bgm_doa2_ko.adx", "Hitomi"},
    {"bgm_doa2_sel.adx", "Character Select"}, {"bgm_doa2_staffroll.adx", "Staff Roll"},
    {"bgm_doa2_name.adx", "Name Entry"}, {"bgm_doa2_trainning.adx", "Sparring"},
    {"bgm_doa2_zk_end.adx", "Zack's Ending"}, {"bgm_doa2_he_end.adx", "Helena's Ending"},
    {"bgm_doa2_jn_end.adx", "Jann Lee's Ending"}, {"bgm_doa2_tn_end.adx", "Tina's Ending"},
    {"bgm_doa2_he_con.adx", "Helena (Story)"}, {"bgm_doa2_ei_con.adx", "Ein (Story)"},
    {"bgm_doa2_ba_con.adx", "Bass (Story)"}, {"bgm_doa2_rplyend.adx", "Replay End"},
    {"bgm_doa2_tagend.adx", "Tag Battle End"}, {"bgm_doa2_gn2.adx", "Gen Fu (DOA1)"},
    {"bgm_doa2_le2.adx", "Leifang (DOA1)"}, {"doa12_e3_bgm.adx", "E3 Demo"},
    {"rmx_ks.adx", "Kasumi (Remix)"}, {"rmx_ln.adx", "Leon (Remix)"}, {"rmx_gn.adx", "Gen Fu (Remix)"},
    {"rmx_zk.adx", "Zack (Remix)"}, {"rmx_tg.adx", "Tengu (Remix)"}, {"rmx_le.adx", "Leifang (Remix)"},
    {"rmx_jn.adx", "Jann Lee (Remix)"}, {"rmx_tn.adx", "Tina (Remix)"}, {"rmx_ba.adx", "Bass (Remix)"},
    {"rmx_ry.adx", "Ryu Hayabusa (Remix)"}, {"rmx_ay.adx", "Ayane (Remix)"}, {"rmx_he.adx", "Helena (Remix)"},
    {"rmx_ei.adx", "Ein (Remix)"}, {"rmx_by.adx", "Bayman (Remix)"}, {"rmx_ko.adx", "Hitomi (Remix)"},
    {"bgm_doa2_online.adx", "Online"}, {"dreamon.adx", "Dream On"}, {"ups.adx", "UPS"},
    {"tgs2004.adx", "TGS 2004"},
};

const char *recomp_jukebox_doa2u_name(const char *file)
{
    for (size_t i = 0u; i < sizeof DOA2U_NAMES / sizeof DOA2U_NAMES[0]; ++i) {
        const char *a = DOA2U_NAMES[i].file, *b = file;
        while (*a != '\0' && tolower((unsigned char)*b) == *a) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return DOA2U_NAMES[i].name;
    }
    return NULL;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool add_track(Album *album, const Track *track)
{
    Track *grown = realloc(album->tracks, (album->count + 1u) * sizeof *grown);
    if (grown == NULL) return false;
    album->tracks = grown;
    album->tracks[album->count++] = *track;
    return true;
}

/* names.txt beside an album's tracks: "file = name" lines a player edits to
   rename tracks, and "album = title". Written with every track the album has
   whenever it lacks one, so each track is always listed there. */
static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    size_t length = strlen(text);
    while (length > 0u && (text[length - 1u] == ' ' || text[length - 1u] == '\t')) text[--length] = '\0';
    return text;
}

static void apply_names(Album *album, const char *folder)
{
    if (album->count == 0u) return;
    char path[PATH_SIZE], line[PATH_SIZE];
    snprintf(path, sizeof path, "%s/%s", jukebox_root, folder);
    make_folder(jukebox_root);
    make_folder(path);
    snprintf(path, sizeof path, "%s/%s/names.txt", jukebox_root, folder);
    bool *listed = calloc(album->count, sizeof *listed);
    if (listed == NULL) return;
    bool titled = false;
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        while (fgets(line, sizeof line, file) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            char *text = line;
            if ((unsigned char)text[0] == 0xefu && (unsigned char)text[1] == 0xbbu && (unsigned char)text[2] == 0xbfu) text += 3;
            if (text[0] == '#') continue;
            char *equals = strchr(text, '=');
            if (equals == NULL) continue;
            *equals = '\0';
            const char *key = trim(text), *name = trim(equals + 1);
            if (*name == '\0') continue;
            if (strcmp(key, "album") == 0) {
                snprintf(album->title, sizeof album->title, "%s", name);
                titled = true;
                continue;
            }
            for (unsigned t = 0u; t < album->count; ++t)
                if (strcmp(album->tracks[t].file, key) == 0) {
                    snprintf(album->tracks[t].name, sizeof album->tracks[t].name, "%s", name);
                    listed[t] = true;
                }
        }
        fclose(file);
    }
    bool complete = titled;
    for (unsigned t = 0u; t < album->count; ++t) complete = complete && listed[t];
    free(listed);
    if (complete || (file = fopen(path, "w")) == NULL) return;
    fprintf(file, "# The Jukebox's names for the %s album's tracks.\n"
                  "# Change the text after \"=\" and save; the game reads this file when it starts.\n"
                  "# Leave the file name before \"=\" as it is.\n\n", album->tab);
    fprintf(file, "album = %s\n\n", album->title);
    for (unsigned t = 0u; t < album->count; ++t) fprintf(file, "%s = %s\n", album->tracks[t].file, album->tracks[t].name);
    fclose(file);
}

static void load_doa2u(Album *album)
{
    snprintf(album->tab, sizeof album->tab, "DOA2U");
    snprintf(album->title, sizeof album->title, "Dead or Alive 2 Ultimate");
    album->missing = "Not on the disc";
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc_root);
#ifdef _WIN32
    for (char *p = path; *p != '\0'; ++p) if (*p == '/') *p = '\\';
#endif
    FILE *file = fopen(path, "rb");
    if (file == NULL) return;
    uint8_t head[8];
    if (fread(head, 1u, 8u, file) != 8u || memcmp(head, "AFS", 3u) != 0) { fclose(file); return; }
    const uint32_t count = le32(head + 4);
    uint8_t *table = count > 0u && count < 4096u ? malloc(8u * (count + 1u)) : NULL;
    uint8_t *names = NULL;
    if (table != NULL && fread(table, 8u, count + 1u, file) == count + 1u) {
        const uint32_t names_at = le32(table + 8u * count), names_size = le32(table + 8u * count + 4u);
        names = names_size >= 48u * count ? malloc(names_size) : NULL;
        if (names != NULL && (fseek(file, (long)names_at, SEEK_SET) != 0 || fread(names, 1u, names_size, file) != names_size)) {
            free(names);
            names = NULL;
        }
    }
    fclose(file);
    if (names != NULL) {
        /* In the music table's order: the names' order, not the archive's. */
        for (size_t n = 0u; n < sizeof DOA2U_NAMES / sizeof DOA2U_NAMES[0]; ++n)
            for (uint32_t i = 0u; i < count; ++i) {
                char entry[33] = {0};
                memcpy(entry, names + 48u * i, 32u);
                if (recomp_jukebox_doa2u_name(entry) != DOA2U_NAMES[n].name) continue;
                Track track = {0};
                snprintf(track.name, sizeof track.name, "%s", DOA2U_NAMES[n].name);
                snprintf(track.file, sizeof track.file, "%s", DOA2U_NAMES[n].file);
                snprintf(track.path, sizeof track.path, "%s", path);
                track.offset = le32(table + 8u * i);
                track.size = le32(table + 8u * i + 4u);
                track.loop_start = track.loop_end = -1;
                add_track(album, &track);
                break;
            }
    }
    free(table);
    free(names);
    apply_names(album, "doa2u");
}

/* Dead or Alive Ultimate's own DOA1 (Doa1.xbe) on the same disc: loose ADX in
   DOA1/Sound, in its music table's order (0x2d50a8 in Doa1.xbe). The codes are
   DOA3's: gy is the slot Leon took in DOA2, Bayman's in DOA1. */
static const struct { const char *file, *name; } DOA1U_NAMES[] = {
    {"ks_sng.adx", "Kasumi"}, {"lu_sng.adx", "Jann Lee"}, {"le_sng.adx", "Leifang"},
    {"km_sng.adx", "Ryu Hayabusa"}, {"gy_sng.adx", "Bayman"}, {"fa_sng.adx", "Gen Fu"},
    {"kl_sng.adx", "Zack"}, {"tn_sng.adx", "Tina"}, {"rd_sng.adx", "Raidou"},
    {"sel_sng.adx", "Character Select"}, {"name_sng.adx", "Name Entry"}, {"cnt_sng.adx", "Continue"},
    {"clr_sng.adx", "Clear"}, {"trng_sng.adx", "Training"},
    {"kl_lose.adx", "Zack (Lose)"}, {"le_lose.adx", "Leifang (Lose)"}, {"km_lose.adx", "Ryu Hayabusa (Lose)"},
    {"lu_lose.adx", "Jann Lee (Lose)"}, {"tn_lose.adx", "Tina (Lose)"}, {"gy_lose.adx", "Bayman (Lose)"},
    {"fa_lose.adx", "Gen Fu (Lose)"}, {"ks_lose.adx", "Kasumi (Lose)"}, {"ks_lose2.adx", "Kasumi (Lose 2)"},
    {"rd_lose.adx", "Raidou (Lose)"}, {"opmov_sng.adx", "Opening Movie"}, {"edmov_sng.adx", "Ending Movie"},
    {"online_sng.adx", "Online"},
};

static bool folder_exists(const char *name);
static void load_folder(Album *album, const char *folder, const char *tab);

static void load_doa1u(Album *album)
{
    /* The build copies them into jukebox\doa1u, levelled with DOA2U's music; the disc's otherwise. */
    if (folder_exists("doa1u")) {
        album->missing = "No tracks listed";
        load_folder(album, "doa1u", "DOA1U");
        if (album->count > 0u) return;
    }
    snprintf(album->tab, sizeof album->tab, "DOA1U");
    snprintf(album->title, sizeof album->title, "Dead or Alive Ultimate");
    album->missing = "Not on the disc";
    for (size_t n = 0u; n < sizeof DOA1U_NAMES / sizeof DOA1U_NAMES[0]; ++n) {
        Track track = {0};
        snprintf(track.name, sizeof track.name, "%s", DOA1U_NAMES[n].name);
        snprintf(track.file, sizeof track.file, "%s", DOA1U_NAMES[n].file);
        snprintf(track.path, sizeof track.path, "%s/DOA1/Sound/%s", disc_root, DOA1U_NAMES[n].file);
#ifdef _WIN32
        for (char *p = track.path; *p != '\0'; ++p) if (*p == '/') *p = '\\';
#endif
        FILE *file = fopen(track.path, "rb");
        uint8_t head[2] = {0};
        const bool adx = file != NULL && fread(head, 1u, 2u, file) == 2u && head[0] == 0x80u && head[1] == 0u;
        if (file != NULL) fclose(file);
        if (!adx) continue;
        track.loop_start = track.loop_end = -1;
        add_track(album, &track);
    }
    apply_names(album, "doa1u");
}

/* tracks.txt: "#title<TAB>name", then file<TAB>loop start<TAB>loop end<TAB>name. */
static void load_folder(Album *album, const char *folder, const char *tab)
{
    char path[PATH_SIZE], line[PATH_SIZE + 128];
    snprintf(album->tab, sizeof album->tab, "%s", tab);
    snprintf(album->title, sizeof album->title, "%s", tab);
    snprintf(path, sizeof path, "%s/%s/tracks.txt", jukebox_root, folder);
    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char *field[4] = {line, NULL, NULL, NULL};
        for (unsigned f = 1u; f < 4u; ++f) {
            char *tab_at = field[f - 1u] != NULL ? strchr(field[f - 1u], '\t') : NULL;
            if (tab_at == NULL) break;
            *tab_at = '\0';
            field[f] = tab_at + 1;
        }
        if (strcmp(field[0], "#title") == 0 && field[1] != NULL) {
            snprintf(album->title, sizeof album->title, "%s", field[1]);
            continue;
        }
        if (field[0][0] == '#' || field[0][0] == '\0' || field[3] == NULL) continue;
        Track track = {0};
        snprintf(track.name, sizeof track.name, "%s", field[3]);
        snprintf(track.file, sizeof track.file, "%s", field[0]);
        snprintf(track.path, sizeof track.path, "%s/%s/%s", jukebox_root, folder, field[0]);
        track.loop_start = strtoll(field[1], NULL, 10);
        track.loop_end = strtoll(field[2], NULL, 10);
        add_track(album, &track);
    }
    fclose(file);
    apply_names(album, folder);
}

static bool folder_exists(const char *name)
{
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/%s/tracks.txt", jukebox_root, name);
    FILE *file = fopen(path, "r");
    if (file != NULL) fclose(file);
    return file != NULL;
}

static void load_library(void)
{
    for (unsigned a = 0u; a < album_count; ++a) free(albums[a].tracks);
    memset(albums, 0, sizeof albums);
    album_count = 0u;
    load_doa2u(&albums[album_count++]);
    load_doa1u(&albums[album_count++]);
    static const struct { const char *folder, *tab; } KNOWN[] = {{"doa3", "DOA3"}, {"doa4", "DOA4"}};
    for (size_t k = 0u; k < sizeof KNOWN / sizeof KNOWN[0]; ++k) {
        Album *album = &albums[album_count++];
        album->missing = "Import its disc in Launcher.exe";
        load_folder(album, KNOWN[k].folder, KNOWN[k].tab);
    }
    /* Any other album a player puts beside them, by folder name. */
#ifdef _WIN32
    char pattern[PATH_SIZE];
    struct _finddata_t data;
    snprintf(pattern, sizeof pattern, "%s/*", jukebox_root);
    intptr_t find = _findfirst(pattern, &data);
    if (find != -1) {
        do {
            const char *name = data.name;
            if ((data.attrib & _A_SUBDIR) == 0) continue;
#else
    DIR *dir = opendir(jukebox_root);
    if (dir != NULL) {
        for (struct dirent *entry; (entry = readdir(dir)) != NULL;) {
            const char *name = entry->d_name;
#endif
            if (name[0] == '.' || album_count == MAX_ALBUMS) continue;
            char lower_name[64];
            snprintf(lower_name, sizeof lower_name, "%s", name);
            for (char *c = lower_name; *c != '\0'; ++c) *c = (char)tolower((unsigned char)*c);
            if (strcmp(lower_name, "doa3") == 0 || strcmp(lower_name, "doa4") == 0 || strcmp(lower_name, "doa1u") == 0 ||
                !folder_exists(name))
                continue;
            char tab[16];
            snprintf(tab, sizeof tab, "%s", name);
            for (char *c = tab; *c != '\0'; ++c) *c = (char)toupper((unsigned char)*c);
            Album *album = &albums[album_count++];
            album->missing = "No tracks listed";
            load_folder(album, name, tab);
#ifdef _WIN32
        } while (_findnext(find, &data) == 0);
        _findclose(find);
    }
#else
        }
        closedir(dir);
    }
#endif
    unsigned total = 0u;
    for (unsigned a = 0u; a < album_count; ++a) total += albums[a].count;
    fprintf(stderr, "recomp jukebox: %u albums, %u tracks\n", album_count, total);
}

static uint64_t track_size(const Track *track)
{
    if (track->size != 0u) return track->size;
    struct stat info;
    return stat(track->path, &info) == 0 ? (uint64_t)info.st_size : 0u;
}

/* ---- The player ------------------------------------------------------------ */

typedef enum Mode { MODE_REPEAT, MODE_ALBUM, MODE_SHUFFLE, MODES } Mode;
static const char *const MODE_NAMES[MODES] = {"Repeat One", "Play All", "Shuffle"};

typedef struct Player {
    bool playing;
    unsigned album, track;
    uint8_t *data;
    uint32_t size, data_start, at;      /* the file, its first frame, the next frame */
    RecompAdxDecoder decoder;
    int64_t loop_start, loop_end;       /* samples; loop_end 0 for none */
    uint32_t skip;                      /* samples to drop from the next frame (a loop's start) */
    unsigned passes;                    /* loops played */
    double start_ms;                    /* the clock at sample 0 of the output */
    uint64_t sent;                      /* samples submitted */
    uint64_t position;                  /* the track's sample */
    double fade_from;                   /* sent at which the fade began; < 0 none */
    bool finished;                      /* decoded to its end */
    int16_t carry[RECOMP_ADX_FRAME_SAMPLES * 2];
    uint32_t carry_count, carry_used;
} Player;

static Player player;
static Mode mode = MODE_REPEAT;
static uint32_t random_state = 0x9e3779b9u;

static double now_ms(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static void stop(void)
{
    if (player.playing) {
        recomp_audio_output_reset_voice(RECOMP_AUDIO_OUTPUT_JUKEBOX_SLOT);
        recomp_dsound_buffer_set_output_muted(false);
    }
    free(player.data);
    memset(&player, 0, sizeof player);
}

static bool load(const Track *track)
{
    FILE *file = fopen(track->path, "rb");
    if (file == NULL) return false;
    uint32_t size = track->size;
    if (size == 0u) {
        fseek(file, 0, SEEK_END);
        size = (uint32_t)ftell(file);
    }
    uint8_t *data = size > 0u ? malloc(size) : NULL;
    const bool ok = data != NULL && fseek(file, (long)track->offset, SEEK_SET) == 0 && fread(data, 1u, size, file) == size;
    fclose(file);
    if (!ok) { free(data); return false; }
    RecompAdxDecoder decoder;
    const uint32_t data_start = recomp_adx_open(&decoder, data, size);
    if (data_start == 0u) {
        fprintf(stderr, "recomp jukebox: %s is not an ADX the Jukebox plays\n", track->name);
        free(data);
        return false;
    }
    player.data = data;
    player.size = size;
    player.data_start = player.at = data_start;
    player.decoder = decoder;
    player.loop_start = player.loop_end = 0;
    if (track->loop_start >= 0 && track->loop_end > track->loop_start) {
        player.loop_start = track->loop_start;
        player.loop_end = track->loop_end;
    } else if (track->loop_start == -1) {
        /* The header's loop: version 3 at 0x18, version 4 at 0x24. */
        const uint32_t at = data[0x12] == 3u ? 0x18u : data[0x12] == 4u ? 0x24u : 0u;
        if (at != 0u && at + 0x14u <= data_start && be32(data + at) != 0u) {
            player.loop_start = be32(data + at + 4u);
            player.loop_end = be32(data + at + 12u);
            if (player.loop_end <= player.loop_start || player.loop_end > decoder.total_samples) player.loop_end = 0;
        }
    }
    return true;
}

static void play(unsigned album, unsigned track)
{
    stop();
    if (album >= album_count || track >= albums[album].count) return;
    if (!load(&albums[album].tracks[track])) return;
    player.playing = true;
    player.album = album;
    player.track = track;
    player.start_ms = now_ms();
    player.fade_from = -1.0;
    /* The game's own sound waits while a track plays. */
    recomp_dsound_buffer_set_output_muted(true);
    fprintf(stderr, "recomp jukebox: playing %s / %s\n", albums[album].tab, albums[album].tracks[track].name);
}

/* The next sample frame (stereo) into out; false at the end. */
static bool next_sample(int16_t out[2])
{
    Player *p = &player;
    const uint32_t channels = p->decoder.channels, block = RECOMP_ADX_FRAME_BYTES * channels;
    while (p->carry_used >= p->carry_count) {
        if (p->loop_end > 0 && (int64_t)p->decoder.decoded >= p->loop_end) {
            /* Back to the frame holding the loop's start, keeping the history. */
            const uint64_t frame = (uint64_t)p->loop_start / RECOMP_ADX_FRAME_SAMPLES;
            p->at = p->data_start + (uint32_t)(frame * block);
            p->decoder.decoded = (uint32_t)(frame * RECOMP_ADX_FRAME_SAMPLES);
            p->decoder.ended = false;
            p->skip = (uint32_t)(p->loop_start % RECOMP_ADX_FRAME_SAMPLES);
            p->position = (uint64_t)p->loop_start;
            ++p->passes;
        }
        if (p->at + block > p->size) return false;
        const uint32_t before = p->decoder.decoded;
        uint32_t n = recomp_adx_decode(&p->decoder, p->data + p->at, p->carry);
        if (n == 0u) return false;
        p->at += block;
        /* A loop ends mid-frame: keep only the samples before it. */
        if (p->loop_end > 0 && (int64_t)before + n > p->loop_end) {
            n = (uint32_t)(p->loop_end - before);
            p->decoder.decoded = (uint32_t)p->loop_end;
        }
        p->carry_count = n;
        p->carry_used = p->skip < n ? p->skip : n;
        p->skip = 0u;
    }
    const uint32_t i = p->carry_used++;
    out[0] = p->carry[i * channels];
    out[1] = p->carry[i * channels + (channels > 1u ? 1u : 0u)];
    ++p->position;
    return true;
}

static void advance(void)
{
    const Album *album = &albums[player.album];
    unsigned next = player.track + 1u;
    if (mode == MODE_SHUFFLE && album->count > 1u) {
        random_state = random_state * 1664525u + 1013904223u;
        next = (player.track + 1u + (random_state >> 8) % (album->count - 1u)) % album->count;
    }
    play(player.album, next % album->count);
}

static void feed(void)
{
    if (!player.playing || player.data == NULL) return;
    const uint32_t rate = player.decoder.sample_rate;
    const double now = now_ms();
    const double played = (now - player.start_ms) * rate / 1000.0;
    uint64_t target = (uint64_t)(played > 0.0 ? played : 0.0) + (uint64_t)rate * LEAD_MS / 1000u;
    /* Fell far behind (a stall): start the clock again rather than flood. */
    if (target > player.sent + (uint64_t)rate) {
        player.start_ms = now - (double)player.sent * 1000.0 / rate;
        target = player.sent + (uint64_t)rate * LEAD_MS / 1000u;
    }
    if (player.finished) {
        if (played > (double)player.sent + rate / 4.0) {
            if (mode == MODE_REPEAT) play(player.album, player.track);
            else advance();
        }
        return;
    }
    if (target < player.sent + (uint64_t)rate * CHUNK_MS / 1000u) return;
    static int16_t pcm[2 * 48000];
    uint32_t frames = 0u;
    const uint32_t want = (uint32_t)(target - player.sent) < 48000u ? (uint32_t)(target - player.sent) : 48000u;
    /* Play All and Shuffle move on after two passes of a loop, fading out. */
    if (mode != MODE_REPEAT && player.loop_end > 0 && player.passes >= PASSES && player.fade_from < 0.0)
        player.fade_from = (double)player.sent;
    const double fade_length = (double)rate * FADE_MS / 1000.0;
    while (frames < want) {
        int16_t s[2];
        if (!next_sample(s)) { player.finished = true; break; }
        if (player.fade_from >= 0.0) {
            const double left = 1.0 - ((double)(player.sent + frames) - player.fade_from) / fade_length;
            if (left <= 0.0) { player.finished = true; break; }
            s[0] = (int16_t)(s[0] * left);
            s[1] = (int16_t)(s[1] * left);
        }
        pcm[2u * frames] = s[0];
        pcm[2u * frames + 1u] = s[1];
        ++frames;
    }
    if (frames > 0u)
        recomp_audio_output_submit(RECOMP_AUDIO_OUTPUT_JUKEBOX_SLOT, (const uint8_t *)pcm, frames * 4u, rate, 2u, 16u, 0);
    player.sent += frames;
}

/* ---- Assignments ----------------------------------------------------------- */

/* What a player can give a track to: an entry of the game's music table. */
typedef struct Target {
    const char *file, *name;
    int entry;                  /* in bgm.afs; -1 without the mod loader's slots */
    int album, track;           /* assigned, or -1 */
} Target;

static Target targets[sizeof DOA2U_NAMES / sizeof DOA2U_NAMES[0]];
static unsigned target_count;
static bool assigning;          /* bgm.afs has slots: assignments work */

static int find_album(const char *tab)
{
    for (unsigned a = 0u; a < album_count; ++a)
        if (strcmp(albums[a].tab, tab) == 0) return (int)a;
    return -1;
}

static int find_track(unsigned album, const char *file)
{
    for (unsigned t = 0u; t < albums[album].count; ++t)
        if (strcmp(albums[album].tracks[t].file, file) == 0) return (int)t;
    return -1;
}

static bool apply(Target *target)
{
    if (target->entry < 0) return false;
    const RecompModSource *source = NULL;
    if (target->album >= 0) {
        const Track *track = &albums[target->album].tracks[target->track];
        source = recomp_mod_source(track->path, track->offset, track->size);
        if (source == NULL) {
            fprintf(stderr, "recomp jukebox: cannot read %s for %s\n", track->name, target->name);
            target->album = target->track = -1;
        }
    }
    if (!recomp_mod_assign(BGM, (unsigned)target->entry, source)) {
        fprintf(stderr, "recomp jukebox: that track does not fit %s's slot\n", target->name);
        target->album = target->track = -1;
        recomp_mod_assign(BGM, (unsigned)target->entry, NULL);
        return false;
    }
    return true;
}

static void save_assignments(void)
{
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        fprintf(stderr, "recomp jukebox: cannot write %s\n", path);
        return;
    }
    fprintf(file, "# The Jukebox's assignments: the game's music<TAB>album<TAB>track file. Set in game.\n");
    for (unsigned i = 0u; i < target_count; ++i) {
        if (targets[i].album < 0) continue;
        fprintf(file, "%s\t%s\t%s\n", targets[i].file, albums[targets[i].album].tab,
            albums[targets[i].album].tracks[targets[i].track].file);
    }
    fclose(file);
}

static void load_assignments(void)
{
    char path[PATH_SIZE], line[512];
    snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#') continue;
        char *album = strchr(line, '\t'), *track = album != NULL ? strchr(album + 1, '\t') : NULL;
        if (track == NULL) continue;
        *album++ = '\0';
        *track++ = '\0';
        for (unsigned i = 0u; i < target_count; ++i) {
            if (strcmp(targets[i].file, line) != 0) continue;
            const int a = find_album(album), t = a >= 0 ? find_track((unsigned)a, track) : -1;
            if (t < 0) {
                fprintf(stderr, "recomp jukebox: %s's track %s/%s is gone\n", targets[i].name, album, track);
                break;
            }
            targets[i].album = a;
            targets[i].track = t;
            if (apply(&targets[i]))
                fprintf(stderr, "recomp jukebox: %s plays %s / %s\n", targets[i].name, album, albums[a].tracks[t].name);
        }
    }
    fclose(file);
}

/* The mod loader's hook, on its reading thread: the game's music starting.
   It logs each track with two bytes to learn from: the main menu's track,
   and which byte holds the fight's stage. */
static const RecompModSource *music_starts(const char *key, unsigned index, const RecompModSource *wanted)
{
    (void)wanted;
    if (strcmp(key, BGM) != 0) return NULL;
    const char *name = recomp_mod_entry_name(BGM, index);
    fprintf(stderr, "recomp jukebox: music starts: %s (entry %u), byte 0x8783a9 = %u, 0x878394 = %u\n",
        name != NULL ? name : "?", index, *recomp_memory(0x008783a9u, 1u), *recomp_memory(0x00878394u, 1u));
    return NULL;
}

/* ---- The page -------------------------------------------------------------- */

static struct {
    unsigned album, track, target;
    bool requested;
    char value[RECOMP_JUKEBOX_ROWS][96];
    char note[96];              /* the last action, on the help line */
} page;

uint64_t recomp_jukebox_prepare(const char *disc, const char *private_dir)
{
    const char *base = private_dir != NULL && *private_dir ? private_dir : "private";
    snprintf(disc_root, sizeof disc_root, "%s", disc != NULL ? disc : ".");
    /* The library sits beside mods\ in the game's folder, where players can
       find it; older builds kept it in private\jukebox. */
    char root[PATH_SIZE];
    snprintf(root, sizeof root, "%s", base);
    size_t cut = strlen(root);
    while (cut > 0u && (root[cut - 1u] == '/' || root[cut - 1u] == '\\')) root[--cut] = '\0';
    while (cut > 0u && root[cut - 1u] != '/' && root[cut - 1u] != '\\') --cut;
    if (cut > 0u) root[cut - 1u] = '\0';
    else snprintf(root, sizeof root, ".");
    snprintf(jukebox_root, sizeof jukebox_root, "%s/jukebox", root);
    char old_root[PATH_SIZE];
    snprintf(old_root, sizeof old_root, "%s/jukebox", base);
    struct stat info;
    if (stat(jukebox_root, &info) != 0 && stat(old_root, &info) == 0) {
        if (rename(old_root, jukebox_root) == 0) fprintf(stderr, "recomp jukebox: moved %s to %s\n", old_root, jukebox_root);
        else snprintf(jukebox_root, sizeof jukebox_root, "%s", old_root);
    }
    snprintf(probe_root, sizeof probe_root, "%s/claude-scratch/probe", base);
    load_library();
    uint64_t largest = 0u;
    for (unsigned a = 0u; a < album_count; ++a)
        for (unsigned t = 0u; t < albums[a].count; ++t) {
            const uint64_t size = track_size(&albums[a].tracks[t]);
            if (size > largest) largest = size;
        }
    return largest;
}

void recomp_jukebox_install(bool online)
{
    stop();
    const unsigned album = page.album, track = page.track, target = page.target;
    memset(&page, 0, sizeof page);
    page.album = album;
    page.track = track;
    page.target = target;
    target_count = 0u;
    for (size_t i = 0u; i < sizeof DOA2U_NAMES / sizeof DOA2U_NAMES[0]; ++i) {
        targets[target_count] = (Target){DOA2U_NAMES[i].file, DOA2U_NAMES[i].name,
            recomp_mod_entry_index(BGM, DOA2U_NAMES[i].file), -1, -1};
        ++target_count;
    }
    recomp_mod_loader_enable_slots(!online);
    assigning = target_count > 0u && targets[0].entry >= 0;
    recomp_mod_entry_start = music_starts;
    if (!assigning) {
        fprintf(stderr, "recomp jukebox: bgm.afs has no slots; tracks cannot be assigned\n");
        return;
    }
    /* Online both games must sound alike: the game's own music there. */
    if (!online) load_assignments();
    else for (unsigned i = 0u; i < target_count; ++i) recomp_mod_assign(BGM, (unsigned)targets[i].entry, NULL);
    if (online) assigning = false;
}

void recomp_jukebox_request_page(void) { page.requested = true; }

bool recomp_jukebox_take_request(void)
{
    const bool requested = page.requested;
    page.requested = false;
    return requested;
}

void recomp_jukebox_page_opened(void)
{
    load_library(); /* an album imported since shows */
    for (unsigned i = 0u; i < target_count; ++i) targets[i].album = targets[i].track = -1;
    if (assigning) load_assignments();
    if (page.album >= album_count) page.album = 0u;
    if (page.track >= albums[page.album].count) page.track = 0u;
    if (page.target >= target_count) page.target = 0u;
    page.note[0] = '\0';
    fprintf(stderr, "recomp jukebox: page opened\n");
}

void recomp_jukebox_page_closed(void)
{
    stop();
    fprintf(stderr, "recomp jukebox: page closed\n");
}

const char *recomp_jukebox_label(unsigned row)
{
    static const char *const LABELS[RECOMP_JUKEBOX_ROWS] = {"ALBUM", "TRACK", "PLAY MODE", "ASSIGN TO", "ASSIGNED"};
    return row < RECOMP_JUKEBOX_ROWS ? LABELS[row] : "";
}

unsigned recomp_jukebox_choices(unsigned row)
{
    switch (row) {
    case ROW_ALBUM: return album_count > 0u ? album_count : 1u;
    case ROW_TRACK: return albums[page.album].count > 0u ? albums[page.album].count : 1u;
    case ROW_MODE: return MODES;
    case ROW_TARGET: return target_count > 0u ? target_count : 1u;
    default: return 1u;
    }
}

unsigned recomp_jukebox_choice(unsigned row)
{
    switch (row) {
    case ROW_ALBUM: return page.album;
    case ROW_TRACK: return page.track;
    case ROW_MODE: return (unsigned)mode;
    case ROW_TARGET: return page.target;
    default: return 0u;
    }
}

void recomp_jukebox_choose(unsigned row, unsigned choice)
{
    page.note[0] = '\0';
    if (choice >= recomp_jukebox_choices(row)) choice = 0u;
    switch (row) {
    case ROW_ALBUM:
        if (choice < album_count && choice != page.album) {
            page.album = choice;
            page.track = player.playing && player.album == choice ? player.track : 0u;
        }
        break;
    case ROW_TRACK: page.track = choice; break;
    case ROW_MODE: mode = (Mode)choice; break;
    case ROW_TARGET: page.target = choice; break;
    default: break;
    }
}

void recomp_jukebox_confirm(unsigned row)
{
    const Album *album = &albums[page.album];
    switch (row) {
    case ROW_ALBUM:
    case ROW_TRACK:
        if (album->count == 0u) break;
        if (player.playing && player.album == page.album && player.track == page.track) stop();
        else play(page.album, page.track);
        break;
    case ROW_TARGET:
        if (!assigning || album->count == 0u || page.target >= target_count) {
            snprintf(page.note, sizeof page.note, "Tracks cannot be assigned here.");
            break;
        }
        targets[page.target].album = (int)page.album;
        targets[page.target].track = (int)page.track;
        if (apply(&targets[page.target])) {
            save_assignments();
            snprintf(page.note, sizeof page.note, "%s's music is now %s: %s.", targets[page.target].name, album->tab,
                album->tracks[page.track].name);
        } else {
            snprintf(page.note, sizeof page.note, "That track does not fit; restart the game to make room.");
        }
        break;
    case ROW_ASSIGNED:
        if (page.target < target_count && targets[page.target].album >= 0) {
            targets[page.target].album = targets[page.target].track = -1;
            apply(&targets[page.target]);
            save_assignments();
            snprintf(page.note, sizeof page.note, "%s plays its own music again.", targets[page.target].name);
        }
        break;
    default: break;
    }
}

/* Long names are cut to fit the value column. */
static const char *fit(char *out, size_t size, const char *text)
{
    enum { MAX = 26 };
    snprintf(out, size, "%s", text);
    if (strlen(out) > MAX && size > MAX) strcpy(out + MAX - 3, "...");
    return out;
}

const char *recomp_jukebox_value(unsigned row)
{
    char *out = page.value[row < RECOMP_JUKEBOX_ROWS ? row : 0u];
    const size_t size = sizeof page.value[0];
    const Album *album = &albums[page.album];
    char text[160];
    switch (row) {
    case ROW_ALBUM: return fit(out, size, album->tab);
    case ROW_TRACK:
        if (album->count == 0u) return fit(out, size, album->missing);
        snprintf(text, sizeof text, "%s%s", player.playing && player.album == page.album && player.track == page.track ? "> " : "",
            album->tracks[page.track].name);
        return fit(out, size, text);
    case ROW_MODE: return fit(out, size, MODE_NAMES[mode]);
    case ROW_TARGET: return fit(out, size, page.target < target_count ? targets[page.target].name : "-");
    case ROW_ASSIGNED: {
        const Target *t = page.target < target_count ? &targets[page.target] : NULL;
        if (t == NULL || t->album < 0) return fit(out, size, "Original");
        snprintf(text, sizeof text, "%s: %s", albums[t->album].tab, albums[t->album].tracks[t->track].name);
        return fit(out, size, text);
    }
    default: return "";
    }
}

const char *recomp_jukebox_help(unsigned row)
{
    static char help[160];
    if (page.note[0] != '\0') return page.note;
    const Album *album = &albums[page.album];
    switch (row) {
    case ROW_ALBUM: return "Choose DOA2U's music, or DOA3's and DOA4's from your discs.";
    case ROW_TRACK:
        if (album->count == 0u) return "Import this game's disc in Launcher.exe to add its music.";
        snprintf(help, sizeof help, "Track %u of %u. Press A to play or stop it.", page.track + 1u, album->count);
        return help;
    case ROW_MODE: return "Repeat One loops a track; Play All and Shuffle move on.";
    case ROW_TARGET: return "Choose the game's music to change, then press A to give it this track.";
    case ROW_ASSIGNED: return "What the music above plays. Press A to put its own track back.";
    default: return "";
    }
}

/* ---- The probe ----------------------------------------------------------- */

/* With private/claude-scratch/probe present (a developer's folder), the
   menu's word art (sprite group 7, the main menu labels) and the game's
   message text are saved there once, to letter new labels from. */
static bool probe_on(void)
{
    struct stat info;
    return probe_root[0] != '\0' && stat(probe_root, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}

static bool find_sprite(uint32_t id, RecompGameSprite *s)
{
    for (int h = 1; h <= 256; ++h)
        if (recomp_game_sprite(id, h, s)) return true;
    return false;
}

static void save_sprite(uint32_t id)
{
    RecompGameSprite s;
    if (!find_sprite(id, &s)) return;
    const int w = s.right - s.left, h = s.bottom - s.top;
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/sprite_%05x.pam", probe_root, (unsigned)id);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return;
    fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t argb = recomp_game_sprite_colour(&s, recomp_game_sprite_index(&s, x, y));
            const uint8_t px[4] = {(uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb, (uint8_t)(argb >> 24)};
            fwrite(px, 1u, 4u, file);
        }
    fclose(file);
}

void recomp_jukebox_pump(void)
{
    feed();
    static bool sprites_saved;
    if (sprites_saved || !probe_on()) return;
    RecompGameSprite s;
    if (!find_sprite(0x70008u, &s)) return;
    for (uint32_t i = 0u; i < 0x40u; ++i) save_sprite(0x70000u | i);
    sprites_saved = true;
    fprintf(stderr, "recomp jukebox: probe saved the main menu's word art\n");
}

extern void sub_0006A3E0(void);

void recomp_jukebox_probe_messages(void)
{
    static bool saved;
    if (saved || !probe_on()) return;
    saved = true;
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/messages.txt", probe_root);
    FILE *file = fopen(path, "w");
    if (file == NULL) return;
    for (uint32_t n = 0u; n < 0x600u; ++n) {
        const uint32_t args[2] = {n, 1u};
        const uint32_t text = kernel_call_lifted(sub_0006A3E0, args, 2u);
        if (text == 0u || text >= 0x04000000u) continue;
        fprintf(file, "%04x\t", (unsigned)n);
        for (unsigned c = 0u; c < 200u; ++c) {
            const uint16_t ch = *recomp_memory_u16(text + 2u * c);
            if (ch == 0u) break;
            if (ch == '\n') fputs("\\n", file);
            else if (ch < 0x80u) fputc((int)ch, file);
            else fprintf(file, "\\u%04x", ch);
        }
        fputc('\n', file);
    }
    fclose(file);
    fprintf(stderr, "recomp jukebox: probe saved the game's messages\n");
}
