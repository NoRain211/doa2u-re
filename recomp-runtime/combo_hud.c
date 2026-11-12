/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "combo_hud.h"
#include "kernel_abi.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The fight HUD (0xba120) draws the hit popups (COUNTER HIT and the like,
   0xb8ae0, 0xb86b0, 0xb8980 and 0xb8810) in every mode but Sparring and its
   Training (battle modes 9 and 10), which draw Sparring's own overlay
   (0x291900) instead. That overlay's panel (0x28e610, 0x28ef70) shows the
   combo's hits and damage, and its bar (0x28e1a0) the damage taken against
   the opponent's life with a blinking segment for what is left of the
   critical stun. This draws the same readings after 0xb8810, on the
   attacking side, with Sparring's own text, number and bar calls.

   Per defending side d:
   - hits taken in the current combo: byte 0x87ae91 + 0x94 d;
   - the combo's damage: 0xdf4a04 + 4 d, added while the side is comboed and
     cleared when the combo ends (0x119420);
   - its life: 0x87d498 + 2 d;
   - critical stun (0x114c50, 0x1124f0): 0xdf38c0 + d is 1 while it lasts,
     0xdf3c70 + 2 d the damage it allows (40, 60, or the opening hit's
     damage + 20) and 0xdf38d8 + 2 d the damage taken in it; past the
     allowance the stun breaks. Sparring reads the allowance through
     0x1124b0. */
enum {
    BATTLE_MODE = 0x0087cfa0u,
    HUD_WIDTH = 0x008783b8u,
    SIDE_HITS = 0x0087ae91u,
    SIDE_STRIDE = 0x94u,
    COMBO_DAMAGE = 0x00df4a04u,
    MAX_LIFE = 0x0087d498u,
    CRITICAL = 0x00df38c0u,
    CRITICAL_ALLOWS = 0x00df3c70u,
    CRITICAL_TAKEN = 0x00df38d8u,
    UI_SCALE_CONSTANT = 0x003c2d6cu, /* 0.9, Sparring's text scale */

    POPUPS_LAST = 0x000b8810u,
    SPRITE_DRAW = 0x000b11e0u,
    UI_RESET = 0x0006baa0u,
    UI_SCALE = 0x000ab510u,
    MESSAGE = 0x0006a3e0u,  /* cdecl (message, language 0xff) */
    TEXT = 0x000b20b0u,     /* cdecl (record, 1) */
    NUMBER = 0x000bb370u,   /* cdecl (ASCII), at 0x8a0980 */
    QUAD = 0x000b13a0u,     /* cdecl (record, layer, 1) */
    NUMBER_X = 0x008a0980u,
    NUMBER_Y = 0x008a0982u,
    NUMBER_SCALE_X = 0x008a0990u,
    NUMBER_SCALE_Y = 0x008a0994u,
    NUMBER_ALIGN = 0x008a0998u,

    MESSAGE_PTS = 931u,
    MESSAGE_HIT = 932u,

    LIFE_SPRITE = 0x1001cu,
    LINGER_FRAMES = 90u,     /* the counter stays 1.5 s after a combo */
    BAR_W = 140u,
    BLOCK_Y = 200u,          /* under three stacked popups (y 0x78 + 19 each) */
    SIDE_MARGIN = 47u,       /* the popups' own margin */
};

extern void sub_000B8810(void);
extern void sub_000B11E0(void);

static bool online_session;
static uint32_t life_bar_tint = 0xffffffffu;

typedef struct Shown {
    unsigned linger;
    unsigned hits;
    unsigned damage;
    unsigned max_life;
    bool critical;
    unsigned critical_left;
} Shown;
static Shown shown[2];  /* by defending side */
static bool blink;

void recomp_combo_hud_install(bool online)
{
    online_session = online;
    memset(shown, 0, sizeof shown);
}

void recomp_combo_hud_set_life_bar_colour(unsigned colour)
{
    /* Multiplied into the life sprite's own colours (0xb11e0's record + 0x1c). */
    static const uint32_t tints[RECOMP_LIFE_BAR_COLOURS] = {
        0xffffffffu, 0xffff4040u, 0xff40ff60u, 0xff4890ffu, 0xffb060ffu, 0xffff70c8u,
    };
    life_bar_tint = tints[colour < RECOMP_LIFE_BAR_COLOURS ? colour : 0u];
}

/* ---- Guest calls ---------------------------------------------------- */

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint32_t call(uint32_t address, const uint32_t *args, unsigned count)
{
    kernel_call_guest(address, args, count);
    return recomp_runtime.registers.eax;
}

static void put16(uint32_t at, uint16_t value) { memcpy(recomp_memory(at, 2u), &value, 2u); }
static void put32(uint32_t at, uint32_t value) { memcpy(recomp_memory(at, 4u), &value, 4u); }

/* A label as Sparring's panel draws one (0x28e717): 0x20-byte record. */
static void draw_label(uint32_t record, uint32_t message, unsigned x, unsigned y)
{
    const uint32_t message_args[2] = {message, 0xffu};
    const uint32_t text = call(MESSAGE, message_args, 2u);
    const uint32_t scale = *recomp_memory_u32(UI_SCALE_CONSTANT);
    memset(recomp_memory(record, 0x20u), 0, 0x20u);
    put16(record + 0x00u, 0x20u);
    put16(record + 0x02u, 0x108u);
    put32(record + 0x04u, text);
    put16(record + 0x08u, (uint16_t)(x << 4));
    put16(record + 0x0au, (uint16_t)(y << 4));
    put32(record + 0x0cu, 0xccffffffu);
    put32(record + 0x10u, 0x30031100u); /* low byte 0: left aligned, as "pts" */
    put16(record + 0x14u, 0x32u);
    *recomp_memory(record + 0x16u, 1u) = 0x40u;
    *recomp_memory(record + 0x17u, 1u) = 0x30u;
    put32(record + 0x18u, scale);
    put32(record + 0x1cu, scale);
    const uint32_t args[2] = {record, 1u};
    call(TEXT, args, 2u);
}

/* A number as the panel draws one: "%3d" at (x, y), alignment 2. */
static void draw_number(uint32_t buffer, unsigned value, unsigned x, unsigned y)
{
    char text[16];
    snprintf(text, sizeof text, "%3u", value > 999u ? 999u : value);
    memcpy(recomp_memory(buffer, sizeof text), text, sizeof text);
    call(UI_RESET, NULL, 0u);
    const uint32_t scale = *recomp_memory_u32(UI_SCALE_CONSTANT);
    put16(NUMBER_X, (uint16_t)(x << 4));
    put16(NUMBER_Y, (uint16_t)(y << 4));
    *recomp_memory(NUMBER_ALIGN, 1u) = 2u;
    put32(NUMBER_SCALE_X, scale);
    put32(NUMBER_SCALE_Y, scale);
    const uint32_t args[1] = {buffer};
    call(NUMBER, args, 1u);
}

/* A rectangle as the bar draws one (0x28e37e): x, y, w, h in pixels. */
static void draw_quad(uint32_t record, uint32_t layer, uint32_t kind, unsigned x, unsigned y,
    unsigned w, unsigned h, uint32_t flags, uint32_t colour)
{
    memset(recomp_memory(record, 0x20u), 0, 0x20u);
    put16(record + 0x00u, (uint16_t)kind);
    put16(record + 0x02u, (uint16_t)layer);
    put16(record + 0x04u, (uint16_t)(x << 4));
    put16(record + 0x06u, (uint16_t)(y << 4));
    put16(record + 0x08u, (uint16_t)(w << 4));
    put16(record + 0x0au, (uint16_t)(h << 4));
    put32(record + 0x0cu, flags);
    for (unsigned corner = 0u; corner < 4u; ++corner) put32(record + 0x10u + 4u * corner, colour);
    const uint32_t args[3] = {record, layer, 1u};
    call(QUAD, args, 3u);
}

/* Sparring's fill colours (0x28e2d4) from full to empty, blended at 70%, 50%
   and 30% of life as 0x28e2fc does. */
static uint32_t bar_colour(float left)
{
    static const uint32_t colours[5] = {0xff3fb6e7u, 0xff86fdbeu, 0xffffff6du, 0xffffa43cu, 0xffff3f3bu};
    static const float stops[5] = {1.0f, 0.7f, 0.5f, 0.3f, 0.0f};
    if (!(left > 0.0f)) return colours[4];
    if (left >= 1.0f) return colours[0];
    unsigned i = 0u;
    while (left < stops[i + 1u]) ++i;
    const float t = (stops[i] - left) / (stops[i] - stops[i + 1u]);
    uint32_t out = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        const float a = (float)((colours[i] >> shift) & 0xffu), b = (float)((colours[i + 1u] >> shift) & 0xffu);
        out |= (uint32_t)lroundf(a + (b - a) * t) << shift;
    }
    return out;
}

/* ---- The counter and bar -------------------------------------------- */

static void update(unsigned defender)
{
    Shown *s = &shown[defender];
    const unsigned hits = *recomp_memory(SIDE_HITS + SIDE_STRIDE * defender, 1u);
    const bool critical = *recomp_memory(CRITICAL + defender, 1u) == 1u;
    if (hits >= 2u || critical) {
        s->linger = LINGER_FRAMES;
        s->hits = hits;
        s->damage = *recomp_memory_u16(COMBO_DAMAGE + 4u * defender);
        s->max_life = *recomp_memory_u16(MAX_LIFE + 2u * defender);
        s->critical = critical;
        const unsigned allows = *recomp_memory_u16(CRITICAL_ALLOWS + 2u * defender);
        const unsigned taken = *recomp_memory_u16(CRITICAL_TAKEN + 2u * defender);
        s->critical_left = critical && allows > taken ? allows - taken : 0u;
    } else if (s->linger > 0u) {
        --s->linger;
        s->critical = false;
    }
}

static void draw_block(uint32_t scratch, unsigned defender)
{
    const Shown *s = &shown[defender];
    if (s->linger == 0u || s->max_life == 0u) return;
    const unsigned width = *recomp_memory_u16(HUD_WIDTH);
    const unsigned attacker = 1u - defender;
    const unsigned left = attacker == 0u ? SIDE_MARGIN
        : (width > SIDE_MARGIN + BAR_W ? width - SIDE_MARGIN - BAR_W : 0u);
    const unsigned y = BLOCK_Y;

    /* "NNN hit", then the combo's damage as "NNN pts", as Sparring's panel. */
    if (s->hits >= 2u) {
        draw_number(scratch + 0x40u, s->hits, left, y);
        call(UI_RESET, NULL, 0u);
        draw_label(scratch, MESSAGE_HIT, left + 28u, y + 5u);
    }
    draw_number(scratch + 0x40u, s->damage, left, y + 22u);
    call(UI_RESET, NULL, 0u);
    draw_label(scratch, MESSAGE_PTS, left + 28u, y + 27u);

    /* The bar: the opponent's life less the combo's damage, as 0x28e1a0. */
    const float rest = (float)((int)s->max_life - (int)s->damage) / (float)s->max_life;
    int fill = (int)(rest * (float)BAR_W);
    if (fill > (int)BAR_W) fill = (int)BAR_W;
    if (fill < 3) fill = 3;
    const unsigned bar_y = y + 50u;
    call(UI_RESET, NULL, 0u);
    draw_quad(scratch, 0x105u, 0x20u, left + 1u, bar_y + 1u, (unsigned)fill - 3u, 8u, 0x40001u,
        bar_colour(rest));
    if (s->critical && blink && s->critical_left > 0u) {
        unsigned segment = s->critical_left * BAR_W / s->max_life;
        if (segment > (unsigned)fill) segment = (unsigned)fill;
        if (segment > 0u)
            draw_quad(scratch, 0x104u, 0x14u, left + 1u + (unsigned)fill - segment, bar_y, segment, 8u,
                0x40000u, 0x50000000u);
    }
    draw_quad(scratch, 0x104u, 0x14u, left, bar_y, BAR_W - 3u, 8u, 0x40003u, 0x50000000u);
}

static void popups_last(void)
{
    sub_000B8810();
    if (online_session) return;
    const uint8_t mode = *recomp_memory(BATTLE_MODE, 1u);
    if (mode == 9u || mode == 10u) return; /* Sparring draws its own */
    update(0u);
    update(1u);
    if (shown[0].linger == 0u && shown[1].linger == 0u) return;
    blink = !blink; /* every frame, as 0x28e1b5 */

    const uint32_t saved_esp = recomp_runtime.registers.esp;
    const uint32_t saved_eax = recomp_runtime.registers.eax;
    recomp_runtime.registers.esp -= 0x100u;
    const uint32_t scratch = recomp_runtime.registers.esp;
    const uint32_t ninety[2] = {float_bits(0.9f), float_bits(0.9f)};
    call(UI_SCALE, ninety, 2u);
    draw_block(scratch, 0u);
    draw_block(scratch, 1u);
    const uint32_t one[2] = {float_bits(1.0f), float_bits(1.0f)};
    call(UI_SCALE, one, 2u);
    recomp_runtime.registers.esp = saved_esp;
    recomp_runtime.registers.eax = saved_eax;
}

/* The life bar (0xb3bf2) draws sprite 0x1001c through 0xb11e0 with a white
   tint at record + 0x1c; the chosen colour replaces it. */
static void sprite_draw(void)
{
    const uint32_t record = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    if (life_bar_tint != 0xffffffffu && record != 0u && *recomp_memory_u32(record + 4u) == LIFE_SPRITE)
        put32(record + 0x1cu, life_bar_tint);
    sub_000B11E0();
}

RecompFunction recomp_combo_hud_lookup_manual(uint32_t guest_address)
{
    if (guest_address == POPUPS_LAST) return popups_last;
    if (guest_address == SPRITE_DRAW) return sprite_draw;
    return NULL;
}
