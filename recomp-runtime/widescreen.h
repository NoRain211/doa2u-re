#ifndef RECOMP_WIDESCREEN_H
#define RECOMP_WIDESCREEN_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* RECOMP_D3D_WIDESCREEN: unset or 0 is the dashboard's 4:3 setting. Any other
   value reports its widescreen flag; "W:H" from 4:3 to 4:1, such as 16:10
   or a monitor's 3440:1440, also gives the screen's shape, and anything else
   means 16:9. Ported from the DOA3 port; the kernel, the runner and both
   presenters read it here so they agree. */
static inline bool recomp_widescreen(unsigned *aspect_w, unsigned *aspect_h)
{
    const char *value = getenv("RECOMP_D3D_WIDESCREEN");
    char *end = NULL;
    unsigned long w = 0ul, h = 0ul;

    if (value == NULL || strcmp(value, "0") == 0) {
        return false;
    }
    w = strtoul(value, &end, 10);
    if (*end == ':') {
        h = strtoul(end + 1, &end, 10);
    }
    if (*end != '\0' || w > 16384ul || h > 16384ul || h == 0ul ||
        w * 3ul < h * 4ul || w > h * 4ul) {
        w = 16ul;
        h = 9ul;
    }
    *aspect_w = (unsigned)w;
    *aspect_h = (unsigned)h;
    return true;
}

/* DOA2U lays its widescreen HUD and menus out for 16:9. On another shape
   (aspect, width over height) a screen-space quad keeps that shape: the 2D
   layer shrinks about the screen center into the largest 16:9 area, so an
   ultrawide screen pillarboxes it and 16:10 letterboxes it. A quad spanning
   the guest target's width (a fade or backdrop) still fills the screen. */
static inline void recomp_widescreen_2d_scale(float aspect, float width, float left, float right,
    float scale[2])
{
    const float wide = 16.0f / 9.0f;

    scale[0] = scale[1] = 1.0f;
    if (left <= 1.0f && right >= width - 1.0f) {
        return;
    }
    if (aspect > wide) {
        scale[0] = wide / aspect;
    } else {
        scale[1] = aspect / wide;
    }
}

#endif
