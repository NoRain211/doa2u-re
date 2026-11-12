/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings.h"
#include "input_host_win32.h"
#include "kernel_abi.h"
#include "runner_assets.h"

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
   own art (group 3), with the same blue glow, as two runner sprites; its
   text is runner text (runner_assets.c).

   The Controller page (4, frontend_options_controller_update at 0x11c330)
   has Button Config, Vibration, Analog Command and Default; its handler acts
   on row 0, steps rows 1 and 2 and restores the defaults on the last row.
   Rebind becomes row 3, before Default, and opens Button Config to rebind
   (below). Its layout gains a row record, which the page's draw (0x93d10)
   passes the cursor by address.

   Rebind opens the game's Button Config scene (0x19), which row 0 opens, on
   the keyboard's and host pads' bindings instead of the profile's. Its update
   (0x11d480) is replaced while it rebinds: depth 3 (page 0x10) lists the two
   devices where Type A-D and Free Settings were, and depth 4 (page 0x11)
   lists eight plates and Default as Free Settings does. The plates show A, B,
   X, Y, White, Black, L and R; Left/Right turns them to Up, Down, Left,
   Right, Start and Back, labelled in place of the button icons, with no
   lines to the pad. Its draw (0x96720) keeps the game's art; its texts and
   sprites are swapped as they are drawn. */
enum {
    PC_ROW = 5,
    TOP_ROWS = 8,
    PC_PAGE = 19,
    PC_ROWS = RECOMP_PC_SETTING_COUNT,
    VALUE_KIND = 0x7e00,           /* 0x910a0 keys for each row's values, outside its 1-28 */
    CONTROLLER_PAGE = 4,
    REBIND_ROW = 3,
    CONTROLLER_ROWS = 5,
    CONTROLLER_RECORDS = CONTROLLER_ROWS + 2, /* rows, then rows 1 and 2's values */

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
    LAYOUT_TEMPLATE = 0x0067a540u, /* page 8's layout */
    CONTROLLER_LAYOUT = 0x0067adf0u, /* page 4's: four rows, then two values */

    OPTIONS_UPDATE = 0x0011cfc0u,
    ROW_COUNT = 0x0011ac90u,
    ROW_COUNT_ALIAS = 0x0011ad10u,
    OPTIONS_DRAW = 0x00095a70u,
    TOP_PREVIEW = 0x00095a10u,
    SPRITE = 0x00097830u,
    QUAD = 0x000976f0u,
    HELP = 0x00091560u,
    PAGE_LAYOUT = 0x00090ad0u,
    PAGE_TEXT = 0x000910a0u,
    VALUE_TEXT = 0x00091370u,
    PAGE_LABELS = 0x00091440u,
    PAGE_CURSOR = 0x00090f00u,
    CONTROLLER_RESET = 0x00091ca0u, /* page 4's start and enter: zeroes its layout's animation */

    BUTTON_CONFIG_SCENE = 0x19,
    DEVICE_DEPTH = 3, DEVICE_PAGE = 0x10,
    PLATE_DEPTH = 4, PLATE_PAGE = 0x11,
    PLATES = 8, DEFAULT_ROW = 8, PLATE_ROWS = 9, SECOND_SET = 6,
    PLATE_X = 0x252, PLATE_Y = 0x58, PLATE_STEP = 0x22, PLATE_TOP = 0x4d, ICON_X = 500,
    LIST_X = 0xa1, LIST_Y = 0x6b, LIST_STEP = 0x1e,
    NAME_CHARS = 16,
    SPRITE_PLATE = 0x019a0006u, SPRITE_ICONS = 0x019a0009u, SPRITE_ICONS_END = 0x019a0019u,
    SPRITE_ARROW_LEFT = 0x019a0020u, SPRITE_ARROW_RIGHT = 0x019a0021u,
    SPRITE_TYPE_A = 0x019a0000u, SPRITE_FREE = 0x019a0004u, SPRITE_LIST_ARROW = 0x019a0005u,
    CONFIG_UPDATE = 0x0011d480u,
    CONFIG_LEAVE = 0x0011b260u,    /* back to page 4 at depth 1 */
    CONFIG_DRAW = 0x00096720u,
    CONFIG_LIST = 0x000955e0u,     /* the left list */
    PAD_TYPE = 0x000fab80u,        /* (port): 4 makes the list two rows */
    LIST_SPRITE = 0x00097c10u,
    TEXT_DRAW = 0x000978e0u,       /* (text, x, y, layer, fade, brightness, align, scale) */
    CONFIG_SPRITE = 0x00097790u,   /* (sprite, x, y, layer, fade, ...) */
    CONFIG_LINE = 0x000b1700u,     /* a line from the chosen plate to its button */

    /* Page 19's entries: lookup keys, never read as memory. */
    PC_ENTER = 0x03fffff0u,
    PC_DRAW = 0x03fffff4u,
    PC_LEAVE = 0x03fffff8u,
};

/* A page layout is a record per row (x 0x10a), then one per row with a value
   (x 0x1e0), then a zero word; the cursor (0x90f00) walks and animates all of
   them. The labels are P8 sprites of 256x32 (2^8 x 2^5). */
enum {
    LAYOUT_RECORDS = 2u * RECOMP_PC_SETTING_COUNT,
    LABEL_LOG_W = 8,
    LABEL_LOG_H = 5,
    LABEL_W = 1 << LABEL_LOG_W,
    LABEL_H = 1 << LABEL_LOG_H,
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
extern void sub_00090AD0(void);
extern void sub_000910A0(void);
extern void sub_00091370(void);
extern void sub_00091440(void);
extern void sub_00093270(void);
extern void sub_00090F00(void);
extern void sub_0011D480(void);
extern void sub_0011B260(void);
extern void sub_0011AB90(void);
extern void sub_00096720(void);
extern void sub_000955E0(void);
extern void sub_000FAB80(void);
extern void sub_00097C10(void);
extern void sub_000978E0(void);
extern void sub_00097790(void);
extern void sub_000B1700(void);

/* ---- Text ------------------------------------------------------------ */

#define COUNT(list) (sizeof (list) / sizeof (list)[0])
static const uint8_t CHOICES[PC_ROWS] = {COUNT(RECOMP_PC_HEIGHTS), COUNT(RECOMP_PC_SAMPLES), 2u,
    COUNT(RECOMP_PC_SCREENS), 2u, 2u, 2u, 2u, 2u, 2u, 2u};
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
    const recomp_utf16 *rebind, *rebind_help; /* the Controller page's row */
    /* Button Config while it rebinds: the devices, the second plates, the
       help lines (devices, plates, listening) and the listening plate. */
    const recomp_utf16 *devices[2], *labels[SECOND_SET], *rebind_helps[3], *press[2];
} Language;

/* The labels of RECOMP_PC_HEIGHTS, RECOMP_PC_SAMPLES and RECOMP_PC_SCREENS. */
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
      u"Takes effect the next time the game starts."},
     u"Rebind", u"Change the keyboard keys and controller buttons.",
     {u"Keyboard", u"Controller"}, {u"Up", u"Down", u"Left", u"Right", u"Start", u"Back"},
     {u"Choose the keyboard or the controllers.", u"A: change. Left/Right: more buttons.",
      u"Press the new key or button. The other device cancels."},
     {u"Press a key", u"Press a button"}},
    {u"PARAM\u00c8TRES PC",
     {u"R\u00e9solution", u"MSAA", u"SMAA", u"\u00c9cran", u"Rendu", u"VRR", u"Sortie audio",
      u"Vid\u00e9os d'intro", u"D\u00e9blocages", u"Costumes CPU", u"Cam\u00e9ra libre"},
     {{RESOLUTIONS}, {SAMPLES(u"Non")}, {u"Non", u"Oui"}, {SHAPES(u"Ultra-large")}, {RENDERERS},
      {u"Non", u"Oui"}, {u"St\u00e9r\u00e9o", u"5.1 Surround"}, {u"Lire", u"Passer"},
      {u"Tout", u"\u00c0 gagner"}, {u"Normaux", u"Al\u00e9atoires"}, {u"Non", u"Oui"}},
     {u"Voici les param\u00e8tres graphiques, audio et de jeu de ce PC.",
      u"Pris en compte imm\u00e9diatement.", u"Pris en compte au prochain lancement du jeu."},
     u"R\u00e9assigner", u"Modifier les touches du clavier et les boutons de la manette.",
     {u"Clavier", u"Manette"}, {u"Haut", u"Bas", u"Gauche", u"Droite", u"Start", u"Back"},
     {u"Choisissez le clavier ou les manettes.", u"A : modifier. Gauche/Droite : autres boutons.",
      u"Appuyez sur la nouvelle touche ou le nouveau bouton. L'autre appareil annule."},
     {u"Une touche...", u"Un bouton..."}},
    {u"PC-EINSTELLUNGEN",
     {u"Aufl\u00f6sung", u"MSAA", u"SMAA", u"Bildschirm", u"Renderer", u"VRR", u"Audioausgabe",
      u"Intro-Videos", u"Freischaltungen", u"CPU-Kost\u00fcme", u"Freie Kamera"},
     {{RESOLUTIONS}, {SAMPLES(u"Aus")}, {u"Aus", u"Ein"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"Aus", u"Ein"}, {u"Stereo", u"5.1 Surround"}, {u"Abspielen", u"\u00dcberspringen"},
      {u"Alles", u"Erspielen"}, {u"Standard", u"Zuf\u00e4llig"}, {u"Aus", u"Ein"}},
     {u"Das sind die Grafik-, Audio- und Spieleinstellungen f\u00fcr diesen PC.",
      u"Wirkt sofort.", u"Wirkt beim n\u00e4chsten Spielstart."},
     u"Tastenbelegung", u"Tastatur- und Controllerbelegung \u00e4ndern.",
     {u"Tastatur", u"Controller"}, {u"Hoch", u"Runter", u"Links", u"Rechts", u"Start", u"Back"},
     {u"W\u00e4hle die Tastatur oder die Controller.", u"A: \u00e4ndern. Links/Rechts: weitere Tasten.",
      u"Dr\u00fccke die neue Taste. Das andere Ger\u00e4t bricht ab."},
     {u"Taste...", u"Knopf..."}},
    {u"IMPOSTAZIONI PC",
     {u"Risoluzione", u"MSAA", u"SMAA", u"Schermo", u"Renderer", u"VRR", u"Uscita audio",
      u"Filmati iniziali", u"Sblocchi", u"Costumi CPU", u"Telecamera libera"},
     {{RESOLUTIONS}, {SAMPLES(u"No")}, {u"No", u"S\u00ec"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"No", u"S\u00ec"}, {u"Stereo", u"5.1 Surround"}, {u"Riproduci", u"Salta"},
      {u"Tutto", u"Da sbloccare"}, {u"Predefiniti", u"Casuali"}, {u"No", u"S\u00ec"}},
     {u"Impostazioni grafiche, audio e di gioco di questo PC.", u"Effettivo immediatamente.",
      u"Effettivo al prossimo avvio del gioco."},
     u"Riassegna", u"Cambia i tasti della tastiera e i pulsanti del controller.",
     {u"Tastiera", u"Controller"}, {u"Su", u"Gi\u00f9", u"Sinistra", u"Destra", u"Start", u"Back"},
     {u"Scegli la tastiera o i controller.", u"A: cambia. Sinistra/Destra: altri pulsanti.",
      u"Premi il nuovo tasto o pulsante. L'altro dispositivo annulla."},
     {u"Un tasto...", u"Un pulsante..."}},
    {u"CONFIG. PC",
     {u"Resoluci\u00f3n", u"MSAA", u"SMAA", u"Pantalla", u"Renderizador", u"VRR", u"Salida de audio",
      u"V\u00eddeos de inicio", u"Desbloqueos", u"Trajes CPU", u"C\u00e1mara libre"},
     {{RESOLUTIONS}, {SAMPLES(u"No")}, {u"No", u"S\u00ed"}, {SHAPES(u"Ultrawide")}, {RENDERERS},
      {u"No", u"S\u00ed"}, {u"Est\u00e9reo", u"5.1 Surround"}, {u"Reproducir", u"Saltar"},
      {u"Todo", u"Ganar"}, {u"Normales", u"Aleatorios"}, {u"No", u"S\u00ed"}},
     {u"Opciones de gr\u00e1ficos, audio y juego de este PC.", u"Se aplica de inmediato.",
      u"Se aplica la pr\u00f3xima vez que inicies el juego."},
     u"Reasignar", u"Cambia las teclas y los botones del mando.",
     {u"Teclado", u"Mando"}, {u"Arriba", u"Abajo", u"Izquierda", u"Derecha", u"Start", u"Back"},
     {u"Elige el teclado o los mandos.", u"A: cambiar. Izquierda/Derecha: m\u00e1s botones.",
      u"Pulsa la nueva tecla o bot\u00f3n. El otro dispositivo cancela."},
     {u"Una tecla...", u"Un bot\u00f3n..."}},
};

/* The game's language (0x212e0): Japanese, English, French, German, Italian, Spanish. */
static const Language *language_text(unsigned language)
{
    return &LANGUAGES[language >= 2u && language <= 5u ? language - 1u : 0u];
}

/* Message numbers from RECOMP_RUNNER_TEXT_PC: rows, then each row's values,
   then the help lines, then Rebind's label and help. */
enum {
    MSG_ROWS = 0, MSG_VALUES = PC_ROWS, MSG_HELP = MSG_VALUES + PC_ROWS * MAX_CHOICES,
    MSG_REBIND = MSG_HELP + HELPS, MSG_REBIND_HELP, MSG_DEVICES,
    MSG_LABELS = MSG_DEVICES + 2, MSG_REBIND_HELPS = MSG_LABELS + SECOND_SET,
    MSG_PRESS = MSG_REBIND_HELPS + 3, MSG_COUNT = MSG_PRESS + 2
};
enum { HELP_DEVICES, HELP_PLATES, HELP_LISTEN };

static const recomp_utf16 *message_text(const Language *text, unsigned message)
{
    if (message < MSG_VALUES) return text->rows[message - MSG_ROWS];
    if (message < MSG_HELP)
        return text->values[(message - MSG_VALUES) / MAX_CHOICES][(message - MSG_VALUES) % MAX_CHOICES];
    if (message < MSG_REBIND) return text->help[message - MSG_HELP];
    if (message == MSG_REBIND) return text->rebind;
    if (message == MSG_REBIND_HELP) return text->rebind_help;
    if (message < MSG_LABELS) return text->devices[message - MSG_DEVICES];
    if (message < MSG_REBIND_HELPS) return text->labels[message - MSG_LABELS];
    if (message < MSG_PRESS) return text->rebind_helps[message - MSG_REBIND_HELPS];
    return text->press[message - MSG_PRESS];
}

/* The top list's sprites in each language: white rows, then glows. */
static const uint32_t WHITE_BASE[6] = {0x3000du, 0x3000du, 0x3001bu, 0x30029u, 0x30045u, 0x30037u};
static const uint32_t GLOW_BASE[6] = {0x30006u, 0x30006u, 0x30014u, 0x30022u, 0x3003eu, 0x30030u};

/* ---- State -------------------------------------------------------- */

static RecompPcSettingsHost host;
static uint32_t values;          /* guest u16 per row: the choices */
static uint32_t layout;          /* guest page layout: LAYOUT_RECORDS of 28 bytes, then 0 */
static uint32_t controller_layout; /* page 4's with Rebind: CONTROLLER_RECORDS, then 0 */
static bool drawing_top;
static bool drawing_pc;          /* page 19's own draw is running */
static bool drawing_controller;  /* page 4's labels are being drawn */
static bool rebinding;           /* Button Config shows the bindings */
static bool drawing_config;      /* ... and its draw is running */
static bool drawing_list;        /* ... and its left list's */
static uint32_t plate_set;       /* guest u16: 0 the eight buttons, 1 the d-pad, Start and Back */
static uint32_t plate_names;     /* guest UTF-16, NAME_CHARS per plate */
static int label_language = -1; /* the language the label sprites were made in */

/* ---- Guest helpers -------------------------------------------------- */

static void set_arg(unsigned i, uint32_t value) { *recomp_memory_u32(recomp_runtime.registers.esp + 4u * i) = value; }
static uint16_t u16(uint32_t address) { return *recomp_memory_u16(address); }
static void set_u16(uint32_t address, uint16_t v) { *recomp_memory_u16(address) = v; }
static void set_u32(uint32_t address, uint32_t v) { *recomp_memory_u32(address) = v; }
static uint32_t call1(RecompFunction function, uint32_t a) { return kernel_call_lifted(function, &a, 1u); }

static unsigned game_language(void) { return kernel_call_lifted(sub_000212E0, NULL, 0u) & 0xffu; }

/* A message's text in the game's language. */
static uint32_t text_for(unsigned message)
{
    uint32_t text = 0u;
    recomp_runner_text_lookup(RECOMP_RUNNER_TEXT_PC + message, game_language(), &text);
    return text;
}

/* ---- Install -------------------------------------------------------- */

unsigned recomp_pc_settings_choices(unsigned setting) { return setting < PC_ROWS ? CHOICES[setting] : 0u; }
bool recomp_pc_settings_applies_now(unsigned setting) { return setting < PC_ROWS && live[setting]; }
void recomp_pc_settings_set_live(unsigned setting, bool on) { if (setting < PC_ROWS) live[setting] = on; }
uint8_t recomp_pc_settings_choice(unsigned setting)
{
    return setting < PC_ROWS && values != 0u ? (uint8_t)u16(values + 2u * setting) : 0u;
}

static void reset_layout(void)
{
    for (unsigned record = 0u; record < LAYOUT_RECORDS; ++record) set_u32(layout + record * 28u + 0xcu, 0u);
}

void recomp_pc_settings_install(const RecompPcSettingsHost *with, const uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    host = with != NULL ? *with : (RecompPcSettingsHost){0};
    drawing_top = false;
    drawing_pc = false;
    rebinding = drawing_config = drawing_list = false;
    label_language = -1;
    memcpy(live, ALWAYS_LIVE, sizeof live);
    values = recomp_runner_alloc(2u * PC_ROWS, 4u);
    layout = recomp_runner_alloc(28u * LAYOUT_RECORDS + 4u, 4u);
    controller_layout = recomp_runner_alloc(28u * CONTROLLER_RECORDS + 4u, 4u);
    plate_set = recomp_runner_alloc(2u, 4u);
    plate_names = recomp_runner_alloc(2u * NAME_CHARS * PLATES, 4u);
    for (unsigned row = 0u; row < PC_ROWS; ++row)
        set_u16(values + 2u * row, choices[row] < CHOICES[row] ? choices[row] : 0u);

    /* Page 8's row record (0) and value record (4), 26 apart, then the end. */
    for (unsigned record = 0u; record < LAYOUT_RECORDS; ++record) {
        const unsigned row = record % PC_ROWS;
        const bool value = record >= PC_ROWS;
        const uint32_t to = layout + 28u * record, at = (0x5du + 26u * row) << 16 | (value ? 0x1e0u : 0x10au);
        memcpy(recomp_memory(to, 28u), recomp_memory(LAYOUT_TEMPLATE + 28u * (value ? 4u : 0u), 28u), 28u);
        set_u32(to, at);
        set_u32(to + 4u, at);
    }
    set_u32(layout + 28u * LAYOUT_RECORDS, 0u);

    /* Page 4's rows, Default's again 26 lower as row 4, then its two values. */
    for (unsigned record = 0u; record < CONTROLLER_RECORDS; ++record) {
        const unsigned from = record <= REBIND_ROW ? record : record - 1u;
        const uint32_t to = controller_layout + 28u * record;
        memcpy(recomp_memory(to, 28u), recomp_memory(CONTROLLER_LAYOUT + 28u * from, 28u), 28u);
        if (record == CONTROLLER_ROWS - 1u) {
            set_u32(to, *recomp_memory_u32(to) + (26u << 16));
            set_u32(to + 4u, *recomp_memory_u32(to + 4u) + (26u << 16));
        }
    }
    set_u32(controller_layout + 28u * CONTROLLER_RECORDS, 0u);

    for (unsigned m = 0u; m < MSG_COUNT; ++m) {
        if (message_text(&LANGUAGES[0], m) == NULL) continue; /* a row's unused value */
        const recomp_utf16 *text[6];
        for (unsigned l = 0u; l < 6u; ++l) text[l] = message_text(language_text(l), m);
        recomp_runner_text_set(RECOMP_RUNNER_TEXT_PC + m, text);
    }

    /* Two P8 sprites (white label, glow), each palette one colour at every alpha. */
    for (unsigned i = 0u; i < 2u; ++i) {
        uint32_t colours;
        const uint32_t palette = recomp_runner_palette(&colours);
        for (unsigned a = 0u; a < 256u; ++a) set_u32(colours + 4u * a, a << 24 | (i == 0u ? 0xffffffu : 0x3990fau));
        recomp_runner_sprite(RECOMP_RUNNER_SPRITE_PC_WHITE + i, LABEL_LOG_W, LABEL_LOG_H, palette);
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

static uint8_t word_alpha(const RecompGameSprite *w, int x, int y)
{
    return (uint8_t)(recomp_game_sprite_colour(w, recomp_game_sprite_index(w, x, y)) >> 24);
}

/* u of a pixel: x less the row's lean, so a letter's slice is u0 <= u < u1. */
static float lean_u(int x, int y) { return (float)x - SLANT * (float)(WHITE_H - 1 - y) + SLANT * (float)WHITE_H; }

/* Copies one letter at pen: the pixels of its slice, less any blob that lies
   mostly outside the slice (a neighbour's corner). */
static void put_letter(const RecompGameSprite *w, float u0, float u1, float pen, float white[LABEL_H][LABEL_W])
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

/* Alphas as palette indices: index a is alpha a. */
static void write_sprite(unsigned slot, float image[LABEL_H][LABEL_W])
{
    static uint8_t pixels[LABEL_H * LABEL_W];
    for (uint32_t y = 0u; y < LABEL_H; ++y)
        for (uint32_t x = 0u; x < LABEL_W; ++x) {
            const float a = image[y][x] < 0.0f ? 0.0f : image[y][x] > 1.0f ? 1.0f : image[y][x];
            pixels[y * LABEL_W + x] = (uint8_t)lrintf(a * 255.0f);
        }
    recomp_runner_sprite_pixels(slot, pixels);
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
        RecompGameSprite word;
        if (!recomp_game_sprite(letter->sprite, WHITE_H, &word) || word.right - word.left > LABEL_W) return false;
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
    write_sprite(RECOMP_RUNNER_SPRITE_PC_WHITE, white);
    write_sprite(RECOMP_RUNNER_SPRITE_PC_GLOW, glow);
    recomp_runner_sprite_size(RECOMP_RUNNER_SPRITE_PC_WHITE, width, WHITE_H);
    recomp_runner_sprite_size(RECOMP_RUNNER_SPRITE_PC_GLOW, width + 12, LABEL_H);
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
    kernel_call_lifted(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS);
    const uint32_t back[2] = {row, count};
    if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        call1(sub_00134950, 1u);
        kernel_call_lifted(sub_00020A70, NULL, 0u);
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
        if (page == 4u) set_u16(CONTROLLER_STATE, (uint16_t)kernel_call_lifted(sub_0011CF20, NULL, 0u));
        set_u16(DEPTH, 1u);
        set_u16(CURSORS + 2u, 0u);
        set_u16(PAGE, page);
    }
}

static void pc_input(void)
{
    const uint32_t count = PC_ROWS;
    const uint32_t navigate[2] = {CURSORS + 2u, count};
    kernel_call_lifted(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS + 2u);
    const uint32_t back[2] = {row, count};
    if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        set_u16(DEPTH, 0u);
        set_u16(PAGE, 0u);
        if (host.closed != NULL) host.closed(host.context);
        return;
    }
    if (row >= PC_ROWS) return;
    const uint32_t value = values + 2u * row;
    const uint16_t before = u16(value);
    const uint32_t step[4] = {value, CHOICES[row], 1u, 0u};
    kernel_call_lifted(sub_0011B9F0, step, 4u);
    const uint16_t after = u16(value);
    if (after != before && host.changed != NULL) host.changed(row, after, host.context);
}

static void options_update(void)
{
    if (u16(DEPTH) == 0u) {
        top_input();
        kernel_return_caller_cleanup(1u);
    } else if (u16(PAGE) == PC_PAGE) {
        pc_input();
        kernel_return_caller_cleanup(1u);
    } else if (u16(PAGE) == CONTROLLER_PAGE && host.listen != NULL &&
               u16(CURSORS + 2u * u16(DEPTH)) == REBIND_ROW && (call1(sub_0011BDA0, 1u) & 0xffu) != 0u) {
        /* As row 0 opens Button Config, on the device list. */
        call1(sub_00134F50, BUTTON_CONFIG_SCENE);
        set_u16(DEPTH, DEVICE_DEPTH);
        set_u16(PAGE, DEVICE_PAGE);
        set_u16(CURSORS + 2u * DEVICE_DEPTH, 0u);
        rebinding = true;
        kernel_return_caller_cleanup(1u);
    } else {
        sub_0011CFC0();
    }
}

static void row_count(void (*original)(void))
{
    const uint16_t page = (uint16_t)kernel_arg(1u);
    if (page == 0u) kernel_return_caller_cleanup(TOP_ROWS);
    else if (page == PC_PAGE) kernel_return_caller_cleanup(PC_ROWS);
    else if (page == CONTROLLER_PAGE) kernel_return_caller_cleanup(CONTROLLER_ROWS);
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
    const uint32_t context = kernel_arg(1u);
    const uint32_t eax = call1(sub_00095A10, context);
    set_u16(CURSORS, row);
    kernel_return_caller_cleanup(eax);
}

/* 0x97830 (sprite, x, y, layer, fade, colour, scale, mode, flags) */
static void sprite(void)
{
    if (!drawing_top) {
        sub_00097830();
        return;
    }
    const unsigned language = game_language() % 6u;
    const uint32_t id = kernel_arg(1u), x = kernel_arg(2u);
    const uint16_t cursor = u16(CURSORS);
    if (x == 0xf0u && id >= WHITE_BASE[language] && id < WHITE_BASE[language] + 7u) {
        const unsigned row = id - WHITE_BASE[language];
        if (row == 4u && label_language >= 0) {
            uint32_t pc[9];
            for (unsigned i = 0u; i < 9u; ++i) pc[i] = kernel_arg(1u + i);
            pc[0] = recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_PC_WHITE);
            pc[2] += 26u;
            pc[5] = cursor == PC_ROW ? 0xffffffu : 0x808080u;
            kernel_call_lifted(sub_00097830, pc, 9u);
        }
        if (row >= 5u) {
            set_arg(3u, kernel_arg(3u) + 26u);
            set_arg(6u, cursor == row + 1u ? 0xffffffu : 0x808080u);
        }
    } else if (x == 0xf6u && id >= GLOW_BASE[language] && id < GLOW_BASE[language] + TOP_ROWS) {
        const unsigned row = id - GLOW_BASE[language];
        if (row == PC_ROW) {
            if (label_language < 0) {
                kernel_return_caller_cleanup(0u);
                return;
            }
            set_arg(1u, recomp_runner_sprite_id(RECOMP_RUNNER_SPRITE_PC_GLOW));
        } else if (row > PC_ROW) {
            set_arg(1u, GLOW_BASE[language] + row - 1u);
        }
    }
    sub_00097830();
}

/* 0x976f0 (x, y, width, height, colours, count, flags): the row bars. */
static void quad(void)
{
    if (drawing_top && kernel_arg(1u) == 0u && kernel_arg(3u) == 0x21eu && kernel_arg(4u) == 0x16u &&
        kernel_arg(2u) == 0x5cu + 6u * 0x1au) {
        uint32_t bar[7];
        for (unsigned i = 0u; i < 7u; ++i) bar[i] = kernel_arg(1u + i);
        kernel_call_lifted(sub_000976F0, bar, 7u);
        set_arg(2u, kernel_arg(2u) + 0x1au);
    }
    sub_000976F0();
}

/* 0x91560 (page, cursor, fade): the help line. */
static void help(void)
{
    const uint16_t page = (uint16_t)kernel_arg(1u), row = (uint16_t)kernel_arg(2u);
    uint32_t args[3] = {kernel_arg(1u), kernel_arg(2u), kernel_arg(3u)};
    unsigned message = MSG_COUNT; /* the game's own */
    if (page == 0u) {
        if (row == PC_ROW) {
            args[1] = 0u;
            message = MSG_HELP + HELP_TOP;
        } else if (row > PC_ROW) {
            args[1] = game_row(row);
        }
    } else if (page == PC_PAGE) {
        args[0] = 0u;
        args[1] = 0u;
        message = MSG_HELP + (row < PC_ROWS && live[row] ? HELP_NOW : HELP_NEXT);
    } else if (page == CONTROLLER_PAGE) {
        if (row == REBIND_ROW) {
            args[0] = 0u;
            args[1] = 0u;
            message = MSG_REBIND_HELP;
        } else if (row > REBIND_ROW) {
            args[1] = row - 1u;
        }
    } else if (rebinding && (page == DEVICE_PAGE || page == PLATE_PAGE)) {
        args[0] = 0u;
        args[1] = 0u;
        message = MSG_REBIND_HELPS + (page == DEVICE_PAGE ? HELP_DEVICES : host.listening() ? HELP_LISTEN : HELP_PLATES);
    }
    /* The help line draws message 0xc4 for the row passed. */
    if (message < MSG_COUNT) recomp_runner_text_redirect(0xc4u, RECOMP_RUNNER_TEXT_PC + message);
    const uint32_t eax = kernel_call_lifted(sub_00091560, args, 3u);
    recomp_runner_text_redirect(0u, 0u);
    kernel_return_caller_cleanup(eax);
}

/* ---- Page 19 ----------------------------------------------------------- */

static void page_layout(void)
{
    const uint16_t page = (uint16_t)kernel_arg(1u);
    if (page == PC_PAGE) kernel_return_caller_cleanup(layout);
    else if (page == CONTROLLER_PAGE) kernel_return_caller_cleanup(controller_layout);
    else sub_00090AD0();
}

/* 0x91440 (page, cursor, fade, leaving) draws a page's labels through 0x910a0. */
static void page_labels(void)
{
    drawing_controller = (uint16_t)kernel_arg(1u) == CONTROLLER_PAGE;
    sub_00091440();
    drawing_controller = false;
}

/* 0x90f00 (context, layout, value mask, cursor, rows, visible). */
static void page_cursor(void)
{
    if (kernel_arg(2u) == CONTROLLER_LAYOUT) set_arg(2u, controller_layout);
    sub_00090F00();
}

static void controller_reset(void)
{
    for (unsigned record = 0u; record < CONTROLLER_RECORDS; ++record)
        set_u32(controller_layout + record * 28u + 0xcu, 0u);
    kernel_return_caller_cleanup(0u);
}

/* 0x910a0 (page, row) for labels; for values 0x93270 asks 0x91370 (page,
   value row) for a kind and then 0x910a0 (kind, choice) for the text. */
static void page_text(void)
{
    if (drawing_controller && (uint16_t)kernel_arg(1u) == CONTROLLER_PAGE) {
        const uint32_t row = kernel_arg(2u);
        if (row == REBIND_ROW) {
            kernel_return_caller_cleanup(text_for(MSG_REBIND));
            return;
        }
        if (row > REBIND_ROW) set_arg(2u, row - 1u);
    }
    if (drawing_pc) {
        const uint16_t key = (uint16_t)kernel_arg(1u);
        if (key == PC_PAGE && kernel_arg(2u) < PC_ROWS) {
            kernel_return_caller_cleanup(text_for(MSG_ROWS + kernel_arg(2u)));
            return;
        }
        if (key >= VALUE_KIND && key < VALUE_KIND + PC_ROWS) {
            const uint32_t row = key - VALUE_KIND, choice = kernel_arg(2u) & 0xffffu;
            const uint32_t value = choice < CHOICES[row] ? choice : 0u;
            kernel_return_caller_cleanup(text_for(MSG_VALUES + row * MAX_CHOICES + value));
            return;
        }
    }
    sub_000910A0();
}

/* Every PC row has a value, so the value row is the row. */
static void value_text(void)
{
    if ((uint16_t)kernel_arg(1u) == PC_PAGE && kernel_arg(2u) < PC_ROWS)
        kernel_return_caller_cleanup(VALUE_KIND + kernel_arg(2u));
    else sub_00091370();
}

static void pc_enter(void)
{
    reset_layout();
    kernel_return_caller_cleanup(0u);
}

/* As page 8's 0x93df0 and 0x93e90: rows, values, then the cursor. */
static void pc_page_draw(uint32_t visible)
{
    const uint32_t context = kernel_arg(1u), fade = *recomp_memory_u32(context + 0x10u);
    const uint32_t cursor = u16(CURSORS + 2u), mask = (1u << PC_ROWS) - 1u;
    static unsigned reported;
    if (reported < 2u) {
        fprintf(stderr, "recomp pc settings: page draws, visible %u, cursor %u\n", visible, cursor);
        ++reported;
    }
    drawing_pc = true;
    const uint32_t rows[4] = {PC_PAGE, cursor, fade, visible};
    kernel_call_lifted(sub_00091440, rows, 4u);
    const uint32_t value_args[6] = {PC_PAGE, cursor, fade, mask, values, visible};
    kernel_call_lifted(sub_00093270, value_args, 6u);
    /* (context, layout, value mask, cursor, rows, visible): the bars and the
       cursor slide out from the left while visible and back while not. */
    const uint32_t mark[6] = {context, layout, mask, cursor, PC_ROWS, visible};
    const uint32_t eax = kernel_call_lifted(sub_00090F00, mark, 6u);
    drawing_pc = false;
    kernel_return_caller_cleanup(eax);
}
static void pc_draw(void) { pc_page_draw(1u); }
static void pc_leave(void) { pc_page_draw(0u); }

/* ---- Button Config, rebinding -------------------------------------------- */

static const uint8_t PLATE_BINDINGS[2][PLATES] = {
    {RECOMP_INPUT_BIND_A, RECOMP_INPUT_BIND_B, RECOMP_INPUT_BIND_X, RECOMP_INPUT_BIND_Y,
     RECOMP_INPUT_BIND_WHITE, RECOMP_INPUT_BIND_BLACK, RECOMP_INPUT_BIND_LTRIG, RECOMP_INPUT_BIND_RTRIG},
    {RECOMP_INPUT_BIND_UP, RECOMP_INPUT_BIND_DOWN, RECOMP_INPUT_BIND_LEFT, RECOMP_INPUT_BIND_RIGHT,
     RECOMP_INPUT_BIND_START, RECOMP_INPUT_BIND_BACK, RECOMP_INPUT_BIND_COUNT, RECOMP_INPUT_BIND_COUNT},
};

static bool pad_device(void) { return u16(CURSORS + 2u * DEVICE_DEPTH) == 1u; }
/* The device list previews the first plates. */
static unsigned shown_set(void) { return u16(PAGE) == PLATE_PAGE ? u16(plate_set) & 1u : 0u; }
/* RECOMP_INPUT_BIND_COUNT for a plate the second set leaves out. */
static unsigned plate_binding(unsigned row) { return row < PLATES ? PLATE_BINDINGS[shown_set()][row] : RECOMP_INPUT_BIND_COUNT; }
/* The plate row a Button Config sprite or text at y belongs to; PLATES for none. */
static unsigned plate_row(uint32_t y) { return y >= PLATE_TOP && (y - PLATE_TOP) / PLATE_STEP < PLATES ? (y - PLATE_TOP) / PLATE_STEP : PLATES; }

static void draw_text(uint32_t text, uint32_t x, uint32_t y, uint32_t fade, uint32_t brightness, uint32_t align)
{
    const uint32_t args[8] = {text, x, y, 3u, fade, brightness, align, 0x3f59999au};
    kernel_call_lifted(sub_000978E0, args, 8u);
}

/* What presses a plate's button, as text in the plate's own buffer. */
static uint32_t plate_name(unsigned row, unsigned binding)
{
    const uint32_t text = plate_names + 2u * NAME_CHARS * row;
    const char *name = host.bound(binding, pad_device());
    if (name == NULL) name = "-";
    unsigned i = 0u;
    for (; name[i] != '\0' && i + 1u < NAME_CHARS; ++i) set_u16(text + 2u * i, (uint8_t)name[i]);
    set_u16(text + 2u * i, 0u);
    return text;
}

static void config_update(void)
{
    if (!rebinding) {
        sub_0011D480();
        return;
    }
    if (*recomp_memory_u32(kernel_arg(1u) + 0x20u) == 1u) kernel_call_lifted(sub_0011AB90, NULL, 0u);
    const uint32_t back[2] = {0u, 0u};
    if (u16(DEPTH) == DEVICE_DEPTH) {
        const uint32_t navigate[2] = {CURSORS + 2u * DEVICE_DEPTH, 2u};
        kernel_call_lifted(sub_0011B8D0, navigate, 2u);
        if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
            call1(sub_00134950, 1u);
        } else if ((call1(sub_0011BDA0, 1u) & 0xffu) != 0u) {
            set_u16(DEPTH, PLATE_DEPTH);
            set_u16(PAGE, PLATE_PAGE);
            set_u16(CURSORS + 2u * PLATE_DEPTH, 0u);
            set_u16(plate_set, 0u);
        }
    } else if (u16(DEPTH) == PLATE_DEPTH && !host.listening()) {
        const uint32_t cursor = CURSORS + 2u * PLATE_DEPTH;
        const uint32_t navigate[2] = {cursor, PLATE_ROWS};
        const uint32_t moved = kernel_call_lifted(sub_0011B8D0, navigate, 2u);
        const uint32_t step[4] = {plate_set, 2u, 1u, 0u};
        kernel_call_lifted(sub_0011B9F0, step, 4u);
        /* The second set's two missing plates are stepped over. */
        if (u16(cursor) < PLATES && plate_binding(u16(cursor)) == RECOMP_INPUT_BIND_COUNT)
            set_u16(cursor, moved == 0xffffffffu ? SECOND_SET - 1u : DEFAULT_ROW);
        if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
            set_u16(DEPTH, DEVICE_DEPTH);
            set_u16(PAGE, DEVICE_PAGE);
        } else if ((call1(sub_0011BDA0, 1u) & 0xffu) != 0u) {
            const unsigned row = u16(cursor);
            if (row == DEFAULT_ROW) host.reset(pad_device());
            else host.listen(plate_binding(row), pad_device());
        }
    }
    kernel_return_caller_cleanup(1u);
}

static void config_leave(void)
{
    if (rebinding) host.rebind_closed();
    rebinding = false;
    sub_0011B260();
}

static void config_draw(void)
{
    drawing_config = rebinding;
    sub_00096720();
    drawing_config = false;
}

static void config_list(void)
{
    drawing_list = drawing_config;
    sub_000955E0();
    drawing_list = false;
}

static void pad_type(void)
{
    if (drawing_list) kernel_return_caller_cleanup(4u);
    else sub_000FAB80();
}

/* 0x97c10 (sprite, x, y, layer, fade, brightness, ...): the list's labels
   become the devices' names; its arrow goes. */
static void list_sprite(void)
{
    const uint32_t id = kernel_arg(1u);
    if (!drawing_list || (id != SPRITE_TYPE_A && id != SPRITE_FREE && id != SPRITE_LIST_ARROW)) {
        sub_00097C10();
        return;
    }
    if (id != SPRITE_LIST_ARROW) {
        const bool plates = u16(PAGE) == PLATE_PAGE;
        const unsigned row = !plates && id == SPRITE_FREE ? 1u : 0u;
        const unsigned device = plates ? pad_device() : row;
        draw_text(text_for(MSG_DEVICES + device), LIST_X, LIST_Y + LIST_STEP * row, kernel_arg(5u), kernel_arg(6u), 1u);
    }
    kernel_return_caller_cleanup(0u);
}

/* 0x978e0: a plate's text names what presses its button. */
static void config_text(void)
{
    if (drawing_config && kernel_arg(2u) == PLATE_X && plate_row(kernel_arg(3u)) < PLATES) {
        const unsigned row = plate_row(kernel_arg(3u)), binding = plate_binding(row);
        if (binding == RECOMP_INPUT_BIND_COUNT) {
            kernel_return_caller_cleanup(0u);
            return;
        }
        const bool asking = u16(PAGE) == PLATE_PAGE && row == u16(CURSORS + 2u * PLATE_DEPTH) && host.listening();
        set_arg(1u, asking ? text_for(MSG_PRESS + pad_device()) : plate_name(row, binding));
    }
    sub_000978E0();
}

/* 0x97790: the second set hides its missing plates, and its labels take the
   button icons' place; the value arrows go. */
static void config_sprite(void)
{
    if (drawing_config) {
        const uint32_t id = kernel_arg(1u);
        const unsigned row = plate_row(kernel_arg(3u));
        const bool icon = kernel_arg(2u) == ICON_X && id >= SPRITE_ICONS && id < SPRITE_ICONS_END;
        const bool plate = id == SPRITE_PLATE || id == SPRITE_PLATE + 1u || id == SPRITE_PLATE + 2u;
        if (id == SPRITE_ARROW_LEFT || id == SPRITE_ARROW_RIGHT ||
            ((icon || plate) && row < PLATES && plate_binding(row) == RECOMP_INPUT_BIND_COUNT)) {
            kernel_return_caller_cleanup(0u);
            return;
        }
        if (icon && shown_set() == 1u && row < PLATES) {
            draw_text(text_for(MSG_LABELS + row), ICON_X, PLATE_Y + PLATE_STEP * row, kernel_arg(5u), 0x3f800000u, 0u);
            kernel_return_caller_cleanup(0u);
            return;
        }
    }
    sub_00097790();
}

static void config_line(void)
{
    if (drawing_config && shown_set() == 1u) kernel_return_caller_cleanup(0u);
    else sub_000B1700();
}

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
    case PAGE_LAYOUT: return page_layout;
    case PAGE_TEXT: return page_text;
    case VALUE_TEXT: return value_text;
    case PAGE_LABELS: return page_labels;
    case PAGE_CURSOR: return page_cursor;
    case CONTROLLER_RESET: return controller_reset;
    case CONFIG_UPDATE: return config_update;
    case CONFIG_LEAVE: return config_leave;
    case CONFIG_DRAW: return config_draw;
    case CONFIG_LIST: return config_list;
    case PAD_TYPE: return pad_type;
    case LIST_SPRITE: return list_sprite;
    case TEXT_DRAW: return config_text;
    case CONFIG_SPRITE: return config_sprite;
    case CONFIG_LINE: return config_line;
    case PC_ENTER: return pc_enter;
    case PC_DRAW: return pc_draw;
    case PC_LEAVE: return pc_leave;
    default: return NULL;
    }
}

