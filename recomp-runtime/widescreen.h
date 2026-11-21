#ifndef RECOMP_WIDESCREEN_H
#define RECOMP_WIDESCREEN_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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

/* A screen-space quad spanning the guest target's width: a fade or backdrop. */
static inline bool recomp_widescreen_2d_spans(float width, float left, float right)
{
    return left <= 1.0f && right >= width - 1.0f;
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
    if (recomp_widescreen_2d_spans(width, left, right)) {
        return;
    }
    if (aspect > wide) {
        scale[0] = wide / aspect;
    } else {
        scale[1] = aspect / wide;
    }
}

/* Whether a pretransformed draw into the screen (width x height guest
   pixels) is part of that HUD layer, with bounds = left, top, right, bottom
   of the vertices it uses (an indexed draw may share its buffer). A quad
   spanning the width is not, nor a screen effect laid over the whole height
   from one of the game's own render targets (Danger Zone's glow), even drawn
   narrower than the target; a picture in a menu panel is shorter. Each vertex
   starts with its screen x and y. */
static inline bool recomp_widescreen_2d_layer(const void *vertex_bytes, uint32_t stride, uint32_t vertex_count,
    const uint16_t *indices, uint32_t index_count, float width, float height, bool render_target,
    float bounds[4])
{
    const uint32_t count = indices != NULL ? index_count : vertex_count;

    bounds[0] = bounds[1] = INFINITY;
    bounds[2] = bounds[3] = -INFINITY;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t vertex = indices != NULL ? indices[i] : i;
        float xy[2];
        if (vertex >= vertex_count) continue;
        memcpy(xy, (const uint8_t *)vertex_bytes + (size_t)vertex * stride, sizeof xy);
        if (!isfinite(xy[0]) || !isfinite(xy[1])) continue;
        bounds[0] = fminf(bounds[0], xy[0]);
        bounds[1] = fminf(bounds[1], xy[1]);
        bounds[2] = fmaxf(bounds[2], xy[0]);
        bounds[3] = fmaxf(bounds[3], xy[1]);
    }
    if (render_target && bounds[1] <= 1.0f && bounds[3] >= height - 1.0f) return false;
    return !recomp_widescreen_2d_spans(width, bounds[0], bounds[2]);
}

#endif
