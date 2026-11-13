/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_JUKEBOX_H
#define DOA2U_RECOMP_JUKEBOX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Jukebox plays DOA2U's music from its disc and the albums
   tools/import_soundtrack.py imports from the player's own DOA3 and DOA4
   (<private>/jukebox/<album>/tracks.txt), and assigns any of them to the
   game's own music: each entry of the music table (0x6a0b2c), which the game
   plays by file name from DOA2/bgm.afs. The mod loader serves bgm.afs with
   every entry in a slot big enough for any track (mod_loader.h), so an
   assignment is a new source for that slot from the track's next start.
   Assignments are kept in <private>/jukebox/assignments.txt.

   Its screen is a page of the game's own Options menu (pc_settings.c), which
   the main menu's JUKEBOX entry opens (menu_quit.c). All calls but the mod
   loader's start hook are on the game thread. */

/* Once, before the mod loader installs: reads the albums and returns the
   largest track, which every bgm.afs slot must hold. */
uint64_t recomp_jukebox_prepare(const char *disc_root, const char *private_dir);
/* Each boot, after the mod loader: applies the saved assignments. */
void recomp_jukebox_install(bool online);

/* The main menu's entry asks for the page; the Options menu opens it at its
   next update and takes the request. */
void recomp_jukebox_request_page(void);
bool recomp_jukebox_take_request(void);

/* The page's rows: Album, Track, Play Mode, Assign To, Assigned. */
enum { RECOMP_JUKEBOX_ROWS = 5 };
void recomp_jukebox_page_opened(void);
void recomp_jukebox_page_closed(void);
const char *recomp_jukebox_label(unsigned row);
unsigned recomp_jukebox_choices(unsigned row);   /* left/right steps through these */
unsigned recomp_jukebox_choice(unsigned row);
void recomp_jukebox_choose(unsigned row, unsigned choice);
void recomp_jukebox_confirm(unsigned row);
/* ASCII, to show beside the row's label; and the help line for a row. */
const char *recomp_jukebox_value(unsigned row);
const char *recomp_jukebox_help(unsigned row);

/* Once a frame: feeds the output; with private/claude-scratch/probe present,
   also saves the menu's word art there (for its lettering). */
void recomp_jukebox_pump(void);
/* In a main menu hook (game context): with the probe folder present, saves
   the game's message text there once. */
void recomp_jukebox_probe_messages(void);

/* For tests: a DOA2U music file's name in the Jukebox, or NULL to leave it out. */
const char *recomp_jukebox_doa2u_name(const char *file);

#ifdef __cplusplus
}
#endif

#endif
