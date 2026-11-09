/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "menu_quit.h"
#include "kernel_abi.h"
#include "quit_icon.h"
#include "quit_prompt.h"
#include "runner_assets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The main menu (frontend_main_menu_update, 0x13b850) is a carousel over the
   16 entries of 0x6e20a0, 28 bytes each: mode, label sprite, picture, wide
   picture (XGetVideoFlags' widescreen). 0x134330 lists the unlocked ones'
   indices at 0xdf5d18 (16 bytes; the carousel object follows at 0xdf5d28)
   and their count at 0x6e2260. Everything else reads entries through
   0x1343a0 (entry at a position) and 0x1343d0 (the selected entry's mode),
   both clamping the position to the count. A confirm plays sound 0x24 and
   then switches on the mode; modes above 0x18 do nothing.

   Quit Game is one more position after the listed ones: the count grows by
   one, and the two readers answer for that position without touching the
   list, which may already be full. Its mode, 0x13, is one no entry uses and
   the switch ignores, so the confirm sound opens the runner's quit prompt
   (quit_prompt.c) instead, which takes the pads while it asks. */

extern void sub_00134330(void);
extern void sub_001343A0(void);
extern void sub_001343D0(void);
extern void sub_001345C0(void);
extern void sub_0013B850(void);
extern void sub_000FED70(void);
extern void sub_00135AE0(void);

enum {
    MENU_BUILD = 0x00134330u,
    MENU_ENTRY = 0x001343a0u,
    MENU_MODE = 0x001343d0u,
    MENU_HELP = 0x001345c0u,
    MENU_UPDATE = 0x0013b850u,
    PLAY_SOUND = 0x000fed70u,
    MENU_COUNT = 0x006e2260u,       /* u8 */
    SELECTED_MODE = 0x00df57b4u,    /* the mode 0x1343d0 gave this frame */

    /* Before the switch, the confirm checks the mode's slot at 0xdf5d70
       (0x134570 fills modes 0-0x18 with 2, mode 5 by the pads connected) and
       plays the "not available" sound for anything but 2. Mode 0x13 is in
       that table, always 2, no entry's, and the switch does nothing for it. */
    QUIT_MODE = 0x13,
    SOUND_CONFIRM = 0x24, SOUND_CANCEL = 0x29,
    LABEL_LOG_W = 7, LABEL_LOG_H = 5, PICTURE_LOG = 7, /* 128x32 and 128x128 */
    LABEL_W = 1 << LABEL_LOG_W, LABEL_H = 1 << LABEL_LOG_H, WORD_H = 24,
};

/* The label: slices of the menu's own words (group 7, all on one texture),
   slanting 0.25 px per row, in tenths of a pixel of u = x - lean(y). Q is
   the O of ONLINE with the end of RANKING's R as its tail. */
#define LEAN 0.25f
typedef struct Piece {
    recomp_utf16 c;      /* 0 for a tail drawn on the letter before */
    uint32_t sprite;
    int16_t from, to;
    int8_t top, bottom;  /* rows; 0 and 0 for all */
    int8_t dx, extra;    /* a tail's place on its letter; extra width for the letter */
} Piece;
static const Piece PIECES[] = {
    {u'Q', 0x70014, 545, 675, 0, 0, 0, 1}, {0, 0x70017, 170, 230, 10, 16, 8, 0},
    {u'U', 0x7000a, 85, 195, 0, 0, 0, 0}, {u'I', 0x7000b, 205, 265, 0, 0, 0, 0},
    {u'T', 0x7000b, 85, 205, 0, 0, 0, 0}, {u' ', 0, 0, 90, 0, 0, 0, 0},
    {u'G', 0x7000e, 405, 530, 0, 0, 0, 0}, {u'A', 0x7000e, 530, 655, 0, 0, 0, 0},
    {u'M', 0x7000b, 265, 405, 0, 0, 0, 0}, {u'E', 0x7000b, 405, 515, 0, 0, 0, 0},
};

/* The help line per game language (Japanese reads English). */
static const recomp_utf16 *const HELP[6] = {
    u"Quit the game and return to the desktop.", u"Quit the game and return to the desktop.",
    u"Quitter le jeu et revenir au bureau.", u"Das Spiel beenden und zum Desktop zur\u00fcckkehren.",
    u"Esci dal gioco e torna al desktop.", u"Salir del juego y volver al escritorio.",
};

/* ---- State -------------------------------------------------------- */

static uint32_t entry;           /* the 28-byte menu entry, in the runner's asset area */
static int quit_position = -1;   /* the carousel position, once listed */
static bool in_update, label_made;
static bool asking;              /* the quit prompt is open for Quit Game */

/* ---- Guest helpers -------------------------------------------------- */

static uint32_t u32(uint32_t address) { return *recomp_memory_u32(address); }
static void set_u32(uint32_t address, uint32_t v) { *recomp_memory_u32(address) = v; }

/* ---- The pictures --------------------------------------------------- */

/* The pictures' palette: 0 clear; 8-63 eight alphas by eight greys for
   edges and the shadow; 64-255 opaque greys. */
static uint8_t picture_index(uint8_t grey, uint8_t alpha)
{
    if (alpha >= 250u) return (uint8_t)(64u + (grey * 191u + 127u) / 255u);
    const unsigned a = (alpha * 7u + 127u) / 255u;
    return a == 0u ? 0u : (uint8_t)(a * 8u + (grey * 7u + 127u) / 255u);
}

static void write_picture(unsigned slot, float squeeze)
{
    static uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE], alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE];
    static uint8_t pixels[QUIT_ICON_SIZE * QUIT_ICON_SIZE];
    recomp_quit_icon_draw(squeeze, grey, alpha);
    for (uint32_t i = 0u; i < QUIT_ICON_SIZE * QUIT_ICON_SIZE; ++i) pixels[i] = picture_index(grey[i], alpha[i]);
    recomp_runner_sprite_pixels(slot, pixels);
}

/* ---- The label ------------------------------------------------------ */

static float lean_u(int x, int y) { return (float)x - LEAN * (float)(WORD_H - 1 - y) + LEAN * (float)WORD_H; }

/* Where pieces overlap, letter body beats outline beats clear. */
static unsigned rank(uint32_t palette_data, uint8_t index)
{
    const uint32_t argb = u32(palette_data + 4u * index);
    const unsigned alpha = argb >> 24, red = (argb >> 16) & 0xffu;
    return alpha == 0u ? 0u : red < 110u ? alpha : 256u + red;
}

/* The words' soft shadow is one dark colour at up to half alpha. Cut with
   the letters it would end in hard edges, so the label takes the letters and
   outlines only and casts its own: its coverage 4 px down, box-blurred
   (radius 2) twice, at 0.436 alpha, fitted to the game's words. */
/* The words brighten from grey at their left end to white by x 51; a letter
   moved along keeps the brightness of its new place (measured over all 16). */
static float brightness_ramp(float x)
{
    static const float RAMP[][2] = {{0, 0.64f}, {11, 0.74f}, {23, 0.76f}, {31, 0.90f}, {39, 0.915f}, {47, 0.975f}, {51, 1.0f}};
    if (x <= RAMP[0][0]) return RAMP[0][1];
    for (unsigned i = 1u; i < sizeof RAMP / sizeof RAMP[0]; ++i)
        if (x <= RAMP[i][0])
            return RAMP[i - 1][1] + (RAMP[i][1] - RAMP[i - 1][1]) * (x - RAMP[i - 1][0]) / (RAMP[i][0] - RAMP[i - 1][0]);
    return 1.0f;
}

/* The palette entry of this alpha closest to a grey; the original if none is grey. */
static uint8_t grey_index(uint32_t palette_data, uint8_t original, unsigned grey)
{
    const uint32_t want_alpha = u32(palette_data + 4u * original) >> 24;
    unsigned best = original, best_distance = 1000u;
    for (unsigned i = 0u; i < 256u; ++i) {
        const uint32_t argb = u32(palette_data + 4u * i);
        const unsigned r = (argb >> 16) & 0xffu, g = (argb >> 8) & 0xffu, b = argb & 0xffu;
        if ((argb >> 24) != want_alpha || r != g || g != b) continue;
        const unsigned d = r > grey ? r - grey : grey - r;
        if (d < best_distance) { best_distance = d; best = i; }
    }
    return (uint8_t)best;
}

#define SHADOW_RGB 0x231f1fu
#define SHADOW_GAIN 0.436f
static bool shadow_colour(uint32_t argb)
{
    return (argb & 0xffffffu) == SHADOW_RGB && (argb >> 24) <= 200u;
}

static void cast_shadow(uint32_t palette_data, uint8_t image[32][128])
{
    enum { W = 128, H = 32, DROP = 4, R = 2 };
    static float cover[H][W], pass[H][W];
    uint8_t nearest[256];
    int best[256];
    for (unsigned a = 0u; a < 256u; ++a) { nearest[a] = 0u; best[a] = 1 << 30; }
    for (unsigned i = 0u; i < 256u; ++i) {
        const uint32_t argb = u32(palette_data + 4u * i);
        if ((argb & 0xffffffu) != SHADOW_RGB) continue;
        const int alpha = (int)(argb >> 24);
        for (int a = 0; a < 256; ++a) {
            const int d = a > alpha ? a - alpha : alpha - a;
            if (d < best[a]) { best[a] = d; nearest[a] = (uint8_t)i; }
        }
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            cover[y][x] = y >= DROP ? (u32(palette_data + 4u * image[y - DROP][x]) >> 24) / 255.0f : 0.0f;
    for (int n = 0; n < 2; ++n) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float sum = 0.0f;
                for (int d = -R; d <= R; ++d) if (x + d >= 0 && x + d < W) sum += cover[y][x + d];
                pass[y][x] = sum / (2 * R + 1);
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float sum = 0.0f;
                for (int d = -R; d <= R; ++d) if (y + d >= 0 && y + d < H) sum += pass[y + d][x];
                cover[y][x] = sum / (2 * R + 1);
            }
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (image[y][x] != 0u && (u32(palette_data + 4u * image[y][x]) >> 24) != 0u) continue;
            const int alpha = (int)lrintf(255.0f * SHADOW_GAIN * cover[y][x]);
            if (alpha > 0 && best[alpha > 255 ? 255 : alpha] < 1 << 30) image[y][x] = nearest[alpha > 255 ? 255 : alpha];
        }
}

/* Makes the label once the menu's art is loaded; false until then. */
static bool make_label(void)
{
    RecompGameSprite first;
    if (!recomp_game_sprite(PIECES[0].sprite, WORD_H, &first)) return false;
    const uint32_t palette_data = first.colours;
    static uint8_t image[LABEL_H][LABEL_W];
    memset(image, 0, sizeof image);
    float pen = LEAN * (float)WORD_H, letter_pen = pen;
    for (size_t p = 0u; p < sizeof PIECES / sizeof PIECES[0]; ++p) {
        const Piece *piece = &PIECES[p];
        if (piece->c == u' ') {
            pen += piece->to / 10.0f;
            continue;
        }
        RecompGameSprite word;
        if (!recomp_game_sprite(piece->sprite, WORD_H, &word) || word.palette != first.palette) return false;
        const float u0 = piece->from / 10.0f, u1 = piece->to / 10.0f;
        const float at = piece->c == 0 ? letter_pen + piece->dx : pen;
        const int shift = (int)lrintf(at - u0);
        for (int y = 0; y < WORD_H; ++y) {
            if (piece->bottom != 0 && (y < piece->top || y >= piece->bottom)) continue;
            for (int x = 0; x < word.right - word.left; ++x) {
                const float u = lean_u(x, y);
                if (u < u0 || u >= u1) continue;
                const int dx = x + shift;
                if (dx < 0 || dx >= LABEL_W) continue;
                uint8_t index = recomp_game_sprite_index(&word, x, y);
                const uint32_t argb = u32(palette_data + 4u * index);
                if (shadow_colour(argb)) continue;
                if (rank(palette_data, index) <= rank(palette_data, image[y][dx])) continue;
                const unsigned red = (argb >> 16) & 0xffu;
                if (red > 110u && dx != x) {
                    const float grey = red * brightness_ramp((float)dx) / brightness_ramp((float)x);
                    index = grey_index(palette_data, index, grey > 255.0f ? 255u : (unsigned)lrintf(grey));
                }
                image[y][dx] = index;
            }
        }
        if (piece->c != 0) {
            letter_pen = pen;
            pen += u1 - u0 + piece->extra;
        }
    }
    cast_shadow(palette_data, image);
    const int width = (int)pen + 8 < LABEL_W ? (int)pen + 8 : LABEL_W;
    recomp_runner_sprite_pixels(RECOMP_RUNNER_SPRITE_QUIT_LABEL, &image[0][0]);
    recomp_runner_sprite_size(RECOMP_RUNNER_SPRITE_QUIT_LABEL, width, WORD_H);
    /* The label draws in the menu labels' palette. */
    recomp_runner_sprite_palette(RECOMP_RUNNER_SPRITE_QUIT_LABEL, first.palette);
    fprintf(stderr, "recomp menu quit: label made, %d pixels wide\n", width);
    return true;
}

/* ---- Install -------------------------------------------------------- */

void recomp_menu_quit_install(void)
{
    quit_position = -1;
    in_update = label_made = asking = false;
    entry = recomp_runner_alloc(28u, 4u);
    const uint32_t ids[4] = {QUIT_MODE, recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_QUIT_LABEL),
        recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_QUIT_PICTURE),
        recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_QUIT_WIDE)};
    for (unsigned i = 0u; i < 4u; ++i) set_u32(entry + 4u * i, ids[i]);

    /* The pictures' palette: 0 clear; 8-63 eight alphas by eight greys;
       64-255 opaque greys. The label's becomes the menu's at make_label. */
    uint32_t colours;
    const uint32_t palette = recomp_runner_palette(&colours);
    for (unsigned i = 0u; i < 256u; ++i) {
        uint32_t a, g;
        if (i >= 64u) { a = 255u; g = (i - 64u) * 255u / 191u; }
        else { a = (i / 8u) * 255u / 7u; g = (i % 8u) * 255u / 7u; }
        set_u32(colours + 4u * i, a << 24 | g << 16 | g << 8 | g);
    }
    recomp_runner_sprite(RECOMP_RUNNER_SPRITE_QUIT_LABEL, LABEL_LOG_W, LABEL_LOG_H, palette);
    recomp_runner_sprite_size(RECOMP_RUNNER_SPRITE_QUIT_LABEL, 1, WORD_H);
    const unsigned pictures[2] = {RECOMP_RUNNER_SPRITE_QUIT_PICTURE, RECOMP_RUNNER_SPRITE_QUIT_WIDE};
    for (unsigned i = 0u; i < 2u; ++i) {
        recomp_runner_sprite(pictures[i], PICTURE_LOG, PICTURE_LOG, palette);
        recomp_runner_sprite_size(pictures[i], QUIT_ICON_SIZE, QUIT_ICON_SIZE);
        write_picture(pictures[i], i == 0u ? 1.0f : 0.75f);
    }
    recomp_runner_text_set(RECOMP_RUNNER_TEXT_QUIT, HELP);
}

/* ---- The carousel --------------------------------------------------- */

static void menu_build(void)
{
    sub_00134330();
    quit_position = -1;
    const uint8_t count = *recomp_memory(MENU_COUNT, 1u);
    if (count >= 0xffu) return;
    quit_position = count;
    *recomp_memory(MENU_COUNT, 1u) = (uint8_t)(count + 1u);
}

/* Both readers clamp the position to the count, as the game's do. */
static int clamped(uint32_t position)
{
    const uint8_t count = *recomp_memory(MENU_COUNT, 1u), p = (uint8_t)position;
    return p > count ? count : p;
}

static void menu_entry(void)
{
    if (quit_position >= 0 && clamped(kernel_arg(1u)) == quit_position) {
        RecompGameSprite word;
        if (!label_made) label_made = make_label();
        else if (recomp_game_sprite(PIECES[0].sprite, WORD_H, &word)) /* reloaded art */
            recomp_runner_sprite_palette(RECOMP_RUNNER_SPRITE_QUIT_LABEL, word.palette);
        kernel_return_caller_cleanup(entry);
        return;
    }
    sub_001343A0();
}

static void menu_mode(void)
{
    if (quit_position >= 0 && clamped(*recomp_memory(kernel_arg(1u) + 0x120u, 1u)) == quit_position) {
        kernel_return_caller_cleanup(QUIT_MODE);
        return;
    }
    sub_001343D0();
}

static void menu_help(void)
{
    if (kernel_arg(1u) == QUIT_MODE) kernel_return_caller_cleanup(RECOMP_RUNNER_TEXT_QUIT);
    else sub_001345C0();
}

/* The confirm on Quit Game asks through the runner's quit prompt. */
static void play_sound(void)
{
    if (in_update && kernel_arg(1u) == SOUND_CONFIRM && !asking && quit_position >= 0 &&
        u32(SELECTED_MODE) == QUIT_MODE) {
        asking = true;
        recomp_quit_prompt_request();
        fprintf(stderr, "recomp menu quit: asking\n");
    }
    sub_000FED70();
}

/* The confirm started the carousel's leave (0x127840 with 2), as before any
   new scene. Kept playing, the game's own transition cancel brings it back on
   the same entry, with its help line; quitting, the runner stops. */
static void menu_update(void)
{
    in_update = true;
    sub_0013B850();
    in_update = false;
    if (!asking || recomp_quit_prompt_open()) return;
    const uint32_t eax = recomp_runtime.registers.eax, sound = SOUND_CANCEL;
    asking = false;
    kernel_call_lifted(sub_000FED70, &sound, 1u);
    kernel_call_lifted(sub_00135AE0, NULL, 0u);
    recomp_runtime.registers.eax = eax;
}

RecompFunction recomp_menu_quit_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case MENU_BUILD: return menu_build;
    case MENU_ENTRY: return menu_entry;
    case MENU_MODE: return menu_mode;
    case MENU_HELP: return menu_help;
    case MENU_UPDATE: return menu_update;
    case PLAY_SOUND: return play_sound;
    default: return NULL;
    }
}
