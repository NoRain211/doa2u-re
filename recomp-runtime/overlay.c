/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "overlay.h"
#include "d3d8_adapter.h"
#include "d3d_draw_model.h"
#include "kernel_abi.h"

#include <string.h>
#include <stdlib.h>

enum { MAX_QUADS = 8192u, FVF_XYZRHW_DIFFUSE = 0x44u };

typedef struct OverlayVertex {
    float x, y, z, rhw;
    uint32_t color;
} OverlayVertex;

typedef struct OverlayQuad {
    float x, y, width, height;
    uint32_t color;
} OverlayQuad;

static OverlayQuad quads[MAX_QUADS];
static uint32_t quad_count;
static OverlayVertex vertices[MAX_QUADS * 4u];
static uint16_t indices[MAX_QUADS * 6u];

/* The built-in font for ' ' through '_', then '~': seven rows of five dots
   each, top to bottom, drawn for this runner. */
static const char font[65][42] = {
    ".....|.....|.....|.....|.....|.....|.....", /* space */
    "..#..|..#..|..#..|..#..|..#..|.....|..#..", /* ! */
    ".#.#.|.#.#.|.....|.....|.....|.....|.....", /* " */
    ".#.#.|#####|.#.#.|.#.#.|#####|.#.#.|.....", /* # */
    "..#..|.####|#.#..|.###.|..#.#|####.|..#..", /* $ */
    "##...|##..#|...#.|..#..|.#...|#..##|...##", /* % */
    ".##..|#..#.|#.#..|.#...|#.#.#|#..#.|.##.#", /* & */
    "..#..|..#..|.....|.....|.....|.....|.....", /* ' */
    "...#.|..#..|.#...|.#...|.#...|..#..|...#.", /* ( */
    ".#...|..#..|...#.|...#.|...#.|..#..|.#...", /* ) */
    ".....|..#..|#.#.#|.###.|#.#.#|..#..|.....", /* * */
    ".....|..#..|..#..|#####|..#..|..#..|.....", /* + */
    ".....|.....|.....|.....|.##..|..#..|.#...", /* , */
    ".....|.....|.....|#####|.....|.....|.....", /* - */
    ".....|.....|.....|.....|.....|.##..|.##..", /* . */
    "....#|....#|...#.|..#..|.#...|#....|#....", /* / */
    ".###.|#...#|#..##|#.#.#|##..#|#...#|.###.", /* 0 */
    "..#..|.##..|..#..|..#..|..#..|..#..|.###.", /* 1 */
    ".###.|#...#|....#|...#.|..#..|.#...|#####", /* 2 */
    "#####|...#.|..#..|...#.|....#|#...#|.###.", /* 3 */
    "...#.|..##.|.#.#.|#..#.|#####|...#.|...#.", /* 4 */
    "#####|#....|####.|....#|....#|#...#|.###.", /* 5 */
    "..##.|.#...|#....|####.|#...#|#...#|.###.", /* 6 */
    "#####|....#|...#.|..#..|.#...|.#...|.#...", /* 7 */
    ".###.|#...#|#...#|.###.|#...#|#...#|.###.", /* 8 */
    ".###.|#...#|#...#|.####|....#|...#.|.##..", /* 9 */
    ".....|.##..|.##..|.....|.##..|.##..|.....", /* : */
    ".....|.##..|.##..|.....|.##..|..#..|.#...", /* ; */
    "...#.|..#..|.#...|#....|.#...|..#..|...#.", /* < */
    ".....|.....|#####|.....|#####|.....|.....", /* = */
    ".#...|..#..|...#.|....#|...#.|..#..|.#...", /* > */
    ".###.|#...#|....#|...#.|..#..|.....|..#..", /* ? */
    ".###.|#...#|....#|.##.#|#.#.#|#.#.#|.###.", /* @ */
    ".###.|#...#|#...#|#...#|#####|#...#|#...#", /* A */
    "####.|#...#|#...#|####.|#...#|#...#|####.", /* B */
    ".###.|#...#|#....|#....|#....|#...#|.###.", /* C */
    "###..|#..#.|#...#|#...#|#...#|#..#.|###..", /* D */
    "#####|#....|#....|####.|#....|#....|#####", /* E */
    "#####|#....|#....|####.|#....|#....|#....", /* F */
    ".###.|#...#|#....|#.###|#...#|#...#|.####", /* G */
    "#...#|#...#|#...#|#####|#...#|#...#|#...#", /* H */
    ".###.|..#..|..#..|..#..|..#..|..#..|.###.", /* I */
    "..###|...#.|...#.|...#.|...#.|#..#.|.##..", /* J */
    "#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#", /* K */
    "#....|#....|#....|#....|#....|#....|#####", /* L */
    "#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#", /* M */
    "#...#|#...#|##..#|#.#.#|#..##|#...#|#...#", /* N */
    ".###.|#...#|#...#|#...#|#...#|#...#|.###.", /* O */
    "####.|#...#|#...#|####.|#....|#....|#....", /* P */
    ".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#", /* Q */
    "####.|#...#|#...#|####.|#.#..|#..#.|#...#", /* R */
    ".####|#....|#....|.###.|....#|....#|####.", /* S */
    "#####|..#..|..#..|..#..|..#..|..#..|..#..", /* T */
    "#...#|#...#|#...#|#...#|#...#|#...#|.###.", /* U */
    "#...#|#...#|#...#|#...#|#...#|.#.#.|..#..", /* V */
    "#...#|#...#|#...#|#.#.#|#.#.#|#.#.#|.#.#.", /* W */
    "#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#", /* X */
    "#...#|#...#|#...#|.#.#.|..#..|..#..|..#..", /* Y */
    "#####|....#|...#.|..#..|.#...|#....|#####", /* Z */
    ".###.|.#...|.#...|.#...|.#...|.#...|.###.", /* [ */
    "#....|#....|.#...|..#..|...#.|....#|....#", /* backslash */
    ".###.|...#.|...#.|...#.|...#.|...#.|.###.", /* ] */
    "..#..|.#.#.|#...#|.....|.....|.....|.....", /* ^ */
    ".....|.....|.....|.....|.....|.....|#####", /* _ */
    ".....|.....|.#...|#.#.#|...#.|.....|.....", /* ~ */
};

void recomp_overlay_rect(float x, float y, float width, float height, uint32_t argb)
{
    if (quad_count == MAX_QUADS || width <= 0.0f || height <= 0.0f) return;
    quads[quad_count++] = (OverlayQuad){x, y, width, height, argb};
}

static const char *glyph(unsigned char c)
{
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
    if (c == '~') return font[64];
    return font[c >= 32u && c < 96u ? c - 32u : '?' - 32u];
}

float recomp_overlay_text_width(const char *text, float scale)
{
    const size_t length = strlen(text);
    return length == 0u ? 0.0f : (float)(length * 6u - 1u) * scale;
}

float recomp_overlay_text(float x, float y, float scale, uint32_t argb, const char *text)
{
    for (const unsigned char *c = (const unsigned char *)text; *c != 0u; ++c, x += 6.0f * scale) {
        const char *dots = glyph(*c);
        for (unsigned row = 0u; row < 7u; ++row) {
            const char *line = dots + row * 6u;
            /* One quad per run of lit dots. */
            for (unsigned dot = 0u; dot < 5u;) {
                if (line[dot] != '#') {
                    ++dot;
                    continue;
                }
                unsigned end = dot + 1u;
                while (end < 5u && line[end] == '#') ++end;
                recomp_overlay_rect(x + (float)dot * scale, y + (float)row * scale,
                    (float)(end - dot) * scale, scale, argb);
                dot = end;
            }
        }
    }
    return recomp_overlay_text_width(text, scale);
}

void recomp_overlay_submit(const RecompD3dPresenterTarget *target, float display_aspect)
{
    if (quad_count == 0u) return;
    /* The 4:3 area spans the full height and (4/3) / display_aspect of the width. */
    const float width = (float)target->color.width, height = (float)target->color.height;
    const float sx = width * (4.0f / 3.0f) / display_aspect / 640.0f, sy = height / 480.0f;
    const float left = (width - 640.0f * sx) * 0.5f;
    for (uint32_t i = 0u; i < quad_count; ++i) {
        const OverlayQuad q = quads[i];
        const float x0 = left + q.x * sx, y0 = q.y * sy, x1 = x0 + q.width * sx, y1 = y0 + q.height * sy;
        OverlayVertex *v = vertices + i * 4u;
        v[0] = (OverlayVertex){x0, y0, 0.0f, 1.0f, q.color};
        v[1] = (OverlayVertex){x1, y0, 0.0f, 1.0f, q.color};
        v[2] = (OverlayVertex){x0, y1, 0.0f, 1.0f, q.color};
        v[3] = (OverlayVertex){x1, y1, 0.0f, 1.0f, q.color};
        uint16_t *index = indices + i * 6u;
        const uint16_t first = (uint16_t)(i * 4u);
        index[0] = first;
        index[1] = (uint16_t)(first + 1u);
        index[2] = (uint16_t)(first + 2u);
        index[3] = (uint16_t)(first + 2u);
        index[4] = (uint16_t)(first + 1u);
        index[5] = (uint16_t)(first + 3u);
    }
    RecompD3dPresenterCommand command;
    memset(&command, 0, sizeof command);
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    RecompD3dPresenterDrawCommand *draw = &command.data.draw;
    draw->fvf = FVF_XYZRHW_DIFFUSE;
    draw->primitive_type = RECOMP_D3D_PT_TRIANGLELIST;
    draw->vertex_count = quad_count * 4u;
    draw->index_count = quad_count * 6u;
    draw->triangle_count = quad_count * 2u;
    draw->vertex_stride = sizeof vertices[0];
    draw->vertex_bytes = vertices;
    draw->index_bytes = indices;
    draw->blend.blend_enable = true;
    draw->blend.color_write_mask = 7u;
    draw->blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
    draw->blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
    draw->blend.op = RECOMP_D3D_BLEND_OP_ADD;
    draw->target = *target;
    recomp_d3d8_submit(&command);
    quad_count = 0u;
}

float recomp_overlay_display_aspect(void)
{
    return recomp_kernel_video_flags() & 0x00010000u ? 16.0f / 9.0f : 4.0f / 3.0f;
}
