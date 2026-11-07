/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_OVERLAY_TEXT_H
#define DOA2U_RECOMP_OVERLAY_TEXT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Text the runtime shows over the game: a short panel of lines and a hint
   line along the bottom. The game side fills it once a frame; a presenter
   draws whatever it holds when the frame is presented. Plain data, with no
   knowledge of how or where it is drawn. */
enum {
    RECOMP_OVERLAY_LINES = 8,
    RECOMP_OVERLAY_LINE_SIZE = 96,
    RECOMP_OVERLAY_HINT_SIZE = 320,
};

typedef struct RecompOverlayText {
    char title[RECOMP_OVERLAY_LINE_SIZE];
    char line[RECOMP_OVERLAY_LINES][RECOMP_OVERLAY_LINE_SIZE];
    unsigned count;
    char hint[RECOMP_OVERLAY_HINT_SIZE];
} RecompOverlayText;

/* Empties the overlay; an empty overlay draws nothing. */
void recomp_overlay_clear(RecompOverlayText *text);
void recomp_overlay_set_title(RecompOverlayText *text, const char *title);
/* Adds a printf-formatted line; lines past RECOMP_OVERLAY_LINES are dropped
   and long lines are cut. False when the line was dropped. */
bool recomp_overlay_add_line(RecompOverlayText *text, const char *format, ...);
void recomp_overlay_set_hint(RecompOverlayText *text, const char *hint);
bool recomp_overlay_empty(const RecompOverlayText *text);

/* Each source keeps its own overlay, so one clearing its text leaves the
   other's. The presenter draws the first non-empty one in this order: the
   quit prompt covers the free camera's panel until it closes. */
typedef enum RecompOverlaySource {
    RECOMP_OVERLAY_QUIT_PROMPT,
    RECOMP_OVERLAY_FREE_CAMERA,
    RECOMP_OVERLAY_SOURCES,
} RecompOverlaySource;

/* The game side hands a source's finished overlay over; a presenter,
   possibly on its own thread, takes a copy of the one to draw. */
void recomp_overlay_publish(RecompOverlaySource source, const RecompOverlayText *text);
void recomp_overlay_take(RecompOverlayText *text);

#ifdef __cplusplus
}
#endif

#endif
