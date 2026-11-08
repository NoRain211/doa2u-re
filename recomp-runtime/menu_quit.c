/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "menu_quit.h"
#include "d3d8_adapter.h"
#include "quit_icon.h"
#include "runner_assets.h"
#include "stop_report.h"
#include "xbox_memory_layout.h"

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
   the switch ignores, so the confirm sound opens the popup instead. The popup draws with the
   Options screen's quads (0x976f0) and text (0x978e0) on layers 2 to 0 (the
   2D queue draws layer 9 first); while it is open the host pads go to it
   and the game sees them idle, as the runner's own quit prompt does. */

extern void sub_00134330(void);
extern void sub_001343A0(void);
extern void sub_001343D0(void);
extern void sub_001345C0(void);
extern void sub_0013B850(void);
extern void sub_000FED70(void);
extern void sub_000976F0(void);
extern void sub_000978E0(void);
extern void sub_000212E0(void);
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
    SPRITE_DEFS = 0x00686698u,
    SPRITE_TEXTURES = 0x0067ce78u,

    /* Before the switch, the confirm checks the mode's slot at 0xdf5d70
       (0x134570 fills modes 0-0x18 with 2, mode 5 by the pads connected) and
       plays the "not available" sound for anything but 2. Mode 0x13 is in
       that table, always 2, no entry's, and the switch does nothing for it. */
    QUIT_MODE = 0x13,
    SOUND_MOVE = 0x23, SOUND_CONFIRM = 0x24, SOUND_CANCEL = 0x29,
    QUIT_FRAMES = 30,               /* the confirm sound plays before the runner stops */
    HELP_TEXT = RECOMP_RUNNER_TEXT_FIRST,
};

/* The guest block, from the game's heap at each boot. */
enum {
    BLOCK_SIZE = 0xa000u,
    ENTRY = 0x000u,         /* 28-byte menu entry */
    RECTS = 0x020u,         /* label, picture, wide picture: 12 bytes each */
    ENTRIES = 0x060u,       /* their texture entries */
    HEADERS = 0x080u,       /* three textures of 20 bytes, then the picture palette's 12 */
    COLOURS = 0x0d0u,       /* four corner colours per quad: box, highlight */
    TEXT = 0x100u,
    TEXT_END = 0x800u,
    PALETTE = 0x800u,       /* the pictures' 256 ARGB entries */
    LABEL = 0x1000u,        /* P8 128x32, in the menu labels' own palette */
    PICTURE = 0x2000u,      /* P8 128x128 */
    WIDE = 0x6000u,         /* P8 128x128 */
    LABEL_W = 128, LABEL_H = 32, WORD_H = 24,
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

/* The popup's text per game language (Japanese reads English). */
typedef struct Words { const recomp_utf16 *title, *yes, *no, *help; } Words;
static const Words WORDS[5] = {
    {u"Quit the game?", u"Yes", u"No", u"Quit the game and return to the desktop."},
    {u"Quitter le jeu ?", u"Oui", u"Non", u"Quitter le jeu et revenir au bureau."},
    {u"Spiel beenden?", u"Ja", u"Nein", u"Das Spiel beenden und zum Desktop zurückkehren."},
    {u"Uscire dal gioco?", u"Sì", u"No", u"Esci dal gioco e torna al desktop."},
    {u"¿Salir del juego?", u"Sí", u"No", u"Salir del juego y volver al escritorio."},
};
static uint32_t word_text[5][4];

/* ---- State -------------------------------------------------------- */

enum { CLOSED, OPEN, QUITTING };
static uint32_t block;           /* 0 when the heap had no room: no Quit Game */
static int quit_position = -1;   /* the carousel position, once listed */
static bool in_update, label_made;
static int state = CLOSED;
static unsigned choice;          /* 0 Yes, 1 No */
static unsigned quit_frames;
/* Host pad events since the last menu frame; set on the input side. */
static volatile unsigned pending_toggle, pending_yes, pending_no;
static uint16_t held[RECOMP_INPUT_PORT_COUNT];
static bool latched[RECOMP_INPUT_PORT_COUNT]; /* swallowed until let go */

/* ---- Guest helpers -------------------------------------------------- */

static uint32_t arg(unsigned i) { return *recomp_memory_u32(recomp_runtime.registers.esp + 4u * i); }
static void set_u16(uint32_t address, uint16_t v) { memcpy(recomp_memory(address, 2u), &v, 2u); }
static uint16_t u16(uint32_t address) { uint16_t v; memcpy(&v, recomp_memory(address, 2u), 2u); return v; }
static void set_u32(uint32_t address, uint32_t v) { *recomp_memory_u32(address) = v; }
static uint32_t u32(uint32_t address) { return *recomp_memory_u32(address); }
static uint32_t f32(float f) { uint32_t v; memcpy(&v, &f, 4u); return v; }

static uint32_t call(void (*function)(void), const uint32_t *args, unsigned count)
{
    const uint32_t saved = recomp_runtime.registers.esp;
    uint32_t esp = saved;
    for (unsigned i = count; i > 0u; --i) {
        esp -= 4u;
        set_u32(esp, args[i - 1u]);
    }
    esp -= 4u;
    set_u32(esp, 0u);
    recomp_runtime.registers.esp = esp;
    function();
    recomp_runtime.registers.esp = saved;
    return recomp_runtime.registers.eax;
}

static void finish(uint32_t eax)
{
    recomp_runtime.registers.eax = eax;
    recomp_runtime.registers.esp += 4u;
}

static unsigned language_index(void)
{
    const unsigned language = call(sub_000212E0, NULL, 0u) & 0xffu;
    return language >= 2u && language <= 5u ? language - 1u : 0u;
}

static uint32_t put_text(uint32_t *cursor, const recomp_utf16 *text)
{
    const uint32_t start = *cursor;
    size_t n = 0u;
    while (text[n] != 0u) ++n;
    if (start + 2u * (n + 1u) > block + TEXT_END) return 0u;
    for (size_t i = 0u; i <= n; ++i) set_u16(start + 2u * (uint32_t)i, (uint16_t)text[i]);
    *cursor = start + 2u * (uint32_t)(n + 1u);
    return start;
}

static uint32_t swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t index = 0u, bit = 0u;
    while (w > 1u || h > 1u) {
        if (w > 1u) { index |= (x & 1u) << bit++; x >>= 1; w >>= 1; }
        if (h > 1u) { index |= (y & 1u) << bit++; y >>= 1; h >>= 1; }
    }
    return index;
}

/* ---- The pictures --------------------------------------------------- */

/* The pictures' palette: 0 clear; 8-63 eight alphas by eight greys for
   edges and the shadow; 64-255 opaque greys. */
static uint8_t picture_index(uint8_t grey, uint8_t alpha)
{
    if (alpha >= 250u) return (uint8_t)(64u + (grey * 191u + 127u) / 255u);
    const unsigned a = (alpha * 7u + 127u) / 255u;
    return a == 0u ? 0u : (uint8_t)(a * 8u + (grey * 7u + 127u) / 255u);
}

static void write_picture(uint32_t pixels, float squeeze)
{
    static uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE], alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE];
    recomp_quit_icon_draw(squeeze, grey, alpha);
    for (uint32_t y = 0u; y < QUIT_ICON_SIZE; ++y)
        for (uint32_t x = 0u; x < QUIT_ICON_SIZE; ++x)
            *recomp_memory(pixels + swizzle(x, y, QUIT_ICON_SIZE, QUIT_ICON_SIZE), 1u) =
                picture_index(grey[y * QUIT_ICON_SIZE + x], alpha[y * QUIT_ICON_SIZE + x]);
}

static void write_texture(uint32_t header, uint32_t pixels, unsigned log_w, unsigned log_h)
{
    set_u32(header, 0x00040001u);
    set_u32(header + 4u, pixels);
    set_u32(header + 8u, 0u);
    set_u32(header + 12u, log_h << 24 | log_w << 20 | 1u << 16 | 0x0bu << 8 | 0x2au); /* P8, swizzled */
    set_u32(header + 16u, 0u);
}

static void write_rect(unsigned i, int16_t right, int16_t bottom, unsigned slot)
{
    const int16_t rect[4] = {0, 0, right, bottom};
    memcpy(recomp_memory(block + RECTS + 12u * i, 8u), rect, 8u);
    set_u16(block + RECTS + 12u * i + 8u, (uint16_t)slot);
    set_u16(block + RECTS + 12u * i + 10u, RECOMP_RUNNER_SPRITE_GROUP);
}

/* ---- The label ------------------------------------------------------ */

/* One of the menu's words: its P8 texture, palette and rectangle. */
typedef struct Word { uint32_t pixels, palette, width, height; int left, top, right, bottom; } Word;

static bool word_for(uint32_t sprite, Word *word)
{
    const uint32_t group = sprite >> 16;
    const uint32_t defs = u32(SPRITE_DEFS + 4u * group), textures = u32(SPRITE_TEXTURES + 4u * group);
    if (defs == 0u || textures == 0u) return false;
    const uint32_t def = defs + 12u * (sprite & 0xffffu);
    int16_t rect[4];
    memcpy(rect, recomp_memory(def, 8u), 8u);
    const uint32_t entry = textures + 8u * u16(def + 8u);
    const uint32_t texture = u32(entry), palette = u32(entry + 4u);
    if (texture == 0u || palette == 0u) return false;
    const uint32_t format = u32(texture + 12u);
    if (((format >> 8) & 0xffu) != 0x0bu) return false;
    *word = (Word){u32(texture + 4u) & 0x03ffffffu, palette, 1u << ((format >> 20) & 0xfu),
        1u << ((format >> 24) & 0xfu), rect[0], rect[1], rect[2], rect[3]};
    return word->right > word->left && word->bottom - word->top == WORD_H &&
        (uint32_t)word->right <= word->width && (uint32_t)word->bottom <= word->height;
}

static uint8_t word_index(const Word *w, int x, int y)
{
    return *recomp_memory(w->pixels + swizzle((uint32_t)(w->left + x), (uint32_t)(w->top + y), w->width, w->height), 1u);
}

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
    Word first;
    if (!word_for(PIECES[0].sprite, &first)) return false;
    const uint32_t palette_data = u32(first.palette + 4u) & 0x03ffffffu;
    static uint8_t image[LABEL_H][LABEL_W];
    memset(image, 0, sizeof image);
    float pen = LEAN * (float)WORD_H, letter_pen = pen;
    for (size_t p = 0u; p < sizeof PIECES / sizeof PIECES[0]; ++p) {
        const Piece *piece = &PIECES[p];
        if (piece->c == u' ') {
            pen += piece->to / 10.0f;
            continue;
        }
        Word word;
        if (!word_for(piece->sprite, &word) || word.palette != first.palette) return false;
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
                uint8_t index = word_index(&word, x, y);
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
    for (uint32_t y = 0u; y < LABEL_H; ++y)
        for (uint32_t x = 0u; x < LABEL_W; ++x)
            *recomp_memory(block + LABEL + swizzle(x, y, LABEL_W, LABEL_H), 1u) = image[y][x];
    write_rect(0u, (int16_t)width, WORD_H, RECOMP_RUNNER_SPRITE_QUIT_LABEL);
    set_u32(block + ENTRIES + 4u, first.palette);   /* the label draws in the menu labels' palette */
    recomp_d3d8_invalidate_textures(block + LABEL, LABEL_W * LABEL_H);
    fprintf(stderr, "recomp menu quit: label made, %d pixels wide\n", width);
    return true;
}

/* ---- Install -------------------------------------------------------- */

void recomp_menu_quit_install(void)
{
    quit_position = -1;
    in_update = label_made = false;
    state = CLOSED;
    pending_toggle = pending_yes = pending_no = 0u;
    memset(held, 0, sizeof held);
    memset(latched, 0, sizeof latched);
    block = xbox_HeapAlloc(BLOCK_SIZE, 0x1000u);
    if (block == 0u) {
        fprintf(stderr, "recomp menu quit: no guest memory; no Quit Game\n");
        return;
    }
    memset(recomp_memory(block, BLOCK_SIZE), 0, BLOCK_SIZE);
    const uint32_t entry[7] = {QUIT_MODE, recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_QUIT_LABEL),
        recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_QUIT_PICTURE), recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_QUIT_WIDE),
        0u, 0u, 0u};
    for (unsigned i = 0u; i < 7u; ++i) set_u32(block + ENTRY + 4u * i, entry[i]);
    const unsigned slots[3] = {RECOMP_RUNNER_SPRITE_QUIT_LABEL, RECOMP_RUNNER_SPRITE_QUIT_PICTURE, RECOMP_RUNNER_SPRITE_QUIT_WIDE};
    const uint32_t pixels[3] = {block + LABEL, block + PICTURE, block + WIDE};
    const uint32_t palette = block + HEADERS + 60u;
    set_u32(palette, 0x00830001u);
    set_u32(palette + 4u, block + PALETTE);
    set_u32(palette + 8u, 0u);
    for (unsigned i = 0u; i < 3u; ++i) {
        const uint32_t header = block + HEADERS + 20u * i;
        write_texture(header, pixels[i], 7u, i == 0u ? 5u : 7u);
        set_u32(block + ENTRIES + 8u * i, header);
        set_u32(block + ENTRIES + 8u * i + 4u, palette); /* the label's becomes the menu's at make_label */
        write_rect(i, i == 0u ? 1 : QUIT_ICON_SIZE, i == 0u ? WORD_H : QUIT_ICON_SIZE, slots[i]);
        recomp_runner_sprite_set(slots[i], block + RECTS + 12u * i, block + ENTRIES + 8u * i);
    }
    for (unsigned i = 0u; i < 256u; ++i) {
        uint32_t a, g;
        if (i >= 64u) { a = 255u; g = (i - 64u) * 255u / 191u; }
        else { a = (i / 8u) * 255u / 7u; g = (i % 8u) * 255u / 7u; }
        set_u32(block + PALETTE + 4u * i, a << 24 | g << 16 | g << 8 | g);
    }
    write_picture(block + PICTURE, 1.0f);
    write_picture(block + WIDE, 0.75f);
    /* The popup's quads: the Options background's greys, and a lighter bar. */
    const uint32_t box[4] = {0xe0202020u, 0xe0383838u, 0xe0202020u, 0xe0383838u};
    const uint32_t bar[4] = {0x70a0a0a0u, 0x70a0a0a0u, 0x70707070u, 0x70707070u};
    for (unsigned i = 0u; i < 4u; ++i) {
        set_u32(block + COLOURS + 4u * i, box[i]);
        set_u32(block + COLOURS + 16u + 4u * i, bar[i]);
    }
    uint32_t cursor = block + TEXT, help[6];
    for (unsigned l = 0u; l < 5u; ++l) {
        word_text[l][0] = put_text(&cursor, WORDS[l].title);
        word_text[l][1] = put_text(&cursor, WORDS[l].yes);
        word_text[l][2] = put_text(&cursor, WORDS[l].no);
        word_text[l][3] = put_text(&cursor, WORDS[l].help);
    }
    help[0] = word_text[0][3];
    for (unsigned l = 0u; l < 5u; ++l) help[l + 1u] = word_text[l][3];
    recomp_runner_text_set(HELP_TEXT, help);
}

/* ---- The carousel --------------------------------------------------- */

static void menu_build(void)
{
    sub_00134330();
    quit_position = -1;
    if (block == 0u) return;
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
    if (quit_position >= 0 && clamped(arg(1u)) == quit_position) {
        Word word;
        if (!label_made) label_made = make_label();
        else if (word_for(PIECES[0].sprite, &word)) set_u32(block + ENTRIES + 4u, word.palette); /* reloaded art */
        finish(block + ENTRY);
        return;
    }
    sub_001343A0();
}

static void menu_mode(void)
{
    if (quit_position >= 0 && clamped(*recomp_memory(arg(1u) + 0x120u, 1u)) == quit_position) {
        finish(QUIT_MODE);
        return;
    }
    sub_001343D0();
}

static void menu_help(void)
{
    if (arg(1u) == QUIT_MODE) finish(HELP_TEXT);
    else sub_001345C0();
}

static void sound(uint32_t id) { call(sub_000FED70, &id, 1u); }

static void play_sound(void)
{
    /* The confirm on Quit Game: the popup opens on No. */
    if (in_update && arg(1u) == SOUND_CONFIRM && state == CLOSED && quit_position >= 0 &&
        u32(SELECTED_MODE) == QUIT_MODE) {
        state = OPEN;
        choice = 1u;
        fprintf(stderr, "recomp menu quit: popup opened\n");
        pending_toggle = pending_yes = pending_no = 0u;
        for (unsigned i = 0u; i < RECOMP_INPUT_PORT_COUNT; ++i) latched[i] = true; /* the A that opened it */
    }
    sub_000FED70();
}

static void quad(int x, int y, int w, int h, uint32_t colours, unsigned layer)
{
    const uint32_t args[7] = {(uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, colours, layer, 0u};
    call(sub_000976F0, args, 7u);
}

/* 0x978e0(text, x, y, layer, alpha, brightness, alignment, scale). Alignment
   0 starts the text at x (the Options values), 2 ends it there (the Options
   labels), 1 centres it on x. */
enum { ALIGN_CENTRE = 1 };
static void text(uint32_t address, int x, int y, float brightness, float scale)
{
    if (address == 0u) return;
    const uint32_t args[8] = {address, (uint32_t)x, (uint32_t)y, 0u, f32(1.0f), f32(brightness), ALIGN_CENTRE, f32(scale)};
    call(sub_000978E0, args, 8u);
}

/* The box centred on the 720x480 screen (0xabcb0's centre anchor is 360,
   240); the title centred in it, and Yes and No centred under it with equal
   highlights centred on each. */
static void draw_popup(void)
{
    enum { X = 216, Y = 188, W = 288, H = 104, CX = X + W / 2, OPTION_Y = 244,
           YES_X = CX - 64, NO_X = CX + 64, BAR_W = 104, BAR_H = 24 };
    const unsigned l = language_index();
    quad(X, Y, W, H, block + COLOURS, 2u);
    quad((choice == 0u ? YES_X : NO_X) - BAR_W / 2, OPTION_Y - 4, BAR_W, BAR_H, block + COLOURS + 16u, 1u);
    text(word_text[l][0], CX, Y + 18, 1.0f, 1.0f);
    text(word_text[l][1], YES_X, OPTION_Y, choice == 0u ? 1.0f : 0.5f, 0.9f);
    text(word_text[l][2], NO_X, OPTION_Y, choice == 1u ? 1.0f : 0.5f, 0.9f);
}

/* The confirm started the carousel's leave (0x127840 with 2), as before any
   new scene; staying, the game's own transition cancel brings it back on the
   same entry, with its help line. */
static void close_popup(const char *why)
{
    fprintf(stderr, "recomp menu quit: popup closed (%s)\n", why);
    state = CLOSED;
    sound(SOUND_CANCEL);
    call(sub_00135AE0, NULL, 0u);
}

static void menu_update(void)
{
    in_update = true;
    sub_0013B850();
    in_update = false;
    const uint32_t eax = recomp_runtime.registers.eax;
    if (state == OPEN) {
        const unsigned toggles = pending_toggle, yes = pending_yes, no = pending_no;
        pending_toggle = pending_yes = pending_no = 0u;
        if (no != 0u) {
            close_popup("B");
        } else if (yes != 0u) {
            if (choice == 0u) {
                state = QUITTING;
                quit_frames = QUIT_FRAMES;
                sound(SOUND_CONFIRM);
            } else {
                close_popup("No");
            }
        } else if (toggles != 0u) {
            if (toggles & 1u) choice ^= 1u;
            sound(SOUND_MOVE);
        }
    }
    if (state == OPEN) draw_popup();
    if (state == QUITTING && --quit_frames == 0u) {
        fprintf(stderr, "recomp menu quit: Quit Game chosen\n");
        recomp_stop(0, "player-quit");
    }
    recomp_runtime.registers.eax = eax;
}

/* ---- Host pads ------------------------------------------------------ */

enum { DPAD_UP = 0x1u, DPAD_DOWN = 0x2u, DPAD_LEFT = 0x4u, DPAD_RIGHT = 0x8u, STICK = 16000 };

static bool pad_idle(const RecompInputGamepad *pad)
{
    if (pad->buttons != 0u) return false;
    for (unsigned i = 0u; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i)
        if (pad->analog_buttons[i] > RECOMP_INPUT_ANALOG_DOWN) return false;
    return pad->thumb_lx < 7849 && pad->thumb_lx > -7849 && pad->thumb_ly < 7849 && pad->thumb_ly > -7849;
}

/* Directions, A and B as bits, the stick counting as the d-pad. */
static uint16_t pad_bits(const RecompInputGamepad *pad)
{
    uint16_t bits = pad->buttons & (DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT);
    if (pad->thumb_lx < -STICK) bits |= DPAD_LEFT;
    if (pad->thumb_lx > STICK) bits |= DPAD_RIGHT;
    if (pad->thumb_ly > STICK) bits |= DPAD_UP;
    if (pad->thumb_ly < -STICK) bits |= DPAD_DOWN;
    if (pad->analog_buttons[0] > RECOMP_INPUT_ANALOG_DOWN) bits |= 0x100u; /* A */
    if (pad->analog_buttons[1] > RECOMP_INPUT_ANALOG_DOWN) bits |= 0x200u; /* B */
    return bits;
}

void recomp_menu_quit_filter(uint32_t port, RecompInputGamepad *pad)
{
    if (port >= RECOMP_INPUT_PORT_COUNT || pad == NULL) return;
    if (state == CLOSED && !latched[port]) return;
    if (latched[port]) {
        if (pad_idle(pad)) latched[port] = false;
        held[port] = pad_bits(pad);
        memset(pad, 0, sizeof *pad);
        return;
    }
    const uint16_t bits = pad_bits(pad), pressed = (uint16_t)(bits & ~held[port]);
    held[port] = bits;
    if (state == OPEN) {
        if (pressed & 0x200u) pending_no = 1u;
        else if (pressed & 0x100u) pending_yes = 1u;
        else if (pressed & (DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT)) pending_toggle ^= 1u;
        /* A press that closes the popup must not reach the menu after it. */
        if (pressed & 0x300u) for (unsigned i = 0u; i < RECOMP_INPUT_PORT_COUNT; ++i) latched[i] = true;
    }
    memset(pad, 0, sizeof *pad);
}

bool recomp_menu_quit_open(void) { return state != CLOSED; }

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
