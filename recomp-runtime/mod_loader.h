/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_MOD_LOADER_H
#define DOA2U_RECOMP_MOD_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mods replace the game's files without touching the disc: each folder in
   mods/ mirrors the disc, and the kernel's file bridge (kernel_file.c) serves
   a mod's file wherever the game reads the disc's.

     mods/<mod>/DOA2/doa2_op.sfd           the whole file
     mods/<mod>/DOA2/bgm.afs/<entry>       one entry of an AFS archive, by its
                                           name in the archive (bgm_doa2_ay.adx)
                                           or its index (10, 10.adx, 10_anything)
     mods/<mod>/mod.ini                    optional: name=, author=, version=

   An entry given as a .wav (8- to 32-bit PCM, mono or stereo, any rate) is
   encoded to ADX once and kept in mods/.cache; it loops where its smpl chunk
   says, or over the whole track if the entry it replaces looped. Folders
   whose name starts with '.' or '_' are off. Mods load in the order of
   mods/order.txt (one folder a line), then by name; a later mod's file wins.

   A replaced entry changes the archive's layout, so the archive is served as
   an overlay: a list of pieces (the original's bytes, a mod's file, a new
   table of contents) that reads as the new archive. The game installs three
   archives to its Z: cache (partition5); a cached copy the size of the
   disc's original gets the same overlay. Overlays never apply to handles
   opened for writing. */

typedef struct RecompModOverlay RecompModOverlay;

/* Reads the mods in mods_root against the disc in disc_root; logs what it
   finds. False (and nothing active) without a mods folder. */
bool recomp_mod_loader_install(const char *mods_root, const char *disc_root);
void recomp_mod_loader_shutdown(void);
/* How many mods are on, and a digest of what they change (0 with none):
   netplay pairs only games that load the same changes. */
unsigned recomp_mod_loader_count(void);
/* Anything to serve: a mod's file or an overlay (the Jukebox's slots count). */
bool recomp_mod_loader_active(void);
uint64_t recomp_mod_loader_digest(void);

/* Keys are disc paths in lower case with '/', no leading slash: doa2/bgm.afs. */
/* A mod's file in place of the whole disc file, or NULL. */
const char *recomp_mod_replacement(const char *key);
/* The overlay for this file, when its size is the original's; else NULL. */
const RecompModOverlay *recomp_mod_overlay(const char *key, uint64_t real_size);
uint64_t recomp_mod_overlay_size(const RecompModOverlay *overlay);

/* Reads from the original file; returns false on a host read error. */
typedef bool (*RecompModBaseRead)(void *context, uint64_t offset, void *buffer, uint32_t length, uint32_t *read);
/* Reads the overlay's bytes; past its end reads short, as a file does. */
bool recomp_mod_overlay_read(const RecompModOverlay *overlay, RecompModBaseRead base, void *context,
    uint64_t offset, void *buffer, uint32_t length, uint32_t *read);

/* ---- Swappable entries ------------------------------------------------

   The Jukebox assigns tracks to the game's music. Before install, a reserved
   archive (doa2/bgm.afs) is built with every entry in a slot of at least
   slot_size bytes that the TOC gives in full; the zeros past a track's end
   are never played (an ADX ends at its end marker). An assignment takes
   effect when the entry's first byte is next read: when its track starts. */
typedef struct RecompModSource RecompModSource;
void recomp_mod_loader_reserve(const char *key, uint64_t slot_size); /* NULL clears */
/* Off (online): an archive reserved only for slots is served as the disc's own, so
   two games with different albums read the same file. Files opened later follow. */
void recomp_mod_loader_enable_slots(bool enabled);
/* A track to assign: part of a file (size 0: to its end); kept until shutdown. */
const RecompModSource *recomp_mod_source(const char *path, uint64_t offset, uint64_t size);
const RecompModSource *recomp_mod_original(const char *key, unsigned index);
/* NULL puts the entry's own track back; false if it does not fit or is not reserved. */
bool recomp_mod_assign(const char *key, unsigned index, const RecompModSource *source);
int recomp_mod_entry_index(const char *key, const char *name);       /* by the archive's names; -1 */
const char *recomp_mod_entry_name(const char *key, unsigned index); /* NULL if none */
uint64_t recomp_mod_slot_size(const char *key, unsigned index);
/* Called, on the thread reading, as an entry's track starts: a source to
   play this once instead of the assigned one, or NULL. */
extern const RecompModSource *(*recomp_mod_entry_start)(const char *key, unsigned index,
    const RecompModSource *wanted);

/* For tests and tools: the key of a host path under the disc root or the
   game's Z: cache, or false. */
bool recomp_mod_key(const char *disc_root, const char *host_path, char *key, size_t key_size);

#ifdef __cplusplus
}
#endif

#endif
