/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mod_loader.h"
#include "adx_encoder.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

enum { PATH_SIZE = 1024, AFS_ALIGN = 2048, NAME_RECORD = 48 };

/* ---- Host files ------------------------------------------------------------ */

typedef struct HostFile {
    char path[PATH_SIZE];
    uint64_t size;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
} HostFile;

static bool host_open(HostFile *file, const char *path)
{
    snprintf(file->path, sizeof file->path, "%s", path);
#ifdef _WIN32
    file->handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    if (file->handle == INVALID_HANDLE_VALUE) return false;
    if (!GetFileSizeEx(file->handle, &size)) { CloseHandle(file->handle); return false; }
    file->size = (uint64_t)size.QuadPart;
#else
    file->fd = open(path, O_RDONLY);
    struct stat info;
    if (file->fd < 0) return false;
    if (fstat(file->fd, &info) != 0) { close(file->fd); return false; }
    file->size = (uint64_t)info.st_size;
#endif
    return true;
}

static void host_close(HostFile *file)
{
#ifdef _WIN32
    if (file->handle != NULL && file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
    file->handle = INVALID_HANDLE_VALUE;
#else
    if (file->fd >= 0) close(file->fd);
    file->fd = -1;
#endif
}

/* Reads at an offset without moving a shared position: safe from the
   kernel's read worker and the game thread at once. */
static bool host_read(const HostFile *file, uint64_t offset, void *buffer, uint32_t length, uint32_t *read)
{
    *read = 0u;
    if (offset >= file->size) return true;
    if (length > file->size - offset) length = (uint32_t)(file->size - offset);
#ifdef _WIN32
    OVERLAPPED at = {0};
    DWORD got = 0;
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    if (!ReadFile(file->handle, buffer, length, &got, &at) && GetLastError() != ERROR_HANDLE_EOF) return false;
    *read = got;
#else
    while (*read < length) {
        const ssize_t got = pread(file->fd, (char *)buffer + *read, length - *read, (off_t)(offset + *read));
        if (got < 0) return false;
        if (got == 0) break;
        *read += (uint32_t)got;
    }
#endif
    return true;
}

static uint8_t *read_whole(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = length >= 0 ? malloc((size_t)length + 1u) : NULL;
    if (data != NULL && fread(data, 1u, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size = data != NULL ? (size_t)length : 0u;
    return data;
}

static bool is_file(const char *path, uint64_t *size, long long *modified)
{
    struct stat info;
    if (stat(path, &info) != 0 || (info.st_mode & S_IFMT) != S_IFREG) return false;
    if (size != NULL) *size = (uint64_t)info.st_size;
    if (modified != NULL) *modified = (long long)info.st_mtime;
    return true;
}

static void make_directory(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

/* Calls back for each entry of a directory: its name and whether it is one. */
typedef void (*ListEntry)(void *context, const char *name, bool directory);
static void list_directory(const char *path, ListEntry each, void *context)
{
#ifdef _WIN32
    char pattern[PATH_SIZE];
    WIN32_FIND_DATAA data;
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        each(context, data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(find, &data));
    FindClose(find);
#else
    DIR *dir = opendir(path);
    if (dir == NULL) return;
    for (struct dirent *entry; (entry = readdir(dir)) != NULL;) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full[PATH_SIZE];
        struct stat info;
        snprintf(full, sizeof full, "%s/%s", path, entry->d_name);
        each(context, entry->d_name, stat(full, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR);
    }
    closedir(dir);
#endif
}

/* ---- Small helpers ---------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint64_t align_up(uint64_t v) { return (v + AFS_ALIGN - 1u) & ~(uint64_t)(AFS_ALIGN - 1u); }

static void lower(char *s)
{
    for (; *s != '\0'; ++s) *s = (char)tolower((unsigned char)*s);
}

static bool ends_with(const char *s, const char *tail)
{
    const size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s + a - b, tail) == 0;
}

static char *copy_string(const char *s)
{
    char *out = malloc(strlen(s) + 1u);
    if (out != NULL) strcpy(out, s);
    return out;
}

static uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *p = data;
    for (size_t i = 0u; i < size; ++i) hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}

static void join(char *out, size_t size, const char *a, const char *b)
{
#ifdef _WIN32
    snprintf(out, size, "%s\\%s", a, b);
#else
    snprintf(out, size, "%s/%s", a, b);
#endif
}

/* ---- State ---------------------------------------------------------------- */

typedef enum PieceKind { PIECE_BASE, PIECE_BYTES, PIECE_FILE, PIECE_ZERO, PIECE_SLOT } PieceKind;

/* What an entry slot plays: part of a file, or of the archive itself (file NULL). */
struct RecompModSource {
    const HostFile *file;
    uint64_t offset, size;
};

/* An entry in a reserved archive: a fixed span the TOC gives in full, whose
   source can change while the game runs. A change waits for the next read
   of the slot's first byte (a track starting), so a playing track is never
   cut into another. */
typedef struct Slot {
    const RecompModSource *original;
    const RecompModSource *volatile wanted;   /* what plays from the next start */
    const RecompModSource *volatile playing;
} Slot;

typedef struct Piece {
    uint64_t start, length;   /* in the overlay */
    PieceKind kind;
    uint64_t source;          /* offset in the base or the file */
    const uint8_t *bytes;
    const HostFile *file;
} Piece;  /* PIECE_SLOT: source is the entry's index */

struct RecompModOverlay {
    char key[PATH_SIZE];
    uint64_t base_size, size;
    Piece *pieces;
    unsigned count;
    uint8_t *header, *names;
    Slot *slots;             /* per entry, for a reserved archive; else NULL */
    uint32_t entries;
    unsigned replaced;       /* entries a mod replaces */
};

typedef struct Change {     /* one file a mod gives */
    char key[PATH_SIZE];    /* the whole file's key, or the archive's */
    char entry[256];        /* the archive entry's name or index; "" for a whole file */
    char path[PATH_SIZE];   /* the mod's file */
    char mod[256];
} Change;

static Change *changes;
static unsigned change_count, change_capacity;
static HostFile **files;
static unsigned file_count;
static RecompModOverlay **overlays;
static unsigned overlay_count;
static struct { char key[PATH_SIZE]; char path[PATH_SIZE]; } *replacements;
static unsigned replacement_count;
static unsigned mods_on;
static uint64_t digest;
static char cache_root[PATH_SIZE];
static struct { char key[PATH_SIZE]; uint64_t slot; } reserved[4];
static unsigned reserved_count;
static bool slots_enabled = true;
static RecompModSource **sources;
static unsigned source_count;
const RecompModSource *(*recomp_mod_entry_start)(const char *key, unsigned index, const RecompModSource *wanted);

static uint64_t reserved_slot(const char *key)
{
    for (unsigned i = 0u; i < reserved_count; ++i)
        if (strcmp(reserved[i].key, key) == 0) return reserved[i].slot;
    return 0u;
}

static RecompModSource *new_source(const HostFile *file, uint64_t offset, uint64_t size)
{
    RecompModSource *source = calloc(1u, sizeof *source);
    RecompModSource **grown = source != NULL ? realloc(sources, (source_count + 1u) * sizeof *grown) : NULL;
    if (grown == NULL) { free(source); return NULL; }
    sources = grown;
    *source = (RecompModSource){file, offset, size};
    sources[source_count++] = source;
    return source;
}

static void add_change(const char *key, const char *entry, const char *path, const char *mod)
{
    if (change_count == change_capacity) {
        const unsigned capacity = change_capacity == 0u ? 64u : change_capacity * 2u;
        Change *grown = realloc(changes, capacity * sizeof *grown);
        if (grown == NULL) return;
        changes = grown;
        change_capacity = capacity;
    }
    Change *c = &changes[change_count++];
    snprintf(c->key, sizeof c->key, "%s", key);
    snprintf(c->entry, sizeof c->entry, "%s", entry);
    snprintf(c->path, sizeof c->path, "%s", path);
    snprintf(c->mod, sizeof c->mod, "%s", mod);
}

static const HostFile *open_file(const char *path)
{
    HostFile *file = calloc(1u, sizeof *file);
    HostFile **grown = realloc(files, (file_count + 1u) * sizeof *grown);
    if (file == NULL || grown == NULL) { free(file); if (grown != NULL) files = grown; return NULL; }
    files = grown;
    if (!host_open(file, path)) { free(file); return NULL; }
    files[file_count++] = file;
    return file;
}

/* ---- Reading the mods --------------------------------------------------------- */

typedef struct Walk {
    const char *disc_root, *mod;
    char folder[PATH_SIZE];   /* host path of the folder being walked */
    char relative[PATH_SIZE]; /* its key */
} Walk;

static bool disc_has_file(const char *disc_root, const char *key)
{
    char path[PATH_SIZE];
    join(path, sizeof path, disc_root, key);
#ifdef _WIN32
    for (char *p = path; *p != '\0'; ++p) if (*p == '/') *p = '\\';
#endif
    return is_file(path, NULL, NULL);
}

static void walk_entry(void *context, const char *name, bool directory);

static void walk(Walk *w)
{
    list_directory(w->folder, walk_entry, w);
}

static void walk_entry(void *context, const char *name, bool directory)
{
    Walk *w = context;
    char lower_name[256];
    snprintf(lower_name, sizeof lower_name, "%s", name);
    lower(lower_name);
    if (w->relative[0] == '\0' && (strcmp(lower_name, "mod.ini") == 0 || strncmp(lower_name, "readme", 6u) == 0))
        return;
    char key[PATH_SIZE], path[PATH_SIZE];
    if (w->relative[0] == '\0') snprintf(key, sizeof key, "%s", lower_name);
    else snprintf(key, sizeof key, "%s/%s", w->relative, lower_name);
    join(path, sizeof path, w->folder, name);
    if (directory) {
        /* A folder named after one of the disc's archives holds its entries. */
        if (ends_with(lower_name, ".afs") && disc_has_file(w->disc_root, key)) {
            Walk inner = *w;
            snprintf(inner.folder, sizeof inner.folder, "%s", path);
            snprintf(inner.relative, sizeof inner.relative, "%s", key);
            list_directory(path, walk_entry, &inner);
            return;
        }
        Walk inner = *w;
        snprintf(inner.folder, sizeof inner.folder, "%s", path);
        snprintf(inner.relative, sizeof inner.relative, "%s", key);
        walk(&inner);
        return;
    }
    if (w->relative[0] != '\0' && ends_with(w->relative, ".afs") && disc_has_file(w->disc_root, w->relative)) {
        add_change(w->relative, lower_name, path, w->mod);
        return;
    }
    add_change(key, "", path, w->mod);
}

typedef struct ModList { char **names; unsigned count; } ModList;

static void add_mod(void *context, const char *name, bool directory)
{
    ModList *list = context;
    if (!directory || name[0] == '.' || name[0] == '_') return;
    char **grown = realloc(list->names, (list->count + 1u) * sizeof *grown);
    if (grown == NULL) return;
    list->names = grown;
    list->names[list->count++] = copy_string(name);
}

static int compare_names(const void *a, const void *b)
{
    char x[256], y[256];
    snprintf(x, sizeof x, "%s", *(char *const *)a);
    snprintf(y, sizeof y, "%s", *(char *const *)b);
    lower(x);
    lower(y);
    return strcmp(x, y);
}

/* order.txt's folders first, in its order, then the rest by name. */
static void order_mods(const char *mods_root, ModList *list)
{
    qsort(list->names, list->count, sizeof *list->names, compare_names);
    char path[PATH_SIZE];
    join(path, sizeof path, mods_root, "order.txt");
    FILE *file = fopen(path, "r");
    if (file == NULL) return;
    unsigned placed = 0u;
    char line[512];
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        for (unsigned i = placed; i < list->count; ++i) {
            if (compare_names(&list->names[i], &(char *){line}) != 0) continue;
            char *name = list->names[i];
            memmove(list->names + placed + 1u, list->names + placed, (i - placed) * sizeof *list->names);
            list->names[placed++] = name;
            break;
        }
    }
    fclose(file);
}

/* ---- WAV entries ------------------------------------------------------------------ */

/* The ADX of a WAV, encoded once into mods/.cache under a name that changes
   with the WAV; "" when it cannot be read. */
static void converted(const char *wav, bool loop_whole, char *out, size_t out_size)
{
    uint64_t size = 0u;
    long long modified = 0;
    out[0] = '\0';
    if (!is_file(wav, &size, &modified)) return;
    uint64_t hash = fnv(0xcbf29ce484222325ull, wav, strlen(wav));
    hash = fnv(hash, &size, sizeof size);
    hash = fnv(hash, &modified, sizeof modified);
    hash = fnv(hash, &loop_whole, sizeof loop_whole);
    char name[64];
    snprintf(name, sizeof name, "%016llx.adx", (unsigned long long)hash);
    make_directory(cache_root);
    join(out, out_size, cache_root, name);
    if (is_file(out, NULL, NULL)) return;
    size_t bytes;
    uint8_t *data = read_whole(wav, &bytes);
    uint32_t frames, channels, rate;
    int64_t loop_start, loop_end;
    int16_t *pcm = data != NULL ? recomp_wav_read(data, bytes, &frames, &channels, &rate, &loop_start, &loop_end) : NULL;
    free(data);
    if (pcm == NULL) {
        fprintf(stderr, "recomp mods: %s is not a PCM WAV; skipped\n", wav);
        out[0] = '\0';
        return;
    }
    if (loop_start < 0 && loop_whole) { loop_start = 0; loop_end = frames; }
    size_t adx_size = 0u;
    uint8_t *adx = recomp_adx_encode(pcm, frames, channels, rate, loop_start, loop_end, &adx_size);
    free(pcm);
    FILE *file = adx != NULL ? fopen(out, "wb") : NULL;
    const bool written = file != NULL && fwrite(adx, 1u, adx_size, file) == adx_size;
    if (file != NULL) fclose(file);
    free(adx);
    if (!written) {
        remove(out);
        out[0] = '\0';
        return;
    }
    fprintf(stderr, "recomp mods: encoded %s as ADX (%u Hz, %u ch%s)\n", wav, (unsigned)rate, (unsigned)channels,
        loop_start >= 0 ? ", looping" : "");
}

/* ---- Archives ----------------------------------------------------------------- */

/* The entry a mod file names: an index (10, 10.adx, 10_title.wav) or the
   entry's name with any extension; all entries of that name. */
static bool names_entry(const char *token, unsigned index, const char *entry_name)
{
    char *end;
    const unsigned long n = strtoul(token, &end, 10);
    if (end != token && (*end == '\0' || *end == '.' || *end == '_')) return n == index;
    if (entry_name == NULL || entry_name[0] == '\0') return false;
    char a[256], b[256];
    snprintf(a, sizeof a, "%s", token);
    snprintf(b, sizeof b, "%s", entry_name);
    lower(b);
    char *dot = strrchr(a, '.');
    if (dot != NULL) *dot = '\0';
    dot = strrchr(b, '.');
    if (dot != NULL) *dot = '\0';
    return strcmp(a, b) == 0;
}

static bool add_piece(RecompModOverlay *o, uint64_t length, PieceKind kind, uint64_t source, const uint8_t *bytes,
    const HostFile *file)
{
    if (length == 0u) return true;
    Piece *grown = realloc(o->pieces, (o->count + 1u) * sizeof *grown);
    if (grown == NULL) return false;
    o->pieces = grown;
    o->pieces[o->count++] = (Piece){o->size, length, kind, source, bytes, file};
    o->size += length;
    return true;
}

typedef struct Entry { uint32_t offset, size, index; } Entry;
static int by_offset(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : (int)x->index - (int)y->index;
}

static void build_archive(const char *disc_root, const char *key)
{
    char path[PATH_SIZE];
    join(path, sizeof path, disc_root, key);
#ifdef _WIN32
    for (char *p = path; *p != '\0'; ++p) if (*p == '/') *p = '\\';
#endif
    HostFile base;
    if (!host_open(&base, path)) return;
    uint8_t head[8];
    uint32_t got;
    RecompModOverlay *o = NULL;
    Entry *entries = NULL;
    uint8_t *table = NULL;
    if (!host_read(&base, 0u, head, 8u, &got) || got != 8u || memcmp(head, "AFS", 3u) != 0) goto fail;
    const uint32_t count = le32(head + 4);
    if (count == 0u || count > 0x100000u) goto fail;
    const uint32_t table_size = 8u * (count + 1u);
    table = malloc(table_size);
    if (table == NULL || !host_read(&base, 8u, table, table_size, &got) || got != table_size) goto fail;
    const uint32_t names_offset = le32(table + 8u * count), names_size = le32(table + 8u * count + 4u);
    const bool named = names_offset != 0u && names_size >= count * NAME_RECORD &&
        (uint64_t)names_offset + names_size <= base.size;

    o = calloc(1u, sizeof *o);
    entries = calloc(count, sizeof *entries);
    if (o == NULL || entries == NULL) goto fail;
    snprintf(o->key, sizeof o->key, "%s", key);
    o->base_size = base.size;
    if (named) {
        o->names = malloc(names_size);
        if (o->names == NULL || !host_read(&base, names_offset, o->names, names_size, &got) || got != names_size) goto fail;
    }

    /* Each entry's file: the last mod's that names it. */
    const HostFile **chosen = calloc(count, sizeof *chosen);
    unsigned replaced = 0u;
    if (chosen == NULL) goto fail;
    uint32_t data_start = UINT32_MAX;
    for (uint32_t i = 0u; i < count; ++i) {
        entries[i] = (Entry){le32(table + 8u * i), le32(table + 8u * i + 4u), i};
        if (entries[i].size != 0u && entries[i].offset < data_start) data_start = entries[i].offset;
    }
    for (unsigned c = 0u; c < change_count; ++c) {
        if (changes[c].entry[0] == '\0' || strcmp(changes[c].key, key) != 0) continue;
        bool found = false;
        for (uint32_t i = 0u; i < count; ++i) {
            char name[33] = {0};
            if (named) memcpy(name, o->names + (size_t)i * NAME_RECORD, 32u);
            if (!names_entry(changes[c].entry, i, named ? name : NULL)) continue;
            found = true;
            const char *source = changes[c].path;
            char adx[PATH_SIZE];
            char lower_path[PATH_SIZE];
            snprintf(lower_path, sizeof lower_path, "%s", source);
            lower(lower_path);
            if (ends_with(lower_path, ".wav")) {
                /* Loop the whole track where the original looped. */
                uint8_t original[0x1c] = {0};
                bool looped = false;
                if (host_read(&base, entries[i].offset, original, sizeof original, &got) && got == sizeof original)
                    looped = original[0] == 0x80u && original[0x12] == 3u && le32(original + 0x18) != 0u;
                converted(source, looped, adx, sizeof adx);
                if (adx[0] == '\0') break;
                source = adx;
            }
            const HostFile *file = open_file(source);
            if (file == NULL) {
                fprintf(stderr, "recomp mods: cannot read %s; skipped\n", changes[c].path);
                break;
            }
            if (chosen[i] == NULL) ++replaced;
            chosen[i] = file;
        }
        if (!found) fprintf(stderr, "recomp mods: %s names no entry of %s; skipped\n", changes[c].path, key);
    }
    const uint64_t slot_size = reserved_slot(key);
    if (replaced == 0u && slot_size == 0u) { free(chosen); goto fail; }
    o->replaced = replaced;
    if (slot_size != 0u) {
        o->slots = calloc(count, sizeof *o->slots);
        if (o->slots == NULL) { free(chosen); goto fail; }
        o->entries = count;
    }
    if (data_start == UINT32_MAX || data_start < 8u + table_size) data_start = (uint32_t)align_up(8u + table_size);

    /* The new table of contents first, then each entry in the original's order. */
    o->header = calloc(1u, data_start);
    if (o->header == NULL) { free(chosen); goto fail; }
    memcpy(o->header, head, 8u);
    if (!add_piece(o, data_start, PIECE_BYTES, 0u, o->header, NULL)) { free(chosen); goto fail; }
    qsort(entries, count, sizeof *entries, by_offset);
    for (uint32_t k = 0u; k < count; ++k) {
        const Entry *e = &entries[k];
        const HostFile *file = chosen[e->index];
        uint64_t size = file != NULL ? file->size : e->size;
        if (e->size == 0u && file == NULL) { /* an empty slot keeps its zero offset */
            put_le32(o->header + 8u + 8u * e->index, e->offset);
            continue;
        }
        bool ok;
        if (o->slots != NULL) {
            /* A reserved archive gives every entry the same room, so any track fits any entry. */
            const RecompModSource *original = file != NULL ? new_source(file, 0u, file->size)
                                                           : new_source(NULL, e->offset, e->size);
            if (original == NULL) { free(chosen); goto fail; }
            o->slots[e->index] = (Slot){original, original, original};
            if (size < slot_size) size = slot_size;
            size = align_up(size);
            ok = add_piece(o, size, PIECE_SLOT, e->index, NULL, NULL);
        } else {
            ok = file != NULL ? add_piece(o, size, PIECE_FILE, 0u, NULL, file)
                              : add_piece(o, size, PIECE_BASE, e->offset, NULL, NULL);
        }
        put_le32(o->header + 8u + 8u * e->index, (uint32_t)(o->size - size));
        put_le32(o->header + 12u + 8u * e->index, (uint32_t)size);
        if (named) put_le32(o->names + (size_t)e->index * NAME_RECORD + 44u, (uint32_t)size);
        if (!ok || !add_piece(o, align_up(o->size) - o->size, PIECE_ZERO, 0u, NULL, NULL)) { free(chosen); goto fail; }
    }
    if (named) {
        put_le32(o->header + 8u + 8u * count, (uint32_t)o->size);
        put_le32(o->header + 12u + 8u * count, names_size);
        if (!add_piece(o, names_size, PIECE_BYTES, 0u, o->names, NULL) ||
            !add_piece(o, align_up(o->size) - o->size, PIECE_ZERO, 0u, NULL, NULL)) { free(chosen); goto fail; }
    }
    free(chosen);
    if (o->size > 0xffffffffull) {
        fprintf(stderr, "recomp mods: %s would pass 4 GB; left as it is\n", key);
        goto fail;
    }
    RecompModOverlay **grown = realloc(overlays, (overlay_count + 1u) * sizeof *grown);
    if (grown == NULL) goto fail;
    overlays = grown;
    overlays[overlay_count++] = o;
    fprintf(stderr, "recomp mods: %s: %u entries replaced (%llu -> %llu bytes)\n", key, replaced,
        (unsigned long long)o->base_size, (unsigned long long)o->size);
    free(entries);
    free(table);
    host_close(&base);
    return;
fail:
    if (o != NULL) { free(o->pieces); free(o->header); free(o->names); free(o->slots); free(o); }
    free(entries);
    free(table);
    host_close(&base);
}

/* What a file adds to the digest: its key, size and first and last 4 KB. */
static uint64_t digest_file(uint64_t hash, const char *key, const char *entry, const HostFile *file)
{
    uint8_t sample[4096];
    uint32_t got = 0u;
    hash = fnv(hash, key, strlen(key));
    hash = fnv(hash, entry, strlen(entry));
    hash = fnv(hash, &file->size, sizeof file->size);
    if (host_read(file, 0u, sample, sizeof sample, &got)) hash = fnv(hash, sample, got);
    if (file->size > sizeof sample && host_read(file, file->size - sizeof sample, sample, sizeof sample, &got))
        hash = fnv(hash, sample, got);
    return hash;
}

bool recomp_mod_loader_install(const char *mods_root, const char *disc_root)
{
    recomp_mod_loader_shutdown();
    if (mods_root == NULL || disc_root == NULL || mods_root[0] == '\0') return false;
    snprintf(cache_root, sizeof cache_root, "%s", mods_root);
    join(cache_root, sizeof cache_root, mods_root, ".cache");
    ModList list = {0};
    list_directory(mods_root, add_mod, &list);
    if (list.count == 0u && reserved_count == 0u) {
        free(list.names);
        return false;
    }
    if (list.count > 0u) order_mods(mods_root, &list);
    for (unsigned i = 0u; i < list.count; ++i) {
        Walk w = {disc_root, list.names[i], "", ""};
        join(w.folder, sizeof w.folder, mods_root, list.names[i]);
        const unsigned before = change_count;
        walk(&w);
        if (change_count != before) ++mods_on;
        fprintf(stderr, "recomp mods: %s: %u files\n", list.names[i], change_count - before);
    }

    /* Whole files: the last mod's wins. */
    digest = 0xcbf29ce484222325ull;
    for (unsigned c = 0u; c < change_count; ++c) {
        if (changes[c].entry[0] != '\0') continue;
        unsigned r = 0u;
        while (r < replacement_count && strcmp(replacements[r].key, changes[c].key) != 0) ++r;
        if (r == replacement_count) {
            void *grown = realloc(replacements, (replacement_count + 1u) * sizeof *replacements);
            if (grown == NULL) continue;
            replacements = grown;
            ++replacement_count;
        }
        snprintf(replacements[r].key, sizeof replacements[r].key, "%s", changes[c].key);
        snprintf(replacements[r].path, sizeof replacements[r].path, "%s", changes[c].path);
    }
    for (unsigned r = 0u; r < replacement_count; ++r) {
        HostFile file;
        if (!host_open(&file, replacements[r].path)) continue;
        digest = digest_file(digest, replacements[r].key, "", &file);
        host_close(&file);
        fprintf(stderr, "recomp mods: %s replaced\n", replacements[r].key);
    }
    /* Archives, each once. */
    for (unsigned c = 0u; c < change_count; ++c) {
        if (changes[c].entry[0] == '\0') continue;
        bool done = false;
        for (unsigned d = 0u; d < c && !done; ++d) done = changes[d].entry[0] != '\0' && strcmp(changes[d].key, changes[c].key) == 0;
        if (!done) build_archive(disc_root, changes[c].key);
    }
    for (unsigned r = 0u; r < reserved_count; ++r) {
        bool done = false;
        for (unsigned i = 0u; i < overlay_count && !done; ++i) done = strcmp(overlays[i]->key, reserved[r].key) == 0;
        if (!done) build_archive(disc_root, reserved[r].key);
    }
    for (unsigned i = 0u; i < overlay_count; ++i) {
        /* Mods in a reserved archive: its slots are part of what both games read. */
        if (overlays[i]->slots != NULL && overlays[i]->replaced != 0u) {
            const uint64_t slot = reserved_slot(overlays[i]->key);
            digest = fnv(digest, &slot, sizeof slot);
        }
        for (unsigned p = 0u; p < overlays[i]->count; ++p)
            if (overlays[i]->pieces[p].kind == PIECE_FILE)
                digest = digest_file(digest, overlays[i]->key, "entry", overlays[i]->pieces[p].file);
            else if (overlays[i]->pieces[p].kind == PIECE_SLOT &&
                     overlays[i]->slots[overlays[i]->pieces[p].source].original->file != NULL)
                digest = digest_file(digest, overlays[i]->key, "entry",
                    overlays[i]->slots[overlays[i]->pieces[p].source].original->file);
    }
    for (unsigned i = 0u; i < list.count; ++i) free(list.names[i]);
    free(list.names);
    free(changes);
    changes = NULL;
    change_count = change_capacity = 0u;
    if (replacement_count == 0u && overlay_count == 0u) {
        mods_on = 0u;
        digest = 0u;
        return false;
    }
    if (mods_on == 0u) digest = 0u;
    if (mods_on != 0u) fprintf(stderr, "recomp mods: %u mods on, digest %016llx\n", mods_on, (unsigned long long)digest);
    return true;
}

void recomp_mod_loader_shutdown(void)
{
    for (unsigned i = 0u; i < overlay_count; ++i) {
        free(overlays[i]->pieces);
        free(overlays[i]->header);
        free(overlays[i]->names);
        free(overlays[i]->slots);
        free(overlays[i]);
    }
    for (unsigned i = 0u; i < file_count; ++i) {
        host_close(files[i]);
        free(files[i]);
    }
    for (unsigned i = 0u; i < source_count; ++i) free(sources[i]);
    free(sources);
    sources = NULL;
    source_count = 0u;
    free(overlays);
    free(files);
    free(replacements);
    free(changes);
    overlays = NULL;
    files = NULL;
    replacements = NULL;
    changes = NULL;
    overlay_count = file_count = replacement_count = change_count = change_capacity = 0u;
    mods_on = 0u;
    digest = 0u;
}

unsigned recomp_mod_loader_count(void) { return mods_on; }
bool recomp_mod_loader_active(void) { return overlay_count != 0u || replacement_count != 0u; }
uint64_t recomp_mod_loader_digest(void) { return digest; }

const char *recomp_mod_replacement(const char *key)
{
    for (unsigned r = 0u; key != NULL && r < replacement_count; ++r)
        if (strcmp(replacements[r].key, key) == 0) return replacements[r].path;
    return NULL;
}

const RecompModOverlay *recomp_mod_overlay(const char *key, uint64_t real_size)
{
    for (unsigned i = 0u; key != NULL && i < overlay_count; ++i)
        if (strcmp(overlays[i]->key, key) == 0) {
            /* An archive reserved only for the Jukebox is the disc's own while slots are off (online). */
            if (!slots_enabled && overlays[i]->slots != NULL && overlays[i]->replaced == 0u) return NULL;
            return overlays[i]->base_size == real_size ? overlays[i] : NULL;
        }
    return NULL;
}

void recomp_mod_loader_enable_slots(bool enabled) { slots_enabled = enabled; }

uint64_t recomp_mod_overlay_size(const RecompModOverlay *overlay)
{
    return overlay != NULL ? overlay->size : 0u;
}

bool recomp_mod_overlay_read(const RecompModOverlay *o, RecompModBaseRead base, void *context,
    uint64_t offset, void *buffer, uint32_t length, uint32_t *read)
{
    *read = 0u;
    if (offset >= o->size) return true;
    if (length > o->size - offset) length = (uint32_t)(o->size - offset);
    /* The piece holding offset: pieces are in order and back to back. */
    unsigned lo = 0u, hi = o->count;
    while (hi - lo > 1u) {
        const unsigned mid = (lo + hi) / 2u;
        if (o->pieces[mid].start <= offset) lo = mid;
        else hi = mid;
    }
    uint8_t *out = buffer;
    for (unsigned p = lo; p < o->count && *read < length; ++p) {
        const Piece *piece = &o->pieces[p];
        const uint64_t at = offset + *read - piece->start;
        uint32_t want = length - *read, got = 0u;
        if (want > piece->length - at) want = (uint32_t)(piece->length - at);
        switch (piece->kind) {
        case PIECE_BYTES: memcpy(out + *read, piece->bytes + at, want); got = want; break;
        case PIECE_ZERO: memset(out + *read, 0, want); got = want; break;
        case PIECE_BASE:
            if (!base(context, piece->source + at, out + *read, want, &got)) return false;
            break;
        case PIECE_FILE:
            if (!host_read(piece->file, piece->source + at, out + *read, want, &got)) return false;
            break;
        case PIECE_SLOT: {
            Slot *slot = &o->slots[piece->source];
            if (at == 0u) {
                /* A track starting: what plays now, or this once (the hook's choice). */
                const RecompModSource *next = slot->wanted;
                if (recomp_mod_entry_start != NULL) {
                    const RecompModSource *once = recomp_mod_entry_start(o->key, (unsigned)piece->source, next);
                    if (once != NULL && once->size <= piece->length) next = once;
                }
                slot->playing = next;
            }
            const RecompModSource *source = slot->playing;
            uint32_t part = at < source->size ? (uint32_t)(source->size - at < want ? source->size - at : want) : 0u;
            got = 0u;
            if (part != 0u && !(source->file != NULL
                    ? host_read(source->file, source->offset + at, out + *read, part, &got)
                    : base(context, source->offset + at, out + *read, part, &got))) return false;
            if (got < want) memset(out + *read + got, 0, want - got);
            got = want;
            break;
        }
        }
        if (got < want) memset(out + *read + got, 0, want - got); /* a file that shrank reads as zeros */
        *read += want;
    }
    return true;
}

bool recomp_mod_key(const char *disc_root, const char *host_path, char *key, size_t key_size)
{
    if (disc_root == NULL || host_path == NULL || key_size == 0u) return false;
    char root[PATH_SIZE], path[PATH_SIZE];
    snprintf(root, sizeof root, "%s", disc_root);
    snprintf(path, sizeof path, "%s", host_path);
    for (char *p = root; *p != '\0'; ++p) *p = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
    for (char *p = path; *p != '\0'; ++p) *p = *p == '\\' ? '/' : (char)tolower((unsigned char)*p);
    size_t n = strlen(root);
    while (n > 0u && root[n - 1u] == '/') root[--n] = '\0';
    /* The game's Z: cache holds copies of three archives from D:\DOA2. */
    const char *cache = strstr(path, "/.recomp-storage/partition5/");
    if (cache != NULL) {
        const char *name = cache + strlen("/.recomp-storage/partition5/");
        if (strchr(name, '/') != NULL || name[0] == '\0') return false;
        snprintf(key, key_size, "doa2/%s", name);
        return true;
    }
    if (strncmp(path, root, n) != 0 || path[n] != '/') return false;
    if (strncmp(path + n + 1u, ".recomp-storage", 15u) == 0) return false;
    snprintf(key, key_size, "%s", path + n + 1u);
    return true;
}

/* ---- Swappable entries (the Jukebox's assignments) ------------------------ */

void recomp_mod_loader_reserve(const char *key, uint64_t slot_size)
{
    if (key == NULL) { reserved_count = 0u; return; }
    for (unsigned i = 0u; i < reserved_count; ++i)
        if (strcmp(reserved[i].key, key) == 0) { if (slot_size > reserved[i].slot) reserved[i].slot = slot_size; return; }
    if (reserved_count == sizeof reserved / sizeof reserved[0]) return;
    snprintf(reserved[reserved_count].key, sizeof reserved[0].key, "%s", key);
    reserved[reserved_count++].slot = slot_size;
}

static RecompModOverlay *overlay_of(const char *key)
{
    for (unsigned i = 0u; key != NULL && i < overlay_count; ++i)
        if (strcmp(overlays[i]->key, key) == 0) return overlays[i];
    return NULL;
}

const RecompModSource *recomp_mod_source(const char *path, uint64_t offset, uint64_t size)
{
    for (unsigned i = 0u; i < source_count; ++i) {
        const RecompModSource *s = sources[i];
        if (s->file != NULL && strcmp(s->file->path, path) == 0 && s->offset == offset && (size == 0u || s->size == size))
            return s;
    }
    const HostFile *file = NULL;
    for (unsigned i = 0u; i < file_count && file == NULL; ++i)
        if (strcmp(files[i]->path, path) == 0) file = files[i];
    if (file == NULL) file = open_file(path);
    if (file == NULL || offset >= file->size) return NULL;
    if (size == 0u || size > file->size - offset) size = file->size - offset;
    return new_source(file, offset, size);
}

int recomp_mod_entry_index(const char *key, const char *name)
{
    const RecompModOverlay *o = overlay_of(key);
    if (o == NULL || o->names == NULL || name == NULL) return -1;
    for (uint32_t i = 0u; i < o->entries; ++i)
        if (strncmp((const char *)o->names + (size_t)i * NAME_RECORD, name, 32u) == 0) return (int)i;
    return -1;
}

const char *recomp_mod_entry_name(const char *key, unsigned index)
{
    const RecompModOverlay *o = overlay_of(key);
    static char name[33];
    if (o == NULL || o->names == NULL || index >= o->entries) return NULL;
    memcpy(name, o->names + (size_t)index * NAME_RECORD, 32u);
    name[32] = '\0';
    return name;
}

uint64_t recomp_mod_slot_size(const char *key, unsigned index)
{
    const RecompModOverlay *o = overlay_of(key);
    if (o == NULL || o->slots == NULL || index >= o->entries) return 0u;
    for (unsigned p = 0u; p < o->count; ++p)
        if (o->pieces[p].kind == PIECE_SLOT && o->pieces[p].source == index) return o->pieces[p].length;
    return 0u;
}

const RecompModSource *recomp_mod_original(const char *key, unsigned index)
{
    const RecompModOverlay *o = overlay_of(key);
    return o != NULL && o->slots != NULL && index < o->entries ? o->slots[index].original : NULL;
}

bool recomp_mod_assign(const char *key, unsigned index, const RecompModSource *source)
{
    RecompModOverlay *o = overlay_of(key);
    if (o == NULL || o->slots == NULL || index >= o->entries || o->slots[index].original == NULL) return false;
    if (source == NULL) source = o->slots[index].original;
    if (source->size > recomp_mod_slot_size(key, index)) return false;
    o->slots[index].wanted = source;
    return true;
}
