#ifndef DOA2U_RECOMP_OVERLAY_H
#define DOA2U_RECOMP_OVERLAY_H

#include "d3d_presenter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Screens the runner draws over the game: rectangles and text in a 640 x 480
   space kept 4:3 and centered on the display, sent as one untextured draw
   before Present. Coordinates may run past the edges to cover a wide display. */
void recomp_overlay_rect(float x, float y, float width, float height, uint32_t argb);
/* Text in the built-in 5 x 7 font, scale pixels per dot; returns its width.
   Lower case draws as upper case and anything else outside ASCII as '?'. */
float recomp_overlay_text(float x, float y, float scale, uint32_t argb, const char *text);
float recomp_overlay_text_width(const char *text, float scale);
/* Draws the queued shapes into target, shown at display_aspect (width / height),
   and empties the queue. */
void recomp_overlay_submit(const RecompD3dPresenterTarget *target, float display_aspect);

#ifdef __cplusplus
}
#endif

#endif
