/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "combo_hud.h"
#include "hit_voice.h"
#include "kernel_abi.h"
#include "runner_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The lone hit the game shows no combo counter for: its damage is the one the
   attack routine (0x1159f0) added to the defender's pending damage, shown for
   64 frames on the counter's row, until the game's own combo damage takes the
   line over. Synthetic guest memory and sprite draws stand in for the game. */

enum {
    BATTLE_MODE = 0x0087cfa0u,
    LIFE = 0x0087ae54u,
    SIDE_STRIDE = 0x94u,
    HUD_Y = 0x008a2f4cu,
    COUNTER_HITS = 0x008a212cu,
    COUNTER_TIMER = 0x008a2138u,
    COUNTER_STRIDE = 0x10u,
    COMBO_DAMAGE = 0x00df4a04u,
    DIGIT_SPRITE = 0x100a7u,
    LINE = 22u,
    ROW = 0x58u,
    STACK = 0x00100000u,
    ATTACK_DAMAGE = 0x001159f0u,
    POPUPS_FIRST = 0x000b8ae0u,
    POPUPS_LAST = 0x000b8810u,
    SPRITE = 0x00075a50u,
};

static int failures;
static void expect(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;    /* NULL: every access goes through recomp_memory */
static uint8_t game[0x2800]; /* 0x87ae00: mode, life, demo flag */
static uint8_t hud[0x2600];  /* 0x8a0980: origin, counter, HUD y */
static uint8_t scale[4];     /* 0x415994: the counter's 0.88 */
static uint8_t combo[0x10];  /* 0xdf4a00: the combo's damage */
static uint8_t stack[0x200];

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= STACK && address + width <= STACK + sizeof stack) return stack + (address - STACK);
    if (address >= 0x0087ae00u && address + width <= 0x0087ae00u + sizeof game)
        return game + (address - 0x0087ae00u);
    if (address >= 0x008a0980u && address + width <= 0x008a0980u + sizeof hud)
        return hud + (address - 0x008a0980u);
    if (address >= 0x00415994u && address + width <= 0x00415994u + sizeof scale)
        return scale + (address - 0x00415994u);
    if (address >= 0x00df4a00u && address + width <= 0x00df4a00u + sizeof combo)
        return combo + (address - 0x00df4a00u);
    fprintf(stderr, "unexpected guest access %08x\n", address);
    abort();
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

/* ---- Guest routines the HUD calls ------------------------------------- */

static unsigned attack_defender, attack_amount;

/* As 0x1159f0: the attack routine adds its damage to the defender's pending
   damage (life record + 2), which 0x119420 then lands. */
void sub_001159F0(void)
{
    uint16_t *pending = recomp_memory_u16(LIFE + SIDE_STRIDE * attack_defender + 2u);
    *pending = (uint16_t)(*pending + attack_amount);
}

void sub_000B8AE0(void) {}
void sub_000B8810(void) {}
void sub_000B11E0(void) {}
void recomp_hit_voice_pump(void) {}

/* The label needs the fight's loaded pages, which a synthetic test has not. */
bool recomp_game_sprite(uint32_t id, int height, RecompGameSprite *sprite)
{
    (void)id;
    (void)height;
    (void)sprite;
    return false;
}
uint8_t recomp_game_sprite_index(const RecompGameSprite *sprite, int x, int y)
{
    (void)sprite;
    (void)x;
    (void)y;
    return 0u;
}
uint32_t recomp_runner_palette(uint32_t *colours)
{
    (void)colours;
    return 0u; /* no runner asset area: the DAMAGE label is never made */
}
void recomp_runner_sprite(unsigned slot, unsigned log_width, unsigned log_height, uint32_t palette)
{
    (void)slot;
    (void)log_width;
    (void)log_height;
    (void)palette;
}
void recomp_runner_sprite_size(unsigned slot, int width, int height)
{
    (void)slot;
    (void)width;
    (void)height;
}
void recomp_runner_sprite_pixels(unsigned slot, const uint8_t *pixels)
{
    (void)slot;
    (void)pixels;
}
bool recomp_d3d_texture_resolve_cpu_address(uint32_t locked_address, uint32_t *cpu_address)
{
    (void)locked_address;
    (void)cpu_address;
    return false;
}

/* ---- What the HUD draws ----------------------------------------------- */

typedef struct SpriteDraw {
    unsigned id, x, y;
} SpriteDraw;
static SpriteDraw draws[16];
static unsigned draw_count;

/* The sprite call's record, as 0x75a50 reads it from the pushed argument. */
void recomp_dispatch_indirect_site(uint32_t guest_address, uint32_t saved_esp, const char *site, int line)
{
    (void)saved_esp;
    (void)site;
    (void)line;
    if (guest_address != SPRITE) return;
    const uint32_t record = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    const uint32_t packed = *recomp_memory_u32(record + 4u);
    if (draw_count >= sizeof draws / sizeof draws[0]) abort();
    draws[draw_count].id = *recomp_memory_u32(record);
    draws[draw_count].x = packed & 0xfffu;
    draws[draw_count].y = packed >> 12 & 0xfffu;
    ++draw_count;
}

static int digit_at(unsigned x, unsigned y)
{
    for (unsigned i = 0u; i < draw_count; ++i)
        if (draws[i].id >= DIGIT_SPRITE && draws[i].id <= DIGIT_SPRITE + 9u && draws[i].x == x &&
            draws[i].y == y)
            return (int)(draws[i].id - DIGIT_SPRITE);
    return -1;
}

static unsigned digit_draws(void)
{
    unsigned count = 0u;
    for (unsigned i = 0u; i < draw_count; ++i)
        if (draws[i].id >= DIGIT_SPRITE && draws[i].id <= DIGIT_SPRITE + 9u) ++count;
    return count;
}

/* ---- Synthetic guest state -------------------------------------------- */

static void set_hits(unsigned side, unsigned hits)
{
    *recomp_memory_u32(COUNTER_HITS + COUNTER_STRIDE * side) = hits;
}

static void set_timer(unsigned side, uint32_t frames)
{
    *recomp_memory_u32(COUNTER_TIMER + COUNTER_STRIDE * side) = frames;
}

static void set_combo_damage(unsigned side, unsigned damage)
{
    const uint16_t value = (uint16_t)damage;
    memcpy(recomp_memory(COMBO_DAMAGE + 4u * (1u - side), 2u), &value, 2u);
}

static void begin(bool online)
{
    memset(game, 0, sizeof game);
    memset(hud, 0, sizeof hud);
    memset(combo, 0, sizeof combo);
    memset(stack, 0, sizeof stack);
    recomp_runtime.registers.esp = STACK + sizeof stack - 0x10u;
    recomp_runtime.registers.eax = 0u;
    recomp_combo_hud_install(online);
}

static void hit(unsigned defender, unsigned damage)
{
    attack_defender = defender;
    attack_amount = damage;
    recomp_combo_hud_lookup_manual(ATTACK_DAMAGE)();
}

static void frame(void)
{
    draw_count = 0u;
    recomp_combo_hud_lookup_manual(POPUPS_FIRST)();
    recomp_combo_hud_lookup_manual(POPUPS_LAST)();
}

int main(void)
{
    /* A lone hit: the game draws no counter for it, so its damage takes the
       counter's own row, its digits where a single-digit counter's are. */
    begin(false);
    hit(1u, 12u);
    frame();
    expect("a lone hit shows its damage", digit_at(48u, ROW) == 1 && digit_at(67u, ROW) == 2);
    expect("a lone hit shows nothing else", digit_draws() == 2u);

    /* It lasts 64 frames: this one and 63 more. */
    for (unsigned i = 1u; i <= 63u; ++i) frame();
    expect("a lone hit lasts 64 frames", digit_at(48u, ROW) == 1);
    frame();
    expect("a lone hit ends", digit_draws() == 0u);

    /* A stale hit count from an earlier combo does not shift its digits: side
       1's counter ends at 491 + 48, and a lone hit starts there too. */
    begin(false);
    set_hits(1u, 15u);
    hit(0u, 7u);
    frame();
    expect("a lone hit ignores an earlier combo's hits", digit_at(539u, ROW) == 7 && digit_draws() == 1u);

    /* The other side's lone hit ends under its own counter. */
    begin(false);
    hit(0u, 34u);
    frame();
    expect("side 1's lone hit", digit_at(539u, ROW) == 3 && digit_at(558u, ROW) == 4);

    /* A second hit makes a combo: the game's own damage takes the line over,
       one row lower, and the lone hit does not come back when it ends. */
    begin(false);
    hit(1u, 12u);
    frame();
    expect("the lone hit shows first", digit_at(48u, ROW) == 1);
    set_hits(0u, 2u);
    set_timer(0u, 40u);
    set_combo_damage(0u, 24u);
    frame();
    expect("the combo's damage takes the line over", digit_at(48u, ROW + LINE) == 2 &&
        digit_at(67u, ROW + LINE) == 4);
    expect("the lone hit does not stay", digit_draws() == 2u);
    set_timer(0u, 0u);
    frame();
    expect("the lone hit does not return", digit_draws() == 0u);

    /* Ten or more hits still move a running counter's digits, as before. */
    begin(false);
    set_hits(1u, 12u);
    set_timer(1u, 9u);
    set_combo_damage(1u, 34u);
    frame();
    expect("a ten-hit counter ends its side's digits under the counter",
        digit_at(520u, ROW + LINE) == 3 && digit_at(539u, ROW + LINE) == 4);

    /* A newer lone hit replaces the one showing, and restarts its 64 frames. */
    begin(false);
    hit(1u, 12u);
    frame();
    hit(1u, 7u);
    frame();
    expect("a newer lone hit replaces the last", digit_at(48u, ROW) == 7 && digit_draws() == 1u);

    /* Sparring and online keep the game's own HUD. */
    begin(false);
    *recomp_memory(BATTLE_MODE, 1u) = 9u;
    hit(1u, 12u);
    frame();
    expect("sparring shows no lone hit", digit_draws() == 0u);
    begin(true);
    hit(1u, 12u);
    frame();
    expect("online shows no lone hit", digit_draws() == 0u);

    if (failures == 0) puts("combo hud tests passed");
    return failures != 0;
}
