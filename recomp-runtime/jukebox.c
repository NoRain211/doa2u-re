/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "jukebox.h"
#include "adx_decoder.h"
#include "audio_output.h"
#include "dsound_buffer_adapter.h"
#include "kernel_abi.h"
#include "mod_loader.h"
#include "runner_assets.h"
#include "atomic_file.h"
#include "utf8_text.h"

#include <ctype.h>
#include <errno.h>
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
    char tab[16];               /* DOA2U, DOA3, DOA4, DOAXBV */
    char title[NAME_SIZE];
    Track *tracks;
    unsigned count;
    const char *missing;        /* why it is empty */
} Album;

static Album albums[MAX_ALBUMS];
static unsigned album_count;
static char disc_root[PATH_SIZE], jukebox_root[PATH_SIZE];

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- The executables ------------------------------------------------------ */

/* An XBE's sections, so the tables in its data section can be read through
   their virtual addresses: the game names its music there, not in bgm.afs. */
typedef struct Xbe {
    FILE *file;
    uint32_t base;
    struct { uint32_t at, size, file_at; } sections[32];
    unsigned count;
} Xbe;

static bool xbe_open(Xbe *xbe, const char *path)
{
    memset(xbe, 0, sizeof *xbe);
    uint8_t magic[4], head[8], directory[8];
    xbe->file = fopen(path, "rb");
    if (xbe->file == NULL) return false;
    bool ok = fseek(xbe->file, 0, SEEK_SET) == 0 && fread(magic, 1u, sizeof magic, xbe->file) == sizeof magic &&
        memcmp(magic, "XBEH", 4u) == 0 &&
        fseek(xbe->file, 0x100, SEEK_SET) == 0 && fread(head, 1u, sizeof head, xbe->file) == sizeof head;
    if (ok) xbe->base = le32(head + 4);
    ok = ok && fseek(xbe->file, 0x11cu, SEEK_SET) == 0 && fread(directory, 1u, 8u, xbe->file) == 8u;
    const uint32_t count = ok ? le32(directory) : 0u, at = ok ? le32(directory + 4) : 0u;
    /* Each section: flags, virtual address, virtual size, file address, file size. */
    ok = ok && at >= xbe->base && count > 0u && count <= sizeof xbe->sections / sizeof xbe->sections[0] &&
        fseek(xbe->file, (long)(at - xbe->base), SEEK_SET) == 0;
    for (uint32_t i = 0u; ok && i < count; ++i) {
        uint8_t section[20];
        ok = fread(section, 1u, sizeof section, xbe->file) == sizeof section &&
            fseek(xbe->file, 56L - (long)sizeof section, SEEK_CUR) == 0;
        if (!ok) break;
        xbe->sections[i].at = le32(section + 4);
        xbe->sections[i].size = le32(section + 16);
        xbe->sections[i].file_at = le32(section + 12);
        ++xbe->count;
    }
    if (!ok) { fclose(xbe->file); xbe->file = NULL; }
    return ok;
}

static void xbe_close(Xbe *xbe)
{
    if (xbe->file != NULL) fclose(xbe->file);
    xbe->file = NULL;
}

static bool xbe_at(const Xbe *xbe, uint32_t va, size_t size, long *at)
{
    for (unsigned i = 0u; i < xbe->count; ++i) {
        const uint32_t start = xbe->sections[i].at;
        if (va < start || (uint64_t)(va - start) + size > xbe->sections[i].size) continue;
        *at = (long)(xbe->sections[i].file_at + (va - start));
        return true;
    }
    return false;
}

static bool xbe_read(Xbe *xbe, uint32_t va, void *out, size_t size)
{
    long at;
    return xbe_at(xbe, va, size, &at) && fseek(xbe->file, at, SEEK_SET) == 0 &&
        fread(out, 1u, size, xbe->file) == size;
}

/* The string a table entry points at; false where the table ends (a 0 pointer). */
static bool xbe_string(Xbe *xbe, uint32_t va, char *out, size_t size)
{
    if (va == 0u || size < 2u) return false;
    for (unsigned i = 0u; i < xbe->count; ++i) {
        const uint32_t start = xbe->sections[i].at;
        if (va < start || va - start >= xbe->sections[i].size) continue;
        const size_t left = xbe->sections[i].size - (va - start);
        const size_t room = left < size ? left : size - 1u;
        if (fseek(xbe->file, (long)(xbe->sections[i].file_at + (va - start)), SEEK_SET) != 0) return false;
        const size_t got = fread(out, 1u, room, xbe->file);
        if (memchr(out, '\0', got) == NULL) return false;
        return true;
    }
    return false;
}

static void host_path(char *path)
{
#ifdef _WIN32
    for (; *path != '\0'; ++path) if (*path == '/') *path = '\\';
#else
    (void)path;
#endif
}

/* A name in the player's files and one in the game's table differ only by case. */
static bool same_name(const char *a, const char *b)
{
    while (*a != '\0' && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return *a == '\0' && *b == '\0';
}

/* A file's own words, for a name no label covers. */
static void stem_label(const char *file, char *out, size_t size)
{
    size_t w = 0u;
    bool start = true;
    for (const char *c = file; *c != '\0' && *c != '.' && w + 1u < size; ++c) {
        if (*c == '_' || *c == '-' || *c == ' ') { start = true; out[w++] = ' '; continue; }
        out[w++] = start ? (char)toupper((unsigned char)*c) : *c;
        start = false;
    }
    out[w] = '\0';
}

/* The music table's own label, made readable: "End_X" is X's ending, "Remix_X"
   a remix, "Con_X" its story track and "DOA1_X" the first game's; a capital
   after a lower-case letter starts a word ("NameEntry"). */
static void readable_label(const char *label, char *out, size_t size)
{
    static const struct { const char *code, *format; } PREFIX[] = {
        {"End_", "%s's Ending"}, {"Remix_", "%s (Remix)"}, {"Con_", "%s (Story)"}, {"DOA1_", "%s (DOA1)"},
    };
    const char *rest = label, *format = "%s";
    for (size_t i = 0u; i < sizeof PREFIX / sizeof PREFIX[0]; ++i) {
        const size_t length = strlen(PREFIX[i].code);
        if (strncmp(label, PREFIX[i].code, length) == 0) {
            rest = label + length;
            format = PREFIX[i].format;
            break;
        }
    }
    char words[NAME_SIZE];
    size_t w = 0u;
    for (const char *c = rest; *c != '\0' && w + 2u < sizeof words; ++c) {
        if (c != rest && islower((unsigned char)c[-1]) &&
            (isupper((unsigned char)*c) || isdigit((unsigned char)*c)))
            words[w++] = ' ';
        words[w++] = *c;
    }
    words[w] = '\0';
    if (words[0] != '\0') snprintf(out, size, format, words);
    else stem_label(label, out, size);
}

static bool add_track(Album *album, const Track *track)
{
    Track *grown = realloc(album->tracks, (album->count + 1u) * sizeof *grown);
    if (grown == NULL) return false;
    album->tracks = grown;
    album->tracks[album->count++] = *track;
    return true;
}

/* A file read for a rewrite: every line of it, whole however long the line is,
   and the ones the rewrite keeps as the player wrote them. A file that could
   not be read or kept whole is no longer intact, and is left as it is rather
   than rewritten without the lines it holds. */
typedef struct Lines {
    FILE *file;
    char *text;                 /* the line being read, and its length */
    size_t size, room;
    char *kept;                 /* the lines the rewrite keeps, as they were */
    size_t kept_size, kept_at;  /* ... and how many it held before the last */
    bool intact;
} Lines;

static void lines_open(Lines *lines, const char *path)
{
    memset(lines, 0, sizeof *lines);
    lines->file = fopen(path, "r");
    /* Absent, it is safe to create; present but unopenable, it holds lines we never read. */
    lines->intact = lines->file != NULL || errno == ENOENT;
}

/* The next line into text, however long it is; false at the end of the file,
   which it then closes so the rewrite can replace the file. */
static bool lines_next(Lines *lines)
{
    if (lines->file == NULL) return false;
    lines->size = 0u;
    for (int c = getc(lines->file); c != EOF; c = getc(lines->file)) {
        if (lines->size + 1u >= lines->room) {
            const size_t room = lines->room != 0u ? 2u * lines->room : PATH_SIZE;
            char *grown = realloc(lines->text, room);
            if (grown == NULL) { lines->intact = false; return false; }
            lines->text = grown;
            lines->room = room;
        }
        lines->text[lines->size++] = (char)c;
        if (c == '\n') break;
    }
    if (lines->size == 0u) {
        if (ferror(lines->file)) lines->intact = false; /* getc's EOF was a read error */
        fclose(lines->file);
        lines->file = NULL;
        return false;
    }
    lines->text[lines->size] = '\0';
    return true;
}

/* Keeps the line as the player's, the file's last given the newline it lacks;
   false when memory runs out, which leaves the file alone. */
static bool lines_keep(Lines *lines)
{
    char *grown = realloc(lines->kept, lines->kept_size + lines->size + 2u);
    if (grown == NULL) { lines->intact = false; return false; }
    lines->kept = grown;
    lines->kept_at = lines->kept_size;
    memcpy(lines->kept + lines->kept_size, lines->text, lines->size);
    lines->kept_size += lines->size;
    if (lines->text[lines->size - 1u] != '\n') lines->kept[lines->kept_size++] = '\n';
    lines->kept[lines->kept_size] = '\0';
    return true;
}

/* Takes the last line back: it is the file's own, not the player's. */
static void lines_drop(Lines *lines)
{
    lines->kept_size = lines->kept_at;
    lines->kept[lines->kept_size] = '\0';
}

static void lines_close(Lines *lines)
{
    if (lines->file != NULL) fclose(lines->file);
    free(lines->text);
    free(lines->kept);
    memset(lines, 0, sizeof *lines);
}

/* names.txt beside an album's tracks: "file = name" lines a player edits to
   rename tracks, and "album = title". A rewrite keeps every line the player
   already has and adds the ones for tracks it lacks, so each track is listed. */
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
    char path[PATH_SIZE], temporary[PATH_SIZE + 64];
    snprintf(path, sizeof path, "%s/%s", jukebox_root, folder);
    make_folder(jukebox_root);
    make_folder(path);
    snprintf(path, sizeof path, "%s/%s/names.txt", jukebox_root, folder);
    bool *listed = calloc(album->count, sizeof *listed);
    if (listed == NULL) return;
    /* Every line the player has stays as it is; only the missing ones are added. */
    Lines lines;
    lines_open(&lines, path);
    bool titled = false;
    while (lines_next(&lines) && lines_keep(&lines)) {
        char *text = lines.text;
        text[strcspn(text, "\r\n")] = '\0';
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
    bool complete = titled;
    for (unsigned t = 0u; t < album->count; ++t) complete = complete && listed[t];
    const bool rewrite = lines.intact && !complete; /* a file not read whole would lose lines */
    FILE *file = rewrite ? recomp_atomic_open(path, temporary, sizeof temporary) : NULL;
    if (rewrite && file == NULL) fprintf(stderr, "recomp jukebox: cannot write %s\n", path);
    if (file != NULL) {
        bool written = lines.kept_size == 0u
            ? fprintf(file, "# The Jukebox's names for the %s album's tracks.\n"
                      "# Change the text after \"=\" and save; the game reads this file when it starts.\n"
                      "# Leave the file name before \"=\" as it is.\n\n", album->tab) > 0
            : fwrite(lines.kept, 1u, lines.kept_size, file) == lines.kept_size;
        if (!titled) written = written && fprintf(file, "album = %s\n\n", album->title) > 0;
        for (unsigned t = 0u; t < album->count; ++t)
            if (!listed[t]) written = written && fprintf(file, "%s = %s\n", album->tracks[t].file, album->tracks[t].name) > 0;
        if (!recomp_atomic_commit(file, temporary, path, written))
            fprintf(stderr, "recomp jukebox: cannot save %s; previous file kept\n", path);
    }
    lines_close(&lines);
    free(listed);
}

/* DOA2.xbe's music table (label, unused, file, unused: 16 bytes an entry, 0
   ending it) names the music bgm.afs holds and gives each its label. A file
   it names twice is one entry of the archive, labelled for both; its unused
   slots point at a one-sample placeholder. */
enum { MAX_MUSIC_ENTRIES = 256 };
static const uint32_t DOA2U_MUSIC = 0x6a0b2cu;

/* The archive entry whose 32-byte name is file, or -1. */
static int archive_entry(const uint8_t *names, uint32_t count, const char *file)
{
    for (uint32_t i = 0u; i < count; ++i) {
        const char *name = (const char *)names + 48u * i;
        size_t n = 0u;
        while (n < 32u && name[n] != '\0' && tolower((unsigned char)file[n]) == tolower((unsigned char)name[n])) ++n;
        if (n < 32u && name[n] == '\0' && file[n] == '\0') return (int)i;
    }
    return -1;
}

/* Whether an archive entry holds a track: at least a second of ADX. */
static bool archive_audio(FILE *file, const uint8_t *table, uint32_t entry)
{
    const uint32_t offset = le32(table + 8u * entry), size = le32(table + 8u * entry + 4u);
    uint8_t head[0x14];
    if (size < sizeof head || fseek(file, (long)offset, SEEK_SET) != 0 ||
        fread(head, 1u, sizeof head, file) != sizeof head)
        return false;
    if (head[0] != 0x80u || head[1] != 0x00u) return false;
    const uint32_t rate = be32(head + 8), samples = be32(head + 12);
    return rate >= 1000u && samples >= rate;
}

static void load_doa2u(Album *album)
{
    snprintf(album->tab, sizeof album->tab, "DOA2U");
    snprintf(album->title, sizeof album->title, "Dead or Alive 2 Ultimate");
    album->missing = "Not on the disc";
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc_root);
    host_path(path);
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
    char executable[PATH_SIZE];
    snprintf(executable, sizeof executable, "%s/DOA2.xbe", disc_root);
    host_path(executable);
    Xbe xbe;
    const bool listed = names != NULL && xbe_open(&xbe, executable);
    if (!listed) album->missing = "No music list on the disc";
    for (unsigned n = 0u; listed && n < MAX_MUSIC_ENTRIES; ++n) {
        uint8_t entry[16];
        if (!xbe_read(&xbe, DOA2U_MUSIC + 16u * n, entry, sizeof entry)) break;
        char label[64], name[64];
        if (!xbe_string(&xbe, le32(entry), label, sizeof label) ||
            !xbe_string(&xbe, le32(entry + 8), name, sizeof name))
            break;
        const int at = archive_entry(names, count, name);
        if (at < 0 || !archive_audio(file, table, (uint32_t)at)) continue;
        char readable[NAME_SIZE];
        readable_label(label, readable, sizeof readable);
        unsigned t = 0u;
        for (; t < album->count; ++t) if (same_name(album->tracks[t].file, name)) break;
        if (t < album->count) {
            Track *track = &album->tracks[t];
            if (strstr(track->name, readable) == NULL &&
                strlen(track->name) + strlen(readable) + 4u < sizeof track->name) {
                strcat(track->name, " / ");
                strcat(track->name, readable);
            }
            continue;
        }
        Track track = {0};
        snprintf(track.name, sizeof track.name, "%s", readable);
        snprintf(track.file, sizeof track.file, "%.32s", (const char *)names + 48u * at);
        snprintf(track.path, sizeof track.path, "%s", path);
        track.offset = le32(table + 8u * at);
        track.size = le32(table + 8u * at + 4u);
        track.loop_start = track.loop_end = -1;
        add_track(album, &track);
    }
    if (listed) xbe_close(&xbe);
    fclose(file);
    free(table);
    free(names);
    apply_names(album, "doa2u");
}

/* Dead or Alive Ultimate's own DOA1 (Doa1.xbe) on the same disc: loose ADX in
   DOA1/Sound, in its music table's order (a pointer per file, 0 ending it).
   Its names use DOA3's fighter codes: gy is the slot Leon took in DOA2,
   Bayman's in DOA1. */
static const uint32_t DOA1U_MUSIC = 0x2e02f8u;
static const struct { const char *code, *name; } DOA1U_CODES[] = {
    {"ks", "Kasumi"}, {"lu", "Jann Lee"}, {"le", "Leifang"}, {"km", "Ryu Hayabusa"},
    {"gy", "Bayman"}, {"fa", "Gen Fu"}, {"kl", "Zack"}, {"tn", "Tina"}, {"rd", "Raidou"},
};
static const struct { const char *code, *name; } DOA1U_LABELS[] = {
    {"sel", "Character Select"}, {"name", "Name Entry"}, {"cnt", "Continue"}, {"clr", "Clear"},
    {"trng", "Training"}, {"opmov", "Opening Movie"}, {"edmov", "Ending Movie"}, {"online", "Online"},
};

/* "ks_sng" is Kasumi's theme, "ks_lose" the one she loses to. */
static void doa1u_label(const char *file, char *out, size_t size)
{
    char stem[64];
    snprintf(stem, sizeof stem, "%s", file);
    char *dot = strrchr(stem, '.');
    if (dot != NULL) *dot = '\0';
    char *kind = strchr(stem, '_');
    if (kind != NULL) *kind++ = '\0';
    const char *base = NULL;
    for (size_t i = 0u; i < sizeof DOA1U_CODES / sizeof DOA1U_CODES[0]; ++i)
        if (strcmp(DOA1U_CODES[i].code, stem) == 0) base = DOA1U_CODES[i].name;
    for (size_t i = 0u; i < sizeof DOA1U_LABELS / sizeof DOA1U_LABELS[0]; ++i)
        if (strcmp(DOA1U_LABELS[i].code, stem) == 0) base = DOA1U_LABELS[i].name;
    const char *suffix = "";
    if (kind != NULL && strcmp(kind, "sng") != 0) {
        if (strcmp(kind, "lose") == 0) suffix = " (Lose)";
        else if (strcmp(kind, "lose2") == 0) suffix = " (Lose 2)";
        else suffix = NULL;
    }
    if (base == NULL || suffix == NULL) { stem_label(file, out, size); return; }
    snprintf(out, size, "%s%s", base, suffix);
}

static bool folder_exists(const char *name);
static void load_folder(Album *album, const char *folder, const char *tab);

/* tracks.txt names files in its own folder: not a path out of it. */
static bool bare_name(const char *name)
{
    if (name[0] == '\0' || strstr(name, "..") != NULL) return false;
    for (const char *c = name; *c != '\0'; ++c)
        if (*c == '/' || *c == '\\' || *c == ':') return false;
    return true;
}

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
    char executable[PATH_SIZE];
    snprintf(executable, sizeof executable, "%s/Doa1.xbe", disc_root);
    host_path(executable);
    Xbe xbe;
    if (!xbe_open(&xbe, executable)) return;
    for (unsigned n = 0u; n < MAX_MUSIC_ENTRIES; ++n) {
        uint8_t entry[4];
        if (!xbe_read(&xbe, DOA1U_MUSIC + 4u * n, entry, sizeof entry)) break;
        char file_name[64];
        if (!xbe_string(&xbe, le32(entry), file_name, sizeof file_name)) break;
        if (!bare_name(file_name)) continue;
        Track track = {0};
        doa1u_label(file_name, track.name, sizeof track.name);
        snprintf(track.file, sizeof track.file, "%s", file_name);
        snprintf(track.path, sizeof track.path, "%s/DOA1/Sound/%s", disc_root, file_name);
        host_path(track.path);
        FILE *file = fopen(track.path, "rb");
        uint8_t head[2] = {0};
        const bool adx = file != NULL && fread(head, 1u, 2u, file) == 2u && head[0] == 0x80u && head[1] == 0u;
        if (file != NULL) fclose(file);
        if (!adx) continue;
        track.loop_start = track.loop_end = -1;
        add_track(album, &track);
    }
    xbe_close(&xbe);
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
        if (!bare_name(field[0])) {
            fprintf(stderr, "recomp jukebox: %s: %s is not a file name in its folder\n", folder, field[0]);
            continue;
        }
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
    static const struct { const char *folder, *tab; } KNOWN[] = {
        {"doa3", "DOA3"}, {"doa4", "DOA4"}, {"doaxbv", "DOAXBV"}};
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
            if (strcmp(lower_name, "doa3") == 0 || strcmp(lower_name, "doa4") == 0 ||
                strcmp(lower_name, "doaxbv") == 0 || strcmp(lower_name, "doa1u") == 0 || !folder_exists(name))
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

/* Ends the track but leaves the game's sound as it is. */
static void release(void)
{
    if (player.playing) recomp_audio_output_reset_voice(RECOMP_AUDIO_OUTPUT_JUKEBOX_SLOT);
    free(player.data);
    memset(&player, 0, sizeof player);
}

static void stop(void)
{
    const bool was_playing = player.playing;
    release();
    if (was_playing) recomp_dsound_buffer_set_output_muted(false);
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
    /* The game stays muted while the next track loads, so it never plays in between. */
    const bool was_playing = player.playing;
    release();
    if (album >= album_count || track >= albums[album].count || !load(&albums[album].tracks[track])) {
        if (was_playing) recomp_dsound_buffer_set_output_muted(false);
        return;
    }
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
    if (player.album >= album_count || albums[player.album].count == 0u) {
        stop();
        return;
    }
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
    /* The game's own music: its two 44.1 kHz stereo stream buffers play at
       -15.9 dB (the buffer headroom's 6 and the game's 9.9), and every album
       is levelled to DOA2U's music files, so a preview at that volume matches
       it. shortcut: fixed at the game's default music volume; read the
       stream buffer's volume if the game's own setting turns out to move it. */
    if (frames > 0u)
        recomp_audio_output_submit(RECOMP_AUDIO_OUTPUT_JUKEBOX_SLOT, (const uint8_t *)pcm, frames * 4u, rate, 2u, 16u,
            -1590);
    player.sent += frames;
}

/* ---- Assignments ----------------------------------------------------------- */

/* What a player can give a track to: an entry of the game's music table. */
typedef struct Target {
    char file[64], name[NAME_SIZE]; /* the album's, copied: its tracks reload */
    int entry;                  /* in bgm.afs; -1 without the mod loader's slots */
    int album, track;           /* assigned, or -1 */
} Target;

static Target targets[MAX_MUSIC_ENTRIES];
static unsigned target_count;
static bool assigning;          /* bgm.afs has slots: assignments work */
static bool installed_online;   /* this boot was online from the start: its slots stay off */
static bool online_now;         /* no previews over the game */

static int find_album(const char *tab)
{
    for (unsigned a = 0u; a < album_count; ++a)
        if (same_name(albums[a].tab, tab)) return (int)a;
    return -1;
}

static int find_track(unsigned album, const char *file)
{
    for (unsigned t = 0u; t < albums[album].count; ++t)
        if (same_name(albums[album].tracks[t].file, file)) return (int)t;
    return -1;
}

static int find_target(const char *file)
{
    for (unsigned i = 0u; i < target_count; ++i)
        if (same_name(targets[i].file, file)) return (int)i;
    return -1;
}

/* Gives the target's assignment to its slot; false, the slot back on its own
   track, when it does not fit. The target itself is the caller's to change. */
static bool apply(Target *target)
{
    if (target->entry < 0) return false;
    const RecompModSource *source = NULL;
    if (target->album >= 0) {
        const Track *track = &albums[target->album].tracks[target->track];
        source = recomp_mod_source(track->path, track->offset, track->size);
        if (source == NULL) {
            fprintf(stderr, "recomp jukebox: cannot read %s for %s\n", track->name, target->name);
            recomp_mod_assign(BGM, (unsigned)target->entry, NULL);
            return false;
        }
    }
    if (!recomp_mod_assign(BGM, (unsigned)target->entry, source)) {
        fprintf(stderr, "recomp jukebox: that track does not fit %s's slot\n", target->name);
        recomp_mod_assign(BGM, (unsigned)target->entry, NULL);
        return false;
    }
    return true;
}

/* Where a saved line's album and track are now; false while the album is
   missing (one being imported again, say) or no longer holds that track. */
static bool find_saved(const char *album, const char *track, int *a, int *t)
{
    *a = find_album(album);
    *t = *a >= 0 ? find_track((unsigned)*a, track) : -1;
    return *t >= 0;
}

/* The one line a save writes itself; the player's own comments stay. */
static const char ASSIGNMENT_HEADER[] =
    "# The Jukebox's assignments: the game's music<TAB>album<TAB>track file. Set in game.\n";

/* A save rewrites the assignments it holds and keeps every line it does not:
   an album that is missing for now, a music entry this build does not list,
   a track the game's table has yet to name, or a player's own comment.
   Names.txt behaves the same. */
static void save_assignments(void)
{
    char path[PATH_SIZE], temporary[PATH_SIZE + 64];
    snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
    Lines lines;
    lines_open(&lines, path);
    while (lines_next(&lines) && lines_keep(&lines)) {
        /* Only the header is the save's: every other '#' line is the player's. */
        if (strncmp(lines.text, ASSIGNMENT_HEADER, sizeof ASSIGNMENT_HEADER - 1u) == 0) { lines_drop(&lines); continue; }
        char *album = strchr(lines.text, '\t'), *track = album != NULL ? strchr(album + 1, '\t') : NULL;
        if (lines.text[0] == '#' || track == NULL) continue;
        *album++ = '\0';
        *track++ = '\0';
        track[strcspn(track, "\r\n")] = '\0';
        const int i = find_target(lines.text);
        int a, t;
        /* An assignment this build places: the save writes that one again itself. */
        if (i >= 0 && (targets[i].album >= 0 || find_saved(album, track, &a, &t))) lines_drop(&lines);
    }
    FILE *file = lines.intact ? recomp_atomic_open(path, temporary, sizeof temporary) : NULL;
    if (lines.intact && file == NULL) fprintf(stderr, "recomp jukebox: cannot write %s\n", path);
    if (file != NULL) {
        bool written = fprintf(file, "%s", ASSIGNMENT_HEADER) > 0;
        for (unsigned i = 0u; written && i < target_count; ++i) {
            if (targets[i].album < 0) continue;
            written = fprintf(file, "%s\t%s\t%s\n", targets[i].file, albums[targets[i].album].tab,
                albums[targets[i].album].tracks[targets[i].track].file) > 0;
        }
        if (written && lines.kept_size > 0u) written = fwrite(lines.kept, 1u, lines.kept_size, file) == lines.kept_size;
        if (!recomp_atomic_commit(file, temporary, path, written))
            fprintf(stderr, "recomp jukebox: cannot save assignments; previous file kept\n");
    }
    lines_close(&lines);
}

static void load_assignments(void)
{
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
    Lines lines;
    lines_open(&lines, path);
    while (lines_next(&lines)) {
        if (lines.text[0] == '#') continue;
        char *album = strchr(lines.text, '\t'), *track = album != NULL ? strchr(album + 1, '\t') : NULL;
        if (track == NULL) continue;
        *album++ = '\0';
        *track++ = '\0';
        track[strcspn(track, "\r\n")] = '\0';
        const int i = find_target(lines.text);
        if (i < 0) continue;
        int a, t;
        if (!find_saved(album, track, &a, &t)) {
            fprintf(stderr, "recomp jukebox: %s's track %s/%s is gone\n", targets[i].name, album, track);
            continue;
        }
        targets[i].album = a;
        targets[i].track = t;
        if (apply(&targets[i]))
            fprintf(stderr, "recomp jukebox: %s plays %s / %s\n", targets[i].name, album, albums[a].tracks[t].name);
        else
            targets[i].album = targets[i].track = -1;
    }
    lines_close(&lines);
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
    installed_online = online_now = online;
    const unsigned album = page.album, track = page.track, target = page.target;
    memset(&page, 0, sizeof page);
    page.album = album;
    page.track = track;
    page.target = target;
    target_count = 0u;
    /* The music table's entries are the album's own tracks: what can replace them. */
    const Album *music = &albums[0];
    for (unsigned t = 0u; t < music->count && target_count < MAX_MUSIC_ENTRIES; ++t) {
        Target *target = &targets[target_count++];
        snprintf(target->file, sizeof target->file, "%s", music->tracks[t].file);
        snprintf(target->name, sizeof target->name, "%s", music->tracks[t].name);
        target->entry = recomp_mod_entry_index(BGM, target->file);
        target->album = target->track = -1;
    }
    if (music->count > MAX_MUSIC_ENTRIES)
        fprintf(stderr, "recomp jukebox: only the first %d of %u music can be assigned\n",
            MAX_MUSIC_ENTRIES, music->count);
    recomp_mod_loader_enable_slots(!online);
    assigning = target_count > 0u && targets[0].entry >= 0;
    recomp_mod_entry_start = NULL;
    if (!assigning) {
        fprintf(stderr, "recomp jukebox: bgm.afs has no slots; tracks cannot be assigned\n");
        return;
    }
    /* Online both games must sound alike: the game's own music there. */
    if (!online) load_assignments();
    else for (unsigned i = 0u; i < target_count; ++i) recomp_mod_assign(BGM, (unsigned)targets[i].entry, NULL);
    if (online) assigning = false;
}

void recomp_jukebox_set_online(bool online)
{
    /* Slots off play every entry's own track and keep the assignments, so
       logoff brings them back as they were. shortcut: an assigned track
       playing at logon jumps to the game's own at its next read, mid-track;
       both games then read the same bytes at once. Finish the track first if
       that jump is heard and the menus can tolerate the difference.
       A boot installed online never loaded its assignments, and the archive
       it opened is the disc's own, so its slots stay off whatever comes
       after; previews read their own files and come back at logoff. */
    online_now = online;
    if (online) stop();
    const bool slots_off = online || installed_online;
    recomp_mod_loader_enable_slots(!slots_off);
    assigning = !slots_off && target_count > 0u && targets[0].entry >= 0;
}

void recomp_jukebox_request_page(void) { page.requested = true; }

bool recomp_jukebox_take_request(void)
{
    const bool requested = page.requested;
    page.requested = false;
    return requested;
}

static void restore_slots(const bool *was_assigned)
{
    for (unsigned i = 0u; i < target_count; ++i)
        if (was_assigned[i] && targets[i].album < 0 && targets[i].entry >= 0)
            recomp_mod_assign(BGM, (unsigned)targets[i].entry, NULL);
}

void recomp_jukebox_page_opened(void)
{
    stop(); /* Album indexes can change while the library is reloaded. */
    load_library(); /* an album imported since shows */
    bool was_assigned[sizeof targets / sizeof targets[0]];
    for (unsigned i = 0u; i < target_count; ++i) {
        was_assigned[i] = targets[i].album >= 0;
        targets[i].album = targets[i].track = -1;
    }
    if (assigning) {
        load_assignments();
        /* A target the file no longer names plays its own music again. */
        restore_slots(was_assigned);
    }
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
        if (online_now) {
            snprintf(page.note, sizeof page.note, "Music is off while online.");
            break;
        }
        if (player.playing && player.album == page.album && player.track == page.track) stop();
        else play(page.album, page.track);
        break;
    case ROW_TARGET: {
        if (!assigning || album->count == 0u || page.target >= target_count) {
            snprintf(page.note, sizeof page.note, "Tracks cannot be assigned here.");
            break;
        }
        Target *target = &targets[page.target];
        const int previous_album = target->album, previous_track = target->track;
        target->album = (int)page.album;
        target->track = (int)page.track;
        if (apply(target)) {
            save_assignments();
            snprintf(page.note, sizeof page.note, "%s's music is now %s: %s.", target->name, album->tab,
                album->tracks[page.track].name);
        } else {
            /* A track that does not fit leaves the one that worked playing. */
            target->album = previous_album;
            target->track = previous_track;
            if (previous_album >= 0) apply(target);
            snprintf(page.note, sizeof page.note, "That track does not fit; restart the game to make room.");
        }
        break;
    }
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
    if (size == 0u) return out;
    const char *end = text;
    unsigned count = 0u;
    while (*end != '\0' && count < MAX) { recomp_utf8_next(&end); ++count; }
    const bool shortened = *end != '\0' || (size_t)(end - text) >= size;
    end = text;
    count = 0u;
    const size_t room = shortened && size >= 4u ? size - 4u : size - 1u;
    while (*end != '\0' && count < (shortened ? MAX - 3u : MAX)) {
        const char *next = end;
        recomp_utf8_next(&next);
        if ((size_t)(next - text) > room) break;
        end = next;
        ++count;
    }
    const size_t bytes = (size_t)(end - text);
    memcpy(out, text, bytes);
    out[bytes] = '\0';
    if (shortened && size >= 4u) memcpy(out + bytes, "...", 4u);
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
    case ROW_ALBUM: return "Choose DOA2U's music, or DOA3's, DOA4's and DOAXBV's from your discs.";
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

/* Once a frame, with no developer probes in the release path. */
void recomp_jukebox_pump(void)
{
    feed();
}
