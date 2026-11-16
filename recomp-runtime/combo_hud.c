/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "combo_hud.h"
#include "hit_voice.h"
#include "d3d8_resource_adapter.h"
#include "d3d_texture_model.h"
#include "kernel_abi.h"
#include "runner_assets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Battle modes 9 and 10 are Sparring and its Training: the fight HUD (0xba120)
   draws Sparring's overlay (0x291900) there, and in every other mode the
   combo counter (0xb57f0) then the hit popups (0xb8ae0, 0xb86b0, 0xb8980,
   0xb8810).

   Combo counter, per attacking side s: hits at 0x8a212c + 16 s, a frame
   timer at 0x8a2138 + 16 s (64 when a hit lands). It draws the hit digits
   (sprites 0x100a7 + digit) and HIT COMBO ! with 0x75a50 at y 0x58 +
   g_hud_y (0x8a2f4c) from origin (0, 0x220), side 1 from x 491; the units
   digit is at x 48, after a tens digit at x 67 on side 0. The hit popups
   stack from y 0x70 + g_hud_y, 19 apart. The damage line goes under the
   counter and the popups move down by its height while it shows.

   The damage side s has dealt in its combo is 0xdf4a04 + 4 (1 - s), added
   while the opponent is comboed and cleared when the combo ends (0x119420).

   Critical stun (0x114c50, 0x1124f0), per side d: 0xdf38c0 + d is 1 while it
   lasts, 0xdf3c70 + 2 d is the damage it allows (40, 60, or the opening
   hit's damage + 20) and 0xdf38d8 + 2 d the damage taken in it; past the
   allowance the stun breaks. Sparring's bar (0x28e1a0) marks what is left
   as a darker segment at the bar's draining end.

   Life bars (0xb36a0): the fill is sprite 0x1001c (texture page 2 of group
   1, an image of its own), drawn through 0xb11e0 from x (record + 0xc,
   1/16 px) at width record + 0x14 = pixels / 8, mirrored for side 0
   (0x8a09a0 = 1, side 1 0xff). 259 px are full life (0x87d498 + 2 d). */
enum {
    BATTLE_MODE = 0x0087cfa0u,
    HUD_Y = 0x008a2f4cu,
    COUNTER_HITS = 0x008a212cu,
    COUNTER_TIMER = 0x008a2138u,
    COUNTER_STRIDE = 0x10u,
    COMBO_DAMAGE = 0x00df4a04u,
    LIFE = 0x0087ae54u,              /* + 0x94 d: side d's life (16-bit) */
    SIDE_STRIDE = 0x94u,
    SINGLE_FRAMES = 64u,             /* as long as the counter shows after a hit */
    MAX_LIFE = 0x0087d498u,
    CRITICAL = 0x00df38c0u,
    CRITICAL_ALLOWS = 0x00df3c70u,
    CRITICAL_TAKEN = 0x00df38d8u,
    SPRITE_FLIP = 0x008a09a0u,
    DEMO_FLAG = 0x0087ae02u,
    TEXTURE_PAGES = 0x0067ce78u,     /* per group: 8-byte entries, texture first */

    ATTACK_DAMAGE = 0x001159f0u,     /* adds an attack's damage to a side's pending damage */
    POPUPS_FIRST = 0x000b8ae0u,
    POPUPS_LAST = 0x000b8810u,
    SPRITE_DRAW = 0x000b11e0u,
    SPRITE = 0x00075a50u,            /* cdecl (record {id, packed position}) */
    UI_RESET = 0x0006baa0u,
    ORIGIN_X = 0x008a0980u,
    ORIGIN_Y = 0x008a0982u,
    SPRITE_SCALE = 0x008a098cu,
    SPRITE_MODE = 0x008a0999u,
    COUNTER_SCALE = 0x00415994u,     /* 0.88 */

    DIGIT_SPRITE = 0x100a7u,
    LIFE_SPRITE = 0x1001cu,
    LIFE_PAGE_GROUP = 1u,
    LIFE_PAGE = 2u,
    LIFE_PIXELS = 259u,
    DIGIT_STEP = 19u,
    LINE = 22u,                      /* the damage line's height */
};

extern void sub_001159F0(void);
extern void sub_000B8AE0(void);
extern void sub_000B8810(void);
extern void sub_000B11E0(void);

static bool online_session;
static uint32_t life_rgb;            /* 0: the game's own colours */
static bool hud_moved;               /* the popups are moved down this frame */
static unsigned latched_damage[2];
static unsigned latched_hits[2];
/* A lone hit: the game counts no combo for it, so its damage is what the
   attack routine (0x1159f0) added to the defender's pending damage, shown
   for SINGLE_FRAMES on the counter's row. Stage hazards, walls and floors
   take life through 0x107170 instead, and do not show. */
static unsigned attack_damage[2], single_damage[2], single_frames[2];

static bool fight_hud(void)
{
    const uint8_t mode = *recomp_memory(BATTLE_MODE, 1u);
    return !online_session && mode != 9u && mode != 10u;
}

static unsigned counter_hits(unsigned side)
{
    return *recomp_memory_u32(COUNTER_HITS + COUNTER_STRIDE * side);
}

static bool counter_shown(unsigned side)
{
    return (int32_t)*recomp_memory_u32(COUNTER_TIMER + COUNTER_STRIDE * side) > 0 && counter_hits(side) >= 2u;
}

static bool damage_shown(unsigned side)
{
    return counter_shown(side) || single_frames[side] > 0u;
}

static void note_attack_damage(void)
{
    uint16_t before[2];
    for (unsigned d = 0u; d < 2u; ++d) before[d] = *recomp_memory_u16(LIFE + SIDE_STRIDE * d + 2u);
    sub_001159F0();
    /* Only where the line is drawn: Sparring and Training never clear it. */
    if (!fight_hud()) return;
    for (unsigned d = 0u; d < 2u; ++d) {
        const uint16_t after = *recomp_memory_u16(LIFE + SIDE_STRIDE * d + 2u);
        if (after > before[d]) attack_damage[d] += after - before[d];
    }
}

/* Once a frame: an attack's damage to side d is the other side's lone-hit
   damage while that side has no combo running (from its second hit the
   game's own combo damage takes over). */
static void watch_attacks(void)
{
    for (unsigned d = 0u; d < 2u; ++d) {
        const unsigned a = 1u - d;
        if (attack_damage[d] > 0u && !counter_shown(a)) {
            single_damage[a] = attack_damage[d];
            single_frames[a] = SINGLE_FRAMES;
        } else if (single_frames[a] > 0u) {
            --single_frames[a];
        }
        if (counter_shown(a)) single_frames[a] = 0u;
        attack_damage[d] = 0u;
    }
}

/* The damage side s has dealt: the opponent's combo damage, kept after the
   combo ends while the line still shows. */
static unsigned combo_damage(unsigned side)
{
    const unsigned hits = counter_hits(side);
    if (!damage_shown(side) || hits < latched_hits[side]) latched_damage[side] = 0u;
    latched_hits[side] = damage_shown(side) ? hits : 0u;
    if (!counter_shown(side)) return single_frames[side] > 0u ? single_damage[side] : 0u;
    const unsigned now = *recomp_memory_u16(COMBO_DAMAGE + 4u * (1u - side));
    if (now > 0u) latched_damage[side] = now;
    return latched_damage[side];
}

/* ---- Guest calls ---------------------------------------------------- */

static void put16(uint32_t at, uint16_t value) { memcpy(recomp_memory(at, 2u), &value, 2u); }
static void put32(uint32_t at, uint32_t value) { memcpy(recomp_memory(at, 4u), &value, 4u); }

static uint32_t call(uint32_t address, const uint32_t *args, unsigned count)
{
    kernel_call_guest(address, args, count);
    return recomp_runtime.registers.eax;
}

/* One sprite as the counter draws it: x, y from the origin set below. */
static void draw_sprite(uint32_t record, uint32_t id, unsigned x, unsigned y)
{
    put32(record, id);
    put32(record + 4u, 0x1000000u | (y & 0xfffu) << 12 | (x & 0xfffu));
    const uint32_t args[1] = {record};
    call(SPRITE, args, 1u);
}

/* DAMAGE in the lettering of the fight's popups (CRITICAL, COUNTER, HOLD...):
   each popup word is a soft fill (pages 52, 54, 56) and an outline drawn
   under it (the next page, same rectangle). The letters are cut from both,
   D of HOLD, A of CRITICAL, M of COMBO, C and E of CLOSE, the G being the
   C with the E's middle arm as its bar and a piece of the E's stem as its
   spur; the fill is white, as the counter's HIT COMBO is. Pieces are in
   texels of their word; an add-on (letter 0) goes dx into the letter before.
   Made at run time from the game's own art, once a fight has loaded it. */
typedef struct LabelPiece {
    uint32_t fill, outline;
    int16_t from, to;
    int8_t top, bottom, dx, letter;
} LabelPiece;
static const LabelPiece DAMAGE_PIECES[] = {
    {0x1017du, 0x10182u, 42, 55, 0, 15, 0, 1},   /* D of HOLD */
    {0x10165u, 0x1016bu, 61, 75, 0, 15, 0, 1},   /* A of CRITICAL */
    {0x10173u, 0x10177u, 29, 43, 0, 15, 0, 1},   /* M of COMBO */
    {0x10165u, 0x1016bu, 61, 75, 0, 15, 0, 1},   /* A */
    {0x10166u, 0x1016cu, 1, 13, 0, 15, 0, 1},    /* C of CLOSE: the G */
    {0x10166u, 0x1016cu, 58, 62, 7, 9, 5, 0},    /*   its bar, the E's middle arm */
    {0x10166u, 0x1016cu, 52, 56, 7, 13, 8, 0},   /*   its spur, the E's stem */
    {0x10166u, 0x1016cu, 53, 64, 0, 15, 0, 1},   /* E of CLOSE */
};
enum { LABEL_W = 128, LABEL_H = 16, WORD_H = 16, LEVELS = 16 };
static uint32_t label_palette, label_colours;
static bool label_made;
static int label_width;

static float coverage(const RecompGameSprite *sprite, int x, int y)
{
    return (float)(recomp_game_sprite_colour(sprite, recomp_game_sprite_index(sprite, x, y)) >> 24) / 255.0f;
}

static bool make_damage_label(void)
{
    static float fill[LABEL_H][LABEL_W], outline[LABEL_H][LABEL_W];
    static uint8_t image[LABEL_H][LABEL_W];
    memset(fill, 0, sizeof fill);
    memset(outline, 0, sizeof outline);
    int pen = 1, letter_pen = 1;
    for (size_t p = 0u; p < sizeof DAMAGE_PIECES / sizeof DAMAGE_PIECES[0]; ++p) {
        const LabelPiece *piece = &DAMAGE_PIECES[p];
        RecompGameSprite f, o;
        if (!recomp_game_sprite(piece->fill, WORD_H, &f) || !recomp_game_sprite(piece->outline, WORD_H, &o)) return false;
        if (piece->to > f.right - f.left || piece->to > o.right - o.left) return false;
        const int at = piece->letter ? pen : letter_pen + piece->dx;
        for (int y = piece->top; y < piece->bottom; ++y)
            for (int u = piece->from; u < piece->to; ++u) {
                const int x = at + u - piece->from;
                if (x < 0 || x >= LABEL_W) continue;
                const float pf = coverage(&f, u, y), po = coverage(&o, u, y);
                if (piece->letter) {
                    if (pf > fill[y][x]) fill[y][x] = pf;
                    if (po > outline[y][x]) outline[y][x] = po;
                } else {
                    /* An add-on keeps the letter's outline where the letter is solid, and
                       adds its own outline only outside both fills. */
                    const float before = fill[y][x];
                    if (pf > fill[y][x]) fill[y][x] = pf;
                    if (before <= 0.6f && pf <= 0.6f && po > outline[y][x]) outline[y][x] = po;
                }
            }
        if (piece->letter) {
            letter_pen = pen;
            pen += piece->to - piece->from + 1;
        }
    }
    for (int y = 0; y < LABEL_H; ++y)
        for (int x = 0; x < LABEL_W; ++x) {
            /* The popups' dark outline speckles at this size; the counter's HIT COMBO
               above is plain white, so the label is the fill alone. */
            const int a = (int)lrintf(fill[y][x] * (LEVELS - 1)), b = 0;
            image[y][x] = (uint8_t)(a * LEVELS + b);
        }
    recomp_runner_sprite_pixels(RECOMP_RUNNER_SPRITE_DAMAGE_LABEL, &image[0][0]);
    label_width = pen < LABEL_W ? pen : LABEL_W;
    recomp_runner_sprite_size(RECOMP_RUNNER_SPRITE_DAMAGE_LABEL, label_width, LABEL_H - 1);
    fprintf(stderr, "recomp combo hud: DAMAGE label made, %d pixels wide\n", label_width);
    return true;
}

/* Index a * 16 + b: white fill coverage a over a black outline of coverage b. */
static void damage_label_install(void)
{
    label_made = false;
    label_palette = recomp_runner_palette(&label_colours);
    if (label_palette == 0u) return;
    for (unsigned a = 0u; a < LEVELS; ++a)
        for (unsigned b = 0u; b < LEVELS; ++b) {
            const float f = a / (float)(LEVELS - 1), o = b / (float)(LEVELS - 1);
            const float alpha = f + o * (1.0f - f);
            const unsigned grey = alpha > 0.0f ? (unsigned)lrintf(255.0f * f / alpha) : 0u;
            const unsigned A = (unsigned)lrintf(255.0f * alpha);
            *recomp_memory_u32(label_colours + 4u * (a * LEVELS + b)) = A << 24 | grey << 16 | grey << 8 | grey;
        }
    recomp_runner_sprite(RECOMP_RUNNER_SPRITE_DAMAGE_LABEL, 7u, 4u, label_palette);
}

/* "NN DAMAGE" for side s: under the counter, its first digit under the
   counter's first, from the second hit; on the counter's own row on the
   first hit, when the game draws no counter. */
static void draw_damage(uint32_t scratch, unsigned side)
{
    const unsigned damage = combo_damage(side);
    if (!damage_shown(side) || damage == 0u) return;
    if (!label_made) label_made = make_damage_label();
    const unsigned units = side == 0u ? 48u + (counter_hits(side) >= 10u ? DIGIT_STEP : 0u) : 491u + 48u;
    const unsigned y = *recomp_memory_u32(HUD_Y) + 0x58u + (counter_shown(side) ? LINE : 0u);
    call(UI_RESET, NULL, 0u);
    put32(SPRITE_SCALE, *recomp_memory_u32(COUNTER_SCALE));
    put16(ORIGIN_X, 0u);
    put16(ORIGIN_Y, 0x220u);
    *recomp_memory(SPRITE_MODE, 1u) = *recomp_memory(DEMO_FLAG, 1u) == 1u ? 4u : 2u;
    const unsigned value = damage > 9999u ? 9999u : damage;
    const unsigned digits = value >= 1000u ? 4u : value >= 100u ? 3u : value >= 10u ? 2u : 1u;
    const unsigned first = units - (counter_hits(side) >= 10u ? DIGIT_STEP : 0u);
    unsigned rest = value;
    for (unsigned i = digits; i > 0u; --i) {
        draw_sprite(scratch, DIGIT_SPRITE + rest % 10u, first + DIGIT_STEP * (i - 1u), y);
        rest /= 10u;
    }
    /* The label's 15 rows sit on the digits' 21: its baseline on theirs. */
    if (label_made)
        draw_sprite(scratch, recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_DAMAGE_LABEL),
                    first + DIGIT_STEP * digits + 4u, y + 5u);
    put32(SPRITE_SCALE, 0x3f800000u);
    call(UI_RESET, NULL, 0u);
}

static void popups_first(void)
{
    hud_moved = fight_hud() && (counter_shown(0u) || counter_shown(1u));
    if (hud_moved) put32(HUD_Y, *recomp_memory_u32(HUD_Y) + LINE);
    sub_000B8AE0();
}

static void popups_last(void)
{
    sub_000B8810();
    recomp_hit_voice_pump(); /* keeps a KO cry fed after the life stops changing */
    if (hud_moved) put32(HUD_Y, *recomp_memory_u32(HUD_Y) - LINE);
    hud_moved = false;
    if (!fight_hud()) {
        memset(attack_damage, 0, sizeof attack_damage);
        memset(single_frames, 0, sizeof single_frames);
        return;
    }
    const uint32_t saved_esp = recomp_runtime.registers.esp, saved_eax = recomp_runtime.registers.eax;
    recomp_runtime.registers.esp -= 0x100u;
    const uint32_t scratch = recomp_runtime.registers.esp;
    watch_attacks();
    draw_damage(scratch, 0u);
    draw_damage(scratch, 1u);
    recomp_runtime.registers.esp = saved_esp;
    recomp_runtime.registers.eax = saved_eax;
}

/* ---- Life bars -------------------------------------------------------- */

/* The life texture is 8-bit, each texture page an entry {texture, palette}
   (0xaa9b0) whose palette 0xb072c binds. Its colours come from that palette,
   which other HUD pages may share, so a coloured bar gets a palette of its
   own in the runner's guest memory, the game's colours recoloured; Default
   puts the game's palette back. */
static uint32_t life_entry;          /* the page entry */
static uint32_t game_palette;        /* the palette the game gave the page */
static uint32_t own_palette, own_colours;
static uint32_t own_rgb = 0xffffffffu;

static unsigned brightness(const uint8_t *bgra)
{
    const unsigned bg = bgra[0] > bgra[1] ? bgra[0] : bgra[1];
    return bg > bgra[2] ? bg : bgra[2];
}

static void colour_life_texture(void)
{
    const uint32_t pages = *recomp_memory_u32(TEXTURE_PAGES + 4u * LIFE_PAGE_GROUP);
    if (pages == 0u) return;
    const uint32_t entry = pages + 8u * LIFE_PAGE;
    const uint32_t bound = *recomp_memory_u32(entry + 4u);
    if (bound == 0u) {
        static bool reported;
        if (!reported) {
            reported = true;
            fprintf(stderr, "recomp combo hud: life texture has no palette; colour unchanged\n");
        }
        return;
    }
    /* A new page or a reloaded group: what is bound is the game's. */
    if (entry != life_entry || (bound != own_palette && bound != game_palette) || own_palette == 0u) {
        if (entry != life_entry || bound != own_palette) game_palette = bound;
        life_entry = entry;
        own_rgb = 0xffffffffu;
    }
    if (life_rgb == 0u) {
        if (bound != game_palette) *recomp_memory_u32(entry + 4u) = game_palette;
        return;
    }
    if (own_palette == 0u) own_palette = recomp_runner_palette(&own_colours);
    uint32_t game_colours;
    if (own_palette == 0u ||
        !recomp_d3d_texture_resolve_cpu_address(*recomp_memory_u32(game_palette + 4u), &game_colours)) return;
    if (own_rgb != life_rgb || bound != own_palette) {
        const uint8_t *from = recomp_memory(game_colours, 1024u);
        uint8_t *to = recomp_memory(own_colours, 1024u);
        unsigned brightest = 1u;
        for (unsigned i = 0u; i < 1024u; i += 4u)
            if (from[i + 3u] != 0u && brightness(from + i) > brightest) brightest = brightness(from + i);
        const unsigned r = life_rgb >> 16 & 255u, g = life_rgb >> 8 & 255u, b = life_rgb & 255u;
        for (unsigned i = 0u; i < 1024u; i += 4u) {
            const unsigned v = brightness(from + i);
            const unsigned scaled = v > brightest ? brightest : v;
            to[i] = (uint8_t)(b * scaled / brightest);
            to[i + 1u] = (uint8_t)(g * scaled / brightest);
            to[i + 2u] = (uint8_t)(r * scaled / brightest);
            to[i + 3u] = from[i + 3u];
        }
        own_rgb = life_rgb;
        *recomp_memory_u32(entry + 4u) = own_palette;
    }
}

void recomp_combo_hud_set_life_bar_colour(unsigned colour, uint32_t custom_rgb)
{
    if (colour >= RECOMP_LIFE_BAR_COLOURS) colour = RECOMP_LIFE_BAR_DEFAULT;
    life_rgb = colour == RECOMP_LIFE_BAR_CUSTOM ? custom_rgb & 0xffffffu : RECOMP_LIFE_BAR_RGB[colour];
    if (colour == RECOMP_LIFE_BAR_CUSTOM && life_rgb == 0u) life_rgb = 0x010101u; /* black, not the default */
}

/* After a life fill, the critical stun's remaining damage as a darker segment
   at its draining end, as Sparring's bar shows it. */
static void critical_segment(uint32_t record)
{
    const unsigned side = *recomp_memory(SPRITE_FLIP, 1u) == 1u ? 0u : 1u;
    if (*recomp_memory(CRITICAL + side, 1u) != 1u) return;
    const unsigned allows = *recomp_memory_u16(CRITICAL_ALLOWS + 2u * side);
    const unsigned taken = *recomp_memory_u16(CRITICAL_TAKEN + 2u * side);
    const unsigned life = *recomp_memory_u16(MAX_LIFE + 2u * side);
    if (allows <= taken || life == 0u) return;
    float fill;
    memcpy(&fill, recomp_memory(record + 0x14u, 4u), 4u);
    float segment = (float)((allows - taken) * LIFE_PIXELS) / (float)life / 8.0f;
    if (!(fill > 0.0f)) return;
    if (segment > fill) segment = fill;
    const uint32_t saved_esp = recomp_runtime.registers.esp;
    recomp_runtime.registers.esp -= 0x40u;
    const uint32_t copy = recomp_runtime.registers.esp;
    memcpy(recomp_memory(copy, 0x20u), recomp_memory(record, 0x20u), 0x20u);
    memcpy(recomp_memory(copy + 0x14u, 4u), &segment, 4u);
    put32(copy + 0x1cu, 0x60000000u); /* black at 38%, as Sparring's 0x50000000 over its blink */
    const uint32_t args[1] = {copy};
    kernel_call_lifted(sub_000B11E0, args, 1u);
    recomp_runtime.registers.esp = saved_esp;
}

static void sprite_draw(void)
{
    const uint32_t record = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    const bool life = !online_session && record != 0u && *recomp_memory_u32(record + 4u) == LIFE_SPRITE;
    if (life) colour_life_texture();
    sub_000B11E0();
    if (life && fight_hud()) {
        const uint32_t result = recomp_runtime.registers.eax;
        critical_segment(record);
        recomp_runtime.registers.eax = result;
    }
}

void recomp_combo_hud_install(bool online)
{
    online_session = online;
    hud_moved = false;
    memset(latched_damage, 0, sizeof latched_damage);
    memset(latched_hits, 0, sizeof latched_hits);
    memset(single_frames, 0, sizeof single_frames);
    memset(attack_damage, 0, sizeof attack_damage);
    life_entry = game_palette = own_palette = own_colours = 0u;
    own_rgb = 0xffffffffu;
    damage_label_install();   /* the runner calls this after its asset area is reset */
}

RecompFunction recomp_combo_hud_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case ATTACK_DAMAGE: return note_attack_damage;
    case POPUPS_FIRST: return popups_first;
    case POPUPS_LAST: return popups_last;
    case SPRITE_DRAW: return sprite_draw;
    default: return NULL;
    }
}
