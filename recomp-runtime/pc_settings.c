/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings.h"
#include "d3d8_adapter.h"
#include "runner_assets.h"
#include "xbox_memory_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The game's Options menu (frontend_options_menu_update, 0x11cfc0) keeps a
   cursor per depth at 0xdf4ac8, the depth at 0xdf4ad4 and the page at 0xdf4ad8.
   Page 0 is the top list: Game Settings, Audio, Language, Controller, Video,
   Warning and Exit, each a sprite of group 3 (spOption) drawn by 0x95a70 at
   x 0xf0, y 0x5d + 26 * row, the selected one also as a glow sprite at x 0xf6,
   y 0x56 + 26 * row. 0x11ac90 gives each page's row count. The other pages
   are drawn from three tables indexed by page (0x67af50 resets a page's
   layout, 0x67aff0 draws it, 0x67afa0 draws it leaving) through 0x91440
   (labels), 0x93270 (values) and 0x90f00 (cursor), which read the page's
   layout records (0x90ad0), labels and value text (0x910a0 and 0x91370) and
   row count.

   PC SETTINGS becomes top-list row 5, after Video, and opens page 19, which
   no table uses. Its label is made from letters cut out of the other labels'
   own art (group 3), with the same blue glow, into a sprite group of its own;
   its text goes through a message range of its own in 0x6a3e0. */
enum {
    PC_ROW = 5,
    TOP_ROWS = 8,
    PC_PAGE = 19,
    PC_ROWS = RECOMP_PC_SETTING_COUNT,
    PC_GROUP = RECOMP_RUNNER_SPRITE_GROUP, /* runner_assets.c answers the game's lookups for it */
    PC_MESSAGES = 0x7000,          /* message numbers the game never uses */
    VALUE_KIND = 0x7e00,           /* 0x910a0 keys for each row's values, outside its 1-28 */

    CURSORS = 0x00df4ac8u,
    MENU_STATE = 0x00df4ad0u,
    DEPTH = 0x00df4ad4u,
    PAGE = 0x00df4ad8u,
    CONTROLLER_STATE = 0x00df4adcu,
    PAGE_START = 0x0067af00u,      /* the five per-page tables of 0x95a70 */
    PAGE_ENTER = 0x0067af50u,
    PAGE_LEAVE = 0x0067afa0u,
    PAGE_DRAW = 0x0067aff0u,
    PAGE_IN = 0x0067b040u,
    SPRITE_DEFS = 0x00686698u,     /* per group: 12-byte rectangles */
    SPRITE_TEXTURES = 0x0067ce78u, /* per group: texture, palette */
    LAYOUT_TEMPLATE = 0x0067a540u, /* page 8's layout */

    OPTIONS_UPDATE = 0x0011cfc0u,
    ROW_COUNT = 0x0011ac90u,
    ROW_COUNT_ALIAS = 0x0011ad10u,
    OPTIONS_DRAW = 0x00095a70u,
    TOP_PREVIEW = 0x00095a10u,
    SPRITE = 0x00097830u,
    QUAD = 0x000976f0u,
    HELP = 0x00091560u,
    MESSAGE = 0x0006a3e0u,
    PAGE_LAYOUT = 0x00090ad0u,
    PAGE_TEXT = 0x000910a0u,
    VALUE_TEXT = 0x00091370u,

    /* Page 19's entries: lookup keys, never read as memory. */
    PC_ENTER = 0x03fffff0u,
    PC_DRAW = 0x03fffff4u,
    PC_LEAVE = 0x03fffff8u,
};

/* The runner's guest asset area (xbox_memory_layout.h). A page layout is a
   record per row (x 0x10a), then one per row with a value (x 0x1e0), then a
   zero word; the cursor (0x90f00) walks and animates all of them, so the
   layout comes after the sprite data, with room to spare. */
enum {
    VALUES = XBOX_RUNNER_ASSET_BASE,             /* u16 per row */
    DEFS = XBOX_RUNNER_ASSET_BASE + 0x40u,       /* white, glow */
    ENTRIES = XBOX_RUNNER_ASSET_BASE + 0x60u,    /* texture, palette per sprite */
    HEADERS = XBOX_RUNNER_ASSET_BASE + 0x80u,    /* 2 textures, 2 palettes */
    LAYOUT = XBOX_RUNNER_ASSET_BASE + 0x100u,    /* 2 * PC_ROWS records of 28, then 0 */
    LAYOUT_RECORDS = 2u * RECOMP_PC_SETTING_COUNT,
    TEXT = XBOX_RUNNER_ASSET_BASE + 0x400u,
    TEXT_END = XBOX_RUNNER_ASSET_BASE + 0x3000u,
    PALETTES = XBOX_RUNNER_ASSET_BASE + 0x3000u, /* white, glow; 1 KiB each */
    WHITE = XBOX_RUNNER_ASSET_BASE + 0x4000u,    /* P8 256x32 */
    GLOW = XBOX_RUNNER_ASSET_BASE + 0x6000u,     /* P8 256x32 */
    LABEL_W = 256,
    LABEL_H = 32,
    WHITE_H = 18,
};

extern void sub_0011CFC0(void);
extern void sub_0011AC90(void);
extern void sub_0011AD10(void);
extern void sub_0011B8D0(void);
extern void sub_0011BCA0(void);
extern void sub_0011BDA0(void);
extern void sub_0011B9F0(void);
extern void sub_0011CF20(void);
extern void sub_00134950(void);
extern void sub_00134F50(void);
extern void sub_00020A70(void);
extern void sub_000212E0(void);
extern void sub_00095A70(void);
extern void sub_00095A10(void);
extern void sub_00097830(void);
extern void sub_000976F0(void);
extern void sub_00091560(void);
extern void sub_0006A3E0(void);
extern void sub_000AA970(void);
extern void sub_000AA9B0(void);
extern void sub_00090AD0(void);
extern void sub_000910A0(void);
extern void sub_00091370(void);
extern void sub_00091440(void);
extern void sub_00093270(void);
extern void sub_00090F00(void);

/* ---- Text ------------------------------------------------------------ */

static const uint8_t CHOICES[PC_ROWS] = {5u, 6u, 2u, 5u, 2u, 2u, 2u, 2u, 2u, 2u, 2u};
static const bool ALWAYS_LIVE[PC_ROWS] = {
    [RECOMP_PC_UNLOCKS] = true, [RECOMP_PC_CPU_COSTUMES] = true, [RECOMP_PC_FREE_CAMERA] = true,
};
static bool live[PC_ROWS];
enum { MAX_CHOICES = 6, HELP_TOP = 0, HELP_NOW = 1, HELP_NEXT = 2, HELPS = 3 };

typedef struct Language {
    const recomp_utf16 *label;  /* the top-list row, in capitals */
    const recomp_utf16 *rows[PC_ROWS];
    const recomp_utf16 *values[PC_ROWS][MAX_CHOICES];
    const recomp_utf16 *help[HELPS];
} Language;

#define RESOLUTIONS u"480p", u"720p", u"1080p", u"1440p", u"4K"
#define SAMPLES(off) off, u"2x", u"4x", u"8x", u"16x", u"32x"
#define SHAPES(wide) u"4:3", u"16:9", u"16:10", wide, u"32:9"
#define RENDERERS u"Direct3D 11", u"Vulkan"

/* English, French, German, Italian and Spanish; Japanese shows English. */
static const Language LANGUAGES[5] = {
    {u"PC SETTINGS",
     {u"Resolution", u"MSAA", u"SMAA", u"Screen", u"Renderer", u"VRR", u"Audio Output",
      u"Intro Movies", u"Unlocks", u"CPU Costumes", u"Free Camera"},
     {{RESOLUTIONS}, {SAMPLES(u"Off")}, {u"Off", u"On"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"Off", u"On"}, {u"Stereo", u"5.1 Surround"}, {u"Play", u"Skip"}, {u"Everything", u"Earn"},
      {u"Default", u"Random"}, {u"Off", u"On"}},
     {u"Graphics, audio and gameplay settings for this PC.", u"Takes effect immediately.",
      u"Takes effect the next time the game starts."}},
    {u"PARAM\u00c8TRES PC",
     {u"R\u00e9solution", u"MSAA", u"SMAA", u"\u00c9cran", u"Rendu", u"VRR", u"Sortie audio",
      u"Vid\u00e9os d'intro", u"D\u00e9blocages", u"Costumes CPU", u"Cam\u00e9ra libre"},
     {{RESOLUTIONS}, {SAMPLES(u"Non")}, {u"Non", u"Oui"}, {SHAPES(u"Ultra-large")}, {RENDERERS},
      {u"Non", u"Oui"}, {u"St\u00e9r\u00e9o", u"5.1 Surround"}, {u"Lire", u"Passer"},
      {u"Tout", u"\u00c0 gagner"}, {u"Normaux", u"Al\u00e9atoires"}, {u"Non", u"Oui"}},
     {u"Voici les param\u00e8tres graphiques, audio et de jeu de ce PC.",
      u"Pris en compte imm\u00e9diatement.", u"Pris en compte au prochain lancement du jeu."}},
    {u"PC-EINSTELLUNGEN",
     {u"Aufl\u00f6sung", u"MSAA", u"SMAA", u"Bildschirm", u"Renderer", u"VRR", u"Audioausgabe",
      u"Intro-Videos", u"Freischaltungen", u"CPU-Kost\u00fcme", u"Freie Kamera"},
     {{RESOLUTIONS}, {SAMPLES(u"Aus")}, {u"Aus", u"Ein"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"Aus", u"Ein"}, {u"Stereo", u"5.1 Surround"}, {u"Abspielen", u"\u00dcberspringen"},
      {u"Alles", u"Erspielen"}, {u"Standard", u"Zuf\u00e4llig"}, {u"Aus", u"Ein"}},
     {u"Das sind die Grafik-, Audio- und Spieleinstellungen f\u00fcr diesen PC.",
      u"Wirkt sofort.", u"Wirkt beim n\u00e4chsten Spielstart."}},
    {u"IMPOSTAZIONI PC",
     {u"Risoluzione", u"MSAA", u"SMAA", u"Schermo", u"Renderer", u"VRR", u"Uscita audio",
      u"Filmati iniziali", u"Sblocchi", u"Costumi CPU", u"Telecamera libera"},
     {{RESOLUTIONS}, {SAMPLES(u"No")}, {u"No", u"S\u00ec"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"No", u"S\u00ec"}, {u"Stereo", u"5.1 Surround"}, {u"Riproduci", u"Salta"},
      {u"Tutto", u"Da sbloccare"}, {u"Predefiniti", u"Casuali"}, {u"No", u"S\u00ec"}},
     {u"Impostazioni grafiche, audio e di gioco di questo PC.", u"Effettivo immediatamente.",
      u"Effettivo al prossimo avvio del gioco."}},
    {u"CONFIG. PC",
     {u"Resoluci\u00f3n", u"MSAA", u"SMAA", u"Pantalla", u"Renderizador", u"VRR", u"Salida de audio",
      u"V\u00eddeos de inicio", u"Desbloqueos", u"Trajes CPU", u"C\u00e1mara libre"},
     {{RESOLUTIONS}, {SAMPLES(u"No")}, {u"No", u"S\u00ed"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"No", u"S\u00ed"}, {u"Est\u00e9reo", u"5.1 Surround"}, {u"Reproducir", u"Saltar"},
      {u"Todo", u"Ganar"}, {u"Normales", u"Aleatorios"}, {u"No", u"S\u00ed"}},
     {u"Opciones de gr\u00e1ficos, audio y juego de este PC.", u"Se aplica de inmediato.",
      u"Se aplica la pr\u00f3xima vez que inicies el juego."}},
};

/* The game's language (0x212e0): Japanese, English, French, German, Italian, Spanish. */
static const Language *language_text(unsigned language)
{
    return &LANGUAGES[language >= 2u && language <= 5u ? language - 1u : 0u];
}

/* Message numbers: rows, then each row's values, then the help lines. */
enum { MSG_ROWS = 0, MSG_VALUES = PC_ROWS, MSG_HELP = MSG_VALUES + PC_ROWS * MAX_CHOICES, MSG_COUNT = MSG_HELP + HELPS };
static uint32_t text_address[5][MSG_COUNT];

/* The top list's sprites in each language: white rows, then glows. */
static const uint32_t WHITE_BASE[6] = {0x3000du, 0x3000du, 0x3001bu, 0x30029u, 0x30045u, 0x30037u};
static const uint32_t GLOW_BASE[6] = {0x30006u, 0x30006u, 0x30014u, 0x30022u, 0x3003eu, 0x30030u};

/* ---- State -------------------------------------------------------- */

static RecompPcSettingsHost host;
static bool drawing_top;
static bool drawing_pc;          /* page 19's own draw is running */
static uint32_t help_message;    /* replaces 0xc4 while the help line draws */
static int label_language = -1; /* the language the label sprites were made in */

/* ---- Guest helpers -------------------------------------------------- */

static uint32_t arg(unsigned i) { return *recomp_memory_u32(recomp_runtime.registers.esp + 4u * i); }
static void set_arg(unsigned i, uint32_t value) { *recomp_memory_u32(recomp_runtime.registers.esp + 4u * i) = value; }
static uint16_t u16(uint32_t address) { uint16_t v; memcpy(&v, recomp_memory(address, 2u), 2u); return v; }
static void set_u16(uint32_t address, uint16_t v) { memcpy(recomp_memory(address, 2u), &v, 2u); }
static void set_u32(uint32_t address, uint32_t v) { *recomp_memory_u32(address) = v; }

/* A cdecl call straight into a lifted function, past the manual hooks. */
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

static uint32_t call1(void (*function)(void), uint32_t a) { return call(function, &a, 1u); }

static void finish(uint32_t eax)
{
    recomp_runtime.registers.eax = eax;
    recomp_runtime.registers.esp += 4u;
}

static unsigned game_language(void) { return call(sub_000212E0, NULL, 0u) & 0xffu; }

/* ---- Install -------------------------------------------------------- */

unsigned recomp_pc_settings_choices(unsigned setting) { return setting < PC_ROWS ? CHOICES[setting] : 0u; }
bool recomp_pc_settings_applies_now(unsigned setting) { return setting < PC_ROWS && live[setting]; }
void recomp_pc_settings_set_live(unsigned setting, bool on) { if (setting < PC_ROWS) live[setting] = on || ALWAYS_LIVE[setting]; }
uint8_t recomp_pc_settings_choice(unsigned setting) { return setting < PC_ROWS ? (uint8_t)u16(VALUES + 2u * setting) : 0u; }

static uint32_t put_text(uint32_t *cursor, const recomp_utf16 *text)
{
    const uint32_t address = *cursor;
    size_t n = 0u;
    while (text[n] != 0u) ++n;
    if (address + (n + 1u) * 2u > TEXT_END) return 0u;
    for (size_t i = 0u; i <= n; ++i) set_u16(address + 2u * (uint32_t)i, (uint16_t)text[i]);
    *cursor = (address + (uint32_t)(n + 1u) * 2u + 3u) & ~3u;
    return address;
}

static void reset_layout(void)
{
    for (unsigned record = 0u; record < LAYOUT_RECORDS; ++record) set_u32(LAYOUT + record * 28u + 0xcu, 0u);
}

void recomp_pc_settings_install(const RecompPcSettingsHost *with, const uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    host = with != NULL ? *with : (RecompPcSettingsHost){0};
    drawing_top = false;
    drawing_pc = false;
    help_message = 0u;
    label_language = -1;
    memcpy(live, ALWAYS_LIVE, sizeof live);
    memset(recomp_memory(XBOX_RUNNER_ASSET_BASE, XBOX_RUNNER_ASSET_SIZE), 0, XBOX_RUNNER_ASSET_SIZE);
    for (unsigned row = 0u; row < PC_ROWS; ++row)
        set_u16(VALUES + 2u * row, choices[row] < CHOICES[row] ? choices[row] : 0u);

    /* Page 8's row record (0) and value record (4), 26 apart, then the end. */
    for (unsigned record = 0u; record < LAYOUT_RECORDS; ++record) {
        const unsigned row = record % PC_ROWS;
        const bool value = record >= PC_ROWS;
        const uint32_t to = LAYOUT + 28u * record, at = (0x5du + 26u * row) << 16 | (value ? 0x1e0u : 0x10au);
        memcpy(recomp_memory(to, 28u), recomp_memory(LAYOUT_TEMPLATE + 28u * (value ? 4u : 0u), 28u), 28u);
        set_u32(to, at);
        set_u32(to + 4u, at);
    }
    set_u32(LAYOUT + 28u * LAYOUT_RECORDS, 0u);

    uint32_t cursor = TEXT;
    for (unsigned l = 0u; l < 5u; ++l) {
        const Language *text = &LANGUAGES[l];
        for (unsigned row = 0u; row < PC_ROWS; ++row) {
            text_address[l][MSG_ROWS + row] = put_text(&cursor, text->rows[row]);
            for (unsigned v = 0u; v < CHOICES[row]; ++v)
                text_address[l][MSG_VALUES + row * MAX_CHOICES + v] = put_text(&cursor, text->values[row][v]);
        }
        for (unsigned h = 0u; h < HELPS; ++h) text_address[l][MSG_HELP + h] = put_text(&cursor, text->help[h]);
    }

    /* Two P8 sprites (white label, glow) with their palettes. */
    for (unsigned i = 0u; i < 2u; ++i) {
        const uint32_t texture = HEADERS + 20u * i, palette = HEADERS + 40u + 12u * i;
        set_u32(texture, 0x00040001u);
        set_u32(texture + 4u, i == 0u ? WHITE : GLOW);
        set_u32(texture + 8u, 0u);
        set_u32(texture + 12u, 5u << 24 | 8u << 20 | 1u << 16 | 0x0bu << 8 | 0x2au); /* 256x32, P8, swizzled */
        set_u32(texture + 16u, 0u);
        set_u32(palette, 0x00830001u);
        set_u32(palette + 4u, PALETTES + 1024u * i);
        set_u32(palette + 8u, 0u);
        set_u32(ENTRIES + 8u * i, texture);
        set_u32(ENTRIES + 8u * i + 4u, palette);
        recomp_runner_sprite_set(RECOMP_RUNNER_SPRITE_PC_WHITE + i, DEFS + 12u * i, ENTRIES + 8u * i);
        for (unsigned a = 0u; a < 256u; ++a)
            set_u32(PALETTES + 1024u * i + 4u * a, a << 24 | (i == 0u ? 0xffffffu : 0x3990fau));
    }

    /* As page 8: start and enter reset the layout, draw shows the page, and
       in and leave draw it sliding. 0x95a70 waits on a null entry forever. */
    set_u32(PAGE_START + 4u * PC_PAGE, PC_ENTER);
    set_u32(PAGE_ENTER + 4u * PC_PAGE, PC_ENTER);
    set_u32(PAGE_DRAW + 4u * PC_PAGE, PC_DRAW);
    set_u32(PAGE_LEAVE + 4u * PC_PAGE, PC_LEAVE);
    set_u32(PAGE_IN + 4u * PC_PAGE, PC_LEAVE);
}

/* ---- The label sprites ---------------------------------------------- */

/* The top list's labels are pictures (group 3), not text, so PC SETTINGS is
   made from letters cut out of them: each letter is a slanted slice of one of
   the game's own words, copied whole-pixel beside the next. The glow is that
   label padded by 6 and 7, blurred (sigma 3.5) and brightened 2.52 times, in
   0x3990fa, fitted to the game's own glows. */
#define SLANT 0.18f   /* the labels lean right by 0.18 px per row */
#define SPACE 7.0f    /* a word space beyond the letters' own half gaps */
#define SIGMA 3.5f
#define GAIN 2.52f

/* Where each letter is in the game's labels: the word's sprite and the
   letter's slice along the row, in tenths of a pixel of u = x - lean(y). */
typedef struct Letter { recomp_utf16 c; uint32_t sprite; int16_t from, to; } Letter;
static const Letter LETTERS[] = {
    {u'-', 0x30029, 485, 540},  {u'.', 0x30037, 660, 715},  {u'A', 0x3000d, 145, 250},
    {u'C', 0x30010, 25, 140},   {u'D', 0x3000e, 255, 380},  {u'E', 0x3000d, 385, 485},
    {u'F', 0x30037, 370, 465},  {u'G', 0x3000d, 25, 145},   {u'H', 0x3002b, 440, 550},
    {u'I', 0x3000d, 965, 1035}, {u'J', 0x3001b, 1455, 1540}, {u'L', 0x3000f, 25, 125},
    {u'M', 0x3000d, 250, 385},  {u'N', 0x3000d, 1035, 1150}, {u'O', 0x3000e, 445, 575},
    {u'P', 0x3001b, 25, 140},   {u'Q', 0x30021, 25, 150},   {u'R', 0x30010, 465, 585},
    {u'S', 0x3000d, 555, 665},  {u'T', 0x3000d, 765, 865},  {u'U', 0x3000e, 140, 255},
    {u'V', 0x30011, 25, 130},   {u'W', 0x30012, 25, 170},   {u'X', 0x30013, 125, 235},
    {u'Z', 0x30045, 260, 365},  {0x00c8u, 0x3001b, 890, 985}, {0x00c9u, 0x3001f, 310, 400},
    {0x00cdu, 0x3003b, 120, 185},
};

static uint32_t swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t index = 0u, bit = 0u;
    while (w > 1u || h > 1u) {
        if (w > 1u) { index |= (x & 1u) << bit++; x >>= 1; w >>= 1; }
        if (h > 1u) { index |= (y & 1u) << bit++; y >>= 1; h >>= 1; }
    }
    return index;
}

/* One of the game's label words: its P8 texture, palette and rectangle. */
typedef struct Word { uint32_t pixels, palette, width, height; int left, top, right, bottom; } Word;

static bool word_for(uint32_t sprite, Word *word)
{
    const uint32_t group = sprite >> 16;
    const uint32_t def = *recomp_memory_u32(SPRITE_DEFS + 4u * group) + 12u * (sprite & 0xffffu);
    int16_t rect[4];
    memcpy(rect, recomp_memory(def, 8u), 8u);
    const uint32_t entry = *recomp_memory_u32(SPRITE_TEXTURES + 4u * group) + 8u * u16(def + 8u);
    const uint32_t texture = *recomp_memory_u32(entry), palette = *recomp_memory_u32(entry + 4u);
    if (texture == 0u || palette == 0u) return false;
    const uint32_t format = *recomp_memory_u32(texture + 12u);
    if (((format >> 8) & 0xffu) != 0x0bu) return false;
    *word = (Word){*recomp_memory_u32(texture + 4u) & 0x03ffffffu, *recomp_memory_u32(palette + 4u) & 0x03ffffffu,
        1u << ((format >> 20) & 0xfu), 1u << ((format >> 24) & 0xfu), rect[0], rect[1], rect[2], rect[3]};
    return word->right > word->left && word->bottom - word->top == WHITE_H &&
        word->right - word->left <= LABEL_W && (uint32_t)word->right <= word->width &&
        (uint32_t)word->bottom <= word->height;
}

static uint8_t word_alpha(const Word *w, int x, int y)
{
    const uint8_t index = *recomp_memory(w->pixels +
        swizzle((uint32_t)(w->left + x), (uint32_t)(w->top + y), w->width, w->height), 1u);
    return (uint8_t)(*recomp_memory_u32(w->palette + 4u * index) >> 24);
}

/* u of a pixel: x less the row's lean, so a letter's slice is u0 <= u < u1. */
static float lean_u(int x, int y) { return (float)x - SLANT * (float)(WHITE_H - 1 - y) + SLANT * (float)WHITE_H; }

/* Copies one letter at pen: the pixels of its slice, less any blob that lies
   mostly outside the slice (a neighbour's corner). */
static void put_letter(const Word *w, float u0, float u1, float pen, float white[LABEL_H][LABEL_W])
{
    enum { MAX_W = LABEL_W };
    static uint8_t alpha[WHITE_H][MAX_W];
    static int16_t blob[WHITE_H][MAX_W];
    static uint16_t stack[WHITE_H * MAX_W][2];
    static uint32_t inside[WHITE_H * MAX_W], total[WHITE_H * MAX_W];
    const int width = w->right - w->left;
    for (int y = 0; y < WHITE_H; ++y)
        for (int x = 0; x < width; ++x) {
            const float u = lean_u(x, y);
            alpha[y][x] = u >= u0 - 3.0f && u < u1 + 3.0f ? word_alpha(w, x, y) : 0u;
            blob[y][x] = -1;
        }
    int blobs = 0;
    for (int y0 = 0; y0 < WHITE_H; ++y0)
        for (int x0 = 0; x0 < width; ++x0) {
            if (alpha[y0][x0] == 0u || blob[y0][x0] >= 0) continue;
            int top = 0;
            inside[blobs] = total[blobs] = 0u;
            blob[y0][x0] = (int16_t)blobs;
            stack[top][0] = (uint16_t)x0; stack[top][1] = (uint16_t)y0; ++top;
            while (top > 0) {
                --top;
                const int x = stack[top][0], y = stack[top][1];
                const float u = lean_u(x, y);
                total[blobs] += alpha[y][x];
                if (u >= u0 && u < u1) inside[blobs] += alpha[y][x];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= WHITE_H) continue;
                        if (alpha[ny][nx] == 0u || blob[ny][nx] >= 0) continue;
                        blob[ny][nx] = (int16_t)blobs;
                        stack[top][0] = (uint16_t)nx; stack[top][1] = (uint16_t)ny; ++top;
                    }
            }
            ++blobs;
        }
    const int shift = (int)lrintf(pen - u0);
    for (int y = 0; y < WHITE_H; ++y)
        for (int x = 0; x < width; ++x) {
            if (alpha[y][x] == 0u) continue;
            const float u = lean_u(x, y);
            const int b = blob[y][x];
            if (u < u0 || u >= u1 || inside[b] * 2u <= total[b]) continue;
            const int px = x + shift;
            const float a = alpha[y][x] / 255.0f;
            if (px >= 0 && px < LABEL_W - 12 && a > white[y][px]) white[y][px] = a;
        }
}

static void write_sprite(uint32_t base, float image[LABEL_H][LABEL_W])
{
    for (uint32_t y = 0u; y < LABEL_H; ++y)
        for (uint32_t x = 0u; x < LABEL_W; ++x) {
            const float a = image[y][x] < 0.0f ? 0.0f : image[y][x] > 1.0f ? 1.0f : image[y][x];
            *recomp_memory(base + swizzle(x, y, LABEL_W, LABEL_H), 1u) = (uint8_t)lrintf(a * 255.0f);
        }
}

/* Draws text into the white sprite; false while the game's label art is not loaded. */
static bool compose(const recomp_utf16 *text, float white[LABEL_H][LABEL_W], int *width)
{
    memset(white, 0, sizeof(float) * LABEL_H * LABEL_W);
    float pen = SLANT * (float)WHITE_H;
    for (const recomp_utf16 *c = text; *c != 0u; ++c) {
        if (*c == u' ') {
            pen += SPACE;
            continue;
        }
        const Letter *letter = NULL;
        for (size_t i = 0u; i < sizeof LETTERS / sizeof LETTERS[0]; ++i)
            if (LETTERS[i].c == *c) letter = &LETTERS[i];
        if (letter == NULL) continue; /* every label's letters are in the table */
        Word word;
        if (!word_for(letter->sprite, &word)) return false;
        const float u0 = letter->from / 10.0f, u1 = letter->to / 10.0f;
        put_letter(&word, u0, u1, pen, white);
        pen += u1 - u0;
    }
    *width = (int)ceilf(pen) + 1;
    if (*width > LABEL_W - 12) *width = LABEL_W - 12;
    return *width > 1;
}

static void blur_glow(float white[LABEL_H][LABEL_W], float glow[LABEL_H][LABEL_W])
{
    enum { RADIUS = 11 };
    static float row_pass[LABEL_H][LABEL_W];
    float kernel[2 * RADIUS + 1], sum = 0.0f;
    for (int i = -RADIUS; i <= RADIUS; ++i) sum += kernel[i + RADIUS] = expf(-(float)(i * i) / (2.0f * SIGMA * SIGMA));
    for (int i = 0; i <= 2 * RADIUS; ++i) kernel[i] /= sum;
    memset(glow, 0, sizeof(float) * LABEL_H * LABEL_W);
    for (int y = 0; y < WHITE_H; ++y)
        for (int x = 0; x < LABEL_W - 12; ++x) glow[y + 7][x + 6] = white[y][x];
    for (int y = 0; y < LABEL_H; ++y)
        for (int x = 0; x < LABEL_W; ++x) {
            float acc = 0.0f;
            for (int i = -RADIUS; i <= RADIUS; ++i)
                if (x + i >= 0 && x + i < LABEL_W) acc += glow[y][x + i] * kernel[i + RADIUS];
            row_pass[y][x] = acc;
        }
    for (int y = 0; y < LABEL_H; ++y)
        for (int x = 0; x < LABEL_W; ++x) {
            float acc = 0.0f;
            for (int i = -RADIUS; i <= RADIUS; ++i)
                if (y + i >= 0 && y + i < LABEL_H) acc += row_pass[y + i][x] * kernel[i + RADIUS];
            glow[y][x] = acc * GAIN;
        }
}

/* (Re)makes the label sprites in the game's language once its font is loaded. */
static void make_label(void)
{
    const int language = (int)game_language();
    if (language == label_language) return;
    static float white[LABEL_H][LABEL_W], glow[LABEL_H][LABEL_W];
    int width = 0;
    if (!compose(language_text((unsigned)language)->label, white, &width)) {
        static bool reported;
        if (!reported) fprintf(stderr, "recomp pc settings: label waits for the game's label art\n");
        reported = true;
        return;
    }
    blur_glow(white, glow);
    write_sprite(WHITE, white);
    write_sprite(GLOW, glow);
    const int16_t rects[2][4] = {{0, 0, (int16_t)width, WHITE_H}, {0, 0, (int16_t)(width + 12), LABEL_H}};
    for (unsigned i = 0u; i < 2u; ++i) {
        memcpy(recomp_memory(DEFS + 12u * i, 8u), rects[i], 8u);
        set_u16(DEFS + 12u * i + 8u, (uint16_t)i);
        set_u16(DEFS + 12u * i + 10u, PC_GROUP);
    }
    recomp_d3d8_invalidate_textures(WHITE, 2u * LABEL_W * LABEL_H);
    label_language = language;
    fprintf(stderr, "recomp pc settings: label made, language %d, %d pixels wide\n", language, width);
}

/* ---- The top list ---------------------------------------------------- */

/* The game's row the PC list's row stands for: rows after PC SETTINGS move down one. */
static unsigned game_row(unsigned row) { return row > PC_ROW ? row - 1u : row; }

static void top_input(void)
{
    const uint32_t count = TOP_ROWS;
    const uint32_t navigate[2] = {CURSORS, count};
    call(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS);
    const uint32_t back[2] = {row, count};
    if ((call(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        call1(sub_00134950, 1u);
        call(sub_00020A70, NULL, 0u);
        return;
    }
    if ((call1(sub_0011BDA0, 1u) & 0xffu) == 0u) return;
    if (row == PC_ROW) {
        set_u16(DEPTH, 1u);
        set_u16(CURSORS + 2u, 0u);
        set_u16(PAGE, PC_PAGE);
        return;
    }
    const uint16_t page = (uint16_t)(game_row(row) + 1u);
    if (page == 6u) {
        call1(sub_00134F50, 0x17u);
        set_u16(DEPTH, 4u);
        set_u16(PAGE, 6u);
    } else if (page == 5u) {
        call1(sub_00134F50, 0x18u);
        set_u16(DEPTH, 4u);
        set_u16(PAGE, 0xfu);
        set_u16(MENU_STATE, 0u);
    } else {
        if (page == 4u) set_u16(CONTROLLER_STATE, (uint16_t)call(sub_0011CF20, NULL, 0u));
        set_u16(DEPTH, 1u);
        set_u16(CURSORS + 2u, 0u);
        set_u16(PAGE, page);
    }
}

static void pc_input(void)
{
    const uint32_t count = PC_ROWS;
    const uint32_t navigate[2] = {CURSORS + 2u, count};
    call(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS + 2u);
    const uint32_t back[2] = {row, count};
    if ((call(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        set_u16(DEPTH, 0u);
        set_u16(PAGE, 0u);
        if (host.closed != NULL) host.closed(host.context);
        return;
    }
    if (row >= PC_ROWS) return;
    const uint32_t value = VALUES + 2u * row;
    const uint16_t before = u16(value);
    const uint32_t step[4] = {value, CHOICES[row], 1u, 0u};
    call(sub_0011B9F0, step, 4u);
    const uint16_t after = u16(value);
    if (after != before && host.changed != NULL) host.changed(row, after, host.context);
}

static void options_update(void)
{
    if (u16(DEPTH) == 0u) {
        top_input();
        finish(1u);
    } else if (u16(PAGE) == PC_PAGE) {
        pc_input();
        finish(1u);
    } else {
        sub_0011CFC0();
    }
}

static void row_count(void (*original)(void))
{
    const uint16_t page = (uint16_t)arg(1u);
    if (page == 0u) finish(TOP_ROWS);
    else if (page == PC_PAGE) finish(PC_ROWS);
    else original();
}
static void row_count_main(void) { row_count(sub_0011AC90); }
static void row_count_alias(void) { row_count(sub_0011AD10); }

static void options_draw(void)
{
    make_label();
    drawing_top = true;
    sub_00095A70();
    drawing_top = false;
}

/* Page 0's draw previews the selected row's page: show the game's row for each. */
static void top_preview(void)
{
    const uint16_t row = u16(CURSORS);
    set_u16(CURSORS, row == PC_ROW ? (uint16_t)(PC_PAGE - 1u) : (uint16_t)game_row(row));
    const uint32_t context = arg(1u);
    const uint32_t eax = call1(sub_00095A10, context);
    set_u16(CURSORS, row);
    finish(eax);
}

/* 0x97830 (sprite, x, y, layer, fade, colour, scale, mode, flags) */
static void sprite(void)
{
    if (!drawing_top) {
        sub_00097830();
        return;
    }
    const unsigned language = game_language() % 6u;
    const uint32_t id = arg(1u), x = arg(2u);
    const uint16_t cursor = u16(CURSORS);
    if (x == 0xf0u && id >= WHITE_BASE[language] && id < WHITE_BASE[language] + 7u) {
        const unsigned row = id - WHITE_BASE[language];
        if (row == 4u && label_language >= 0) {
            uint32_t pc[9];
            for (unsigned i = 0u; i < 9u; ++i) pc[i] = arg(1u + i);
            pc[0] = (uint32_t)PC_GROUP << 16;
            pc[2] += 26u;
            pc[5] = cursor == PC_ROW ? 0xffffffu : 0x808080u;
            static bool reported;
            if (!reported) {
                int16_t r[4];
                memcpy(r, recomp_memory(DEFS, 8u), 8u);
                fprintf(stderr, "recomp pc settings: label drawn at %u,%u, rect %d,%d,%d,%d, key %08x\n",
                    pc[1], pc[2], r[0], r[1], r[2], r[3], *recomp_memory_u32(DEFS + 8u));
                reported = true;
            }
            call(sub_00097830, pc, 9u);
        }
        if (row >= 5u) {
            set_arg(3u, arg(3u) + 26u);
            set_arg(6u, cursor == row + 1u ? 0xffffffu : 0x808080u);
        }
    } else if (x == 0xf6u && id >= GLOW_BASE[language] && id < GLOW_BASE[language] + TOP_ROWS) {
        const unsigned row = id - GLOW_BASE[language];
        if (row == PC_ROW) {
            if (label_language < 0) {
                finish(0u);
                return;
            }
            set_arg(1u, (uint32_t)PC_GROUP << 16 | 1u);
        } else if (row > PC_ROW) {
            set_arg(1u, GLOW_BASE[language] + row - 1u);
        }
    }
    sub_00097830();
}

/* 0x976f0 (x, y, width, height, colours, count, flags): the row bars. */
static void quad(void)
{
    if (drawing_top && arg(1u) == 0u && arg(3u) == 0x21eu && arg(4u) == 0x16u && arg(2u) == 0x5cu + 6u * 0x1au) {
        uint32_t bar[7];
        for (unsigned i = 0u; i < 7u; ++i) bar[i] = arg(1u + i);
        call(sub_000976F0, bar, 7u);
        set_arg(2u, arg(2u) + 0x1au);
    }
    sub_000976F0();
}

/* 0x91560 (page, cursor, fade): the help line. */
static void help(void)
{
    const uint16_t page = (uint16_t)arg(1u), row = (uint16_t)arg(2u);
    uint32_t args[3] = {arg(1u), arg(2u), arg(3u)};
    if (page == 0u) {
        if (row == PC_ROW) {
            args[1] = 0u;
            help_message = PC_MESSAGES + MSG_HELP + HELP_TOP;
        } else if (row > PC_ROW) {
            args[1] = game_row(row);
        }
    } else if (page == PC_PAGE) {
        args[0] = 0u;
        args[1] = 0u;
        help_message = PC_MESSAGES + MSG_HELP + (row < PC_ROWS && live[row] ? HELP_NOW : HELP_NEXT);
    }
    const uint32_t eax = call(sub_00091560, args, 3u);
    help_message = 0u;
    finish(eax);
}

/* 0x6a3e0 (message, language or 0xff) */
static void message(void)
{
    uint32_t number = arg(1u) & 0xffffu;
    if (help_message != 0u && number == 0xc4u) number = help_message;
    const uint32_t requested = arg(2u) & 0xffu;
    if (number >= PC_MESSAGES && number < PC_MESSAGES + MSG_COUNT) {
        const unsigned language = requested == 0xffu ? game_language() : requested;
        finish(text_address[language_text(language) - LANGUAGES][number - PC_MESSAGES]);
        return;
    }
    uint32_t text;
    if (number >= RECOMP_RUNNER_TEXT_FIRST &&
        recomp_runner_text_lookup(number, requested == 0xffu ? game_language() : requested, &text)) {
        finish(text);
        return;
    }
    sub_0006A3E0();
}

/* ---- Page 19 ----------------------------------------------------------- */

static void page_layout(void)
{
    if ((uint16_t)arg(1u) == PC_PAGE) finish(LAYOUT);
    else sub_00090AD0();
}

/* 0x910a0 (page, row) for labels; for values 0x93270 asks 0x91370 (page,
   value row) for a kind and then 0x910a0 (kind, choice) for the text. */
static void page_text(void)
{
    if (drawing_pc) {
        const uint16_t key = (uint16_t)arg(1u);
        const Language *text = language_text(game_language());
        const unsigned l = (unsigned)(text - LANGUAGES);
        if (key == PC_PAGE && arg(2u) < PC_ROWS) {
            finish(text_address[l][MSG_ROWS + arg(2u)]);
            return;
        }
        if (key >= VALUE_KIND && key < VALUE_KIND + PC_ROWS) {
            const uint32_t row = key - VALUE_KIND, choice = arg(2u) & 0xffffu;
            finish(text_address[l][MSG_VALUES + row * MAX_CHOICES + (choice < CHOICES[row] ? choice : 0u)]);
            return;
        }
    }
    sub_000910A0();
}

/* Every PC row has a value, so the value row is the row. */
static void value_text(void)
{
    if ((uint16_t)arg(1u) == PC_PAGE && arg(2u) < PC_ROWS) finish(VALUE_KIND + arg(2u));
    else sub_00091370();
}

static void pc_enter(void)
{
    reset_layout();
    finish(0u);
}

/* As page 8's 0x93df0 and 0x93e90: rows, values, then the cursor. */
static void pc_page_draw(uint32_t visible)
{
    const uint32_t context = arg(1u), fade = *recomp_memory_u32(context + 0x10u);
    const uint32_t cursor = u16(CURSORS + 2u), mask = (1u << PC_ROWS) - 1u;
    static unsigned reported;
    if (reported < 2u) {
        fprintf(stderr, "recomp pc settings: page draws, visible %u, cursor %u\n", visible, cursor);
        ++reported;
    }
    drawing_pc = true;
    const uint32_t rows[4] = {PC_PAGE, cursor, fade, visible};
    call(sub_00091440, rows, 4u);
    const uint32_t values[6] = {PC_PAGE, cursor, fade, mask, VALUES, visible};
    call(sub_00093270, values, 6u);
    /* (context, layout, value mask, cursor, rows, visible): the bars and the
       cursor slide out from the left while visible and back while not. */
    const uint32_t mark[6] = {context, LAYOUT, mask, cursor, PC_ROWS, visible};
    const uint32_t eax = call(sub_00090F00, mark, 6u);
    drawing_pc = false;
    finish(eax);
}
static void pc_draw(void) { pc_page_draw(1u); }
static void pc_leave(void) { pc_page_draw(0u); }

RecompFunction recomp_pc_settings_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case OPTIONS_UPDATE: return options_update;
    case ROW_COUNT: return row_count_main;
    case ROW_COUNT_ALIAS: return row_count_alias;
    case OPTIONS_DRAW: return options_draw;
    case TOP_PREVIEW: return top_preview;
    case SPRITE: return sprite;
    case QUAD: return quad;
    case HELP: return help;
    case MESSAGE: return message;
    case PAGE_LAYOUT: return page_layout;
    case PAGE_TEXT: return page_text;
    case VALUE_TEXT: return value_text;
    case PC_ENTER: return pc_enter;
    case PC_DRAW: return pc_draw;
    case PC_LEAVE: return pc_leave;
    default: return NULL;
    }
}
