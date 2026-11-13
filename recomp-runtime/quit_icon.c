/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "quit_icon.h"

#include <math.h>
#include <stdbool.h>

/* The Quit Game picture, drawn in the style of the main menu's mode pictures:
   gradient white-to-grey shapes with a 3 px outline of the pictures' own
   grey 51, on a plate of that grey, with no shadow (the pictures' only
   partial alphas are their outlines' edges). An open door swings out of a
   dark doorway and an arrow
   leaves through it. Shapes are in 128x128 picture units, drawn back to front;
   each pixel averages 4x4 samples. */

typedef struct Point { float x, y; } Point;

#define OUTLINE 51.0f

enum { SHAPE_POLYGON, SHAPE_CIRCLE };
typedef struct Shape {
    int kind;
    Point points[8];
    int count;           /* polygon corners; a circle's centre is points[0] */
    float radius;        /* circle */
    float outline;       /* black outline width; 0 for none */
    float top, bottom;   /* grey at the shape's top and bottom (0-255) */
} Shape;

static const Shape SHAPES[] = {
    /* The door frame: a plate of the outline grey, as behind the tools of Options. */
    {SHAPE_POLYGON, {{34, 10}, {94, 10}, {94, 116}, {34, 116}}, 4, 0, 0, 51, 51},
    /* The doorway. */
    {SHAPE_POLYGON, {{43, 19}, {85, 19}, {85, 116}, {43, 116}}, 4, 0, 0, 22, 14},
    /* The open door, hinged at the doorway's left and swung towards us. */
    {SHAPE_POLYGON, {{43, 19}, {22, 9}, {22, 124}, {43, 116}}, 4, 0, 3.0f, 250, 140},
    /* Its knob. */
    {SHAPE_CIRCLE, {{28, 70}}, 0, 4.0f, 0, 51, 51},
    /* The arrow leaving through the doorway. */
    {SHAPE_POLYGON, {{54, 55}, {90, 55}, {90, 40}, {118, 66}, {90, 92}, {90, 77}, {54, 77}}, 7, 0, 3.0f, 252, 150},
};

static float segment_distance(Point p, Point a, Point b)
{
    const float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
    float t = length > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    const float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return sqrtf(ex * ex + ey * ey);
}

static bool polygon_inside(const Shape *s, Point p)
{
    bool inside = false;
    for (int i = 0, j = s->count - 1; i < s->count; j = i++) {
        const Point a = s->points[i], b = s->points[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

/* 2 inside, 1 on the outline, 0 outside. */
static int shape_hit(const Shape *s, Point p)
{
    if (s->kind == SHAPE_CIRCLE) {
        const float dx = p.x - s->points[0].x, dy = p.y - s->points[0].y, d = sqrtf(dx * dx + dy * dy);
        return d <= s->radius ? 2 : d <= s->radius + s->outline ? 1 : 0;
    }
    if (polygon_inside(s, p)) return 2;
    if (s->outline <= 0.0f) return 0;
    for (int i = 0, j = s->count - 1; i < s->count; j = i++)
        if (segment_distance(p, s->points[j], s->points[i]) <= s->outline) return 1;
    return 0;
}

static void shape_span(const Shape *s, float *top, float *bottom)
{
    if (s->kind == SHAPE_CIRCLE) {
        *top = s->points[0].y - s->radius;
        *bottom = s->points[0].y + s->radius;
        return;
    }
    *top = *bottom = s->points[0].y;
    for (int i = 1; i < s->count; ++i) {
        if (s->points[i].y < *top) *top = s->points[i].y;
        if (s->points[i].y > *bottom) *bottom = s->points[i].y;
    }
}

/* The Jukebox picture: a record, its label and hole, and a quaver in front
   of it at the right, in the same greys and outlines. */
static const Shape JUKEBOX_SHAPES[] = {
    /* The record, darker than the other pictures' objects, and its grooves. */
    {SHAPE_CIRCLE, {{54, 60}}, 0, 42.0f, 3.0f, 130, 62},
    {SHAPE_CIRCLE, {{54, 60}}, 0, 33.0f, 0, 96, 52},
    {SHAPE_CIRCLE, {{54, 60}}, 0, 26.0f, 0, 118, 60},
    /* Its label and the hole. */
    {SHAPE_CIRCLE, {{54, 60}}, 0, 14.0f, 3.0f, 250, 170},
    {SHAPE_CIRCLE, {{54, 60}}, 0, 3.5f, 0, 51, 51},
    /* The quaver: stem, flag, head. */
    {SHAPE_POLYGON, {{99, 18}, {106, 18}, {106, 100}, {99, 100}}, 4, 0, 3.0f, 252, 150},
    {SHAPE_POLYGON, {{106, 18}, {117, 27}, {123, 42}, {120, 55}, {114, 41}, {106, 36}}, 6, 0, 3.0f, 252, 170},
    {SHAPE_CIRCLE, {{93, 100}}, 0, 13.0f, 3.0f, 252, 150},
};

static void draw_shapes(const Shape *list, int shapes, float squeeze,
    uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE], uint8_t alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE])
{
    enum { N = QUIT_ICON_SIZE, SS = 4, MAX_SHAPES = 16 };
    const float FIT = 0.88f; /* the design's margin, as the other pictures leave */
    static float shape_alpha[N * N], shape_grey[N * N];
    float tops[MAX_SHAPES], bottoms[MAX_SHAPES];
    if (shapes > MAX_SHAPES) shapes = MAX_SHAPES;
    for (int i = 0; i < shapes; ++i) shape_span(&list[i], &tops[i], &bottoms[i]);
    if (!(squeeze > 0.0f)) squeeze = 1.0f;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float a = 0.0f, g = 0.0f;
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    /* A squeezed picture samples the full design wider about its centre. */
                    const Point p = {64.0f + ((float)x + (sx + 0.5f) / SS - 64.0f) / (squeeze * FIT),
                        66.0f + ((float)y + (sy + 0.5f) / SS - 66.0f) / FIT};
                    for (int i = shapes - 1; i >= 0; --i) {
                        const int hit = shape_hit(&list[i], p);
                        if (hit == 0) continue;
                        float value = OUTLINE; /* the pictures' outline grey */
                        if (hit == 2) {
                            const float span = bottoms[i] - tops[i];
                            const float t = span > 0.0f ? (p.y - tops[i]) / span : 0.0f;
                            value = list[i].top + (list[i].bottom - list[i].top) * (t < 0 ? 0 : t > 1 ? 1 : t);
                        }
                        a += 1.0f;
                        g += value;
                        break;
                    }
                }
            shape_alpha[y * N + x] = a / (SS * SS);
            shape_grey[y * N + x] = a > 0.0f ? g / a : 0.0f;
        }
    for (int i = 0; i < N * N; ++i) {
        const float value = shape_grey[i];
        grey[i] = (uint8_t)lrintf(value < 0 ? 0 : value > 255 ? 255 : value);
        alpha[i] = (uint8_t)lrintf(shape_alpha[i] * 255.0f);
    }
}

void recomp_quit_icon_draw(float squeeze, uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE],
    uint8_t alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE])
{
    draw_shapes(SHAPES, (int)(sizeof SHAPES / sizeof SHAPES[0]), squeeze, grey, alpha);
}

void recomp_jukebox_icon_draw(float squeeze, uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE],
    uint8_t alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE])
{
    draw_shapes(JUKEBOX_SHAPES, (int)(sizeof JUKEBOX_SHAPES / sizeof JUKEBOX_SHAPES[0]), squeeze, grey, alpha);
}
