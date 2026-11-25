/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings.h"
#include "button_config.h"
#include "kernel_abi.h"
#include "runner_assets.h"
#include "jukebox.h"
#include "utf8_text.h"

#include <stdio.h>
#include <string.h>

/* The game's Options menu (frontend_options_menu_update, 0x11cfc0) keeps a
   cursor per depth at 0xdf4ac8, the depth at 0xdf4ad4 and the page at 0xdf4ad8.
   Page 0 is the top list: Game Settings, Audio, Language, Controller, Video,
   Warning and Exit, each a sprite of group 3 (spOption) drawn by 0x95a70 at
   x 0xf0, the selected one also as a glow sprite at x 0xf6. 0x11ac90 gives
   each page's row count. The other pages are drawn from three tables indexed
   by page (0x67af50 resets a page's layout, 0x67aff0 draws it, 0x67afa0 draws
   it leaving) through 0x91440 (labels), 0x93270 (values) and 0x90f00
   (cursor), which read the page's layout records (0x90ad0), labels and value
   text (0x910a0 and 0x91370) and row count.

   PC SETTINGS is page 19, which no table uses. The main menu's PC SETTINGS
   entry (menu_quit.c) enters Options (its mode, 0xa) and asks for it; the
   first update then opens page 19 without the top list, and leaving it
   leaves Options for the main menu as the top list's back does. Its settings
   are grouped in categories: a list of them on the left, where the top list
   would be, and the chosen one's settings on the right. The game's labels
   (0x91440) draw at one x, so the category list's text moves left as it is
   drawn (0x978e0). Its text is runner text (runner_assets.c). The Controller page's Rebind row and the Button
   Config screen it opens are button_config.c.

   The Jukebox (jukebox.c) is page 19 too: the per-page tables hold 20 pages,
   so there is no page 20. The main menu's JUKEBOX entry opens it the same
   way. Its labels, values and help line are written into guest buffers each
   frame from jukebox.c's text. */
enum {
    PC_PAGE = 19,
    PC_ROWS = RECOMP_PC_SETTING_COUNT,
    CATEGORIES = 5,
    SHOWN = 6,                     /* the most settings a category holds */
    CATEGORY_X = 0x24,             /* the category bars' x; the settings' are at 0x10a */
    LABEL_X = 0x1d4,               /* where 0x91440 right-aligns a label, 0xca past its bar */
    NO_CURSOR = 0xffffu,           /* a cursor no row matches: the list is not in use */
    VALUE_KIND = 0x7e00,           /* 0x910a0 keys for each row's values, outside its 1-28 */

    CURSORS = 0x00df4ac8u,
    MENU_STATE = 0x00df4ad0u,
    DEPTH = 0x00df4ad4u,
    PAGE = 0x00df4ad8u,
    PAGE_START = 0x0067af00u,      /* the five per-page tables of 0x95a70 */
    PAGE_ENTER = 0x0067af50u,
    PAGE_LEAVE = 0x0067afa0u,
    PAGE_DRAW = 0x0067aff0u,
    PAGE_IN = 0x0067b040u,
    LAYOUT_TEMPLATE = 0x0067a540u, /* page 8's layout */

    OPTIONS_INIT = 0x0011afd0u,
    OPTIONS_UPDATE = 0x0011cfc0u,
    ROW_COUNT = 0x0011ac90u,
    ROW_COUNT_ALIAS = 0x0011ad10u,
    OPTIONS_DRAW = 0x00095a70u,
    SPRITE = 0x00097830u,
    HELP = 0x00091560u,
    PAGE_LAYOUT = 0x00090ad0u,
    PAGE_TEXT = 0x000910a0u,
    VALUE_TEXT = 0x00091370u,
    PAGE_CURSOR = 0x00090f00u,
    TEXT_DRAW = 0x000978e0u,       /* (text, x, y, layer, fade, brightness, align, scale) */

    /* Page 19's entries: lookup keys, never read as memory. */
    PC_ENTER = 0x03fffff0u,
    PC_DRAW = 0x03fffff4u,
    PC_LEAVE = 0x03fffff8u,

    JB_ROWS = RECOMP_JUKEBOX_ROWS,
    JB_TEXT = 64,                  /* UTF-16 units per label or value buffer */
    JB_HELP = 160,
};

/* A page layout is a record per row (x 0x10a), then one per row with a value
   (x 0x1e0), then a zero word; the cursor (0x90f00) walks and animates all of
   them. */
enum { LAYOUT_RECORDS = 2u * SHOWN };

extern void sub_0011AFD0(void);
extern void sub_0011CFC0(void);
extern void sub_0011AC90(void);
extern void sub_0011AD10(void);
extern void sub_0011B8D0(void);
extern void sub_0011BCA0(void);
extern void sub_0011BDA0(void);
extern void sub_0011B9F0(void);
extern void sub_00134950(void);
extern void sub_00020A70(void);
extern void sub_000212E0(void);
extern void sub_00095A70(void);
extern void sub_00097830(void);
extern void sub_00091560(void);
extern void sub_00090AD0(void);
extern void sub_000910A0(void);
extern void sub_00091370(void);
extern void sub_00091440(void);
extern void sub_00093270(void);
extern void sub_00090F00(void);
extern void sub_000978E0(void);

/* ---- Text ------------------------------------------------------------ */

#define COUNT(list) (sizeof (list) / sizeof (list)[0])
static const uint8_t CHOICES[PC_ROWS] = {COUNT(RECOMP_PC_HEIGHTS), COUNT(RECOMP_PC_SAMPLES), 2u,
    COUNT(RECOMP_PC_SHARPNESS), 2u, COUNT(RECOMP_PC_SCREENS), 3u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 18u, 4u, 4u, 2u, 3u};
static const bool ALWAYS_LIVE[PC_ROWS] = {
    [RECOMP_PC_UNLOCKS] = true, [RECOMP_PC_CPU_COSTUMES] = true, [RECOMP_PC_FREE_CAMERA] = true,
    [RECOMP_PC_DEDICATION] = true, [RECOMP_PC_LIFE_BAR] = true, [RECOMP_PC_FIGHT_HUD] = true,
    [RECOMP_PC_HIT_VOICES] = true, [RECOMP_PC_KO_REPLAY] = true,
};
/* Each category's settings, in the order shown; -1 ends a short one. */
static const int8_t CATEGORY_SETTINGS[CATEGORIES][SHOWN] = {
    {RECOMP_PC_DISPLAY_MODE, RECOMP_PC_RESOLUTION, RECOMP_PC_SCREEN, RECOMP_PC_RENDERER, RECOMP_PC_VRR,
     RECOMP_PC_FRAME_RATE},
    {RECOMP_PC_MSAA, RECOMP_PC_SMAA, RECOMP_PC_SHARPEN, RECOMP_PC_AO, -1, -1},
    {RECOMP_PC_AUDIO, RECOMP_PC_HIT_VOICES, -1, -1, -1, -1},
    {RECOMP_PC_UNLOCKS, RECOMP_PC_CPU_COSTUMES, RECOMP_PC_FREE_CAMERA, RECOMP_PC_KO_REPLAY, RECOMP_PC_INTRO,
     RECOMP_PC_DEDICATION},
    {RECOMP_PC_LIFE_BAR, RECOMP_PC_FIGHT_HUD, -1, -1, -1, -1},
};
static bool live[PC_ROWS], after_online[PC_ROWS];
enum { MAX_CHOICES = 18, HELP_NOW = 0, HELP_NEXT = 1, HELP_LEAVE = 2, HELPS = 3 };

typedef struct Language {
    const recomp_utf16 *rows[PC_ROWS];
    const recomp_utf16 *values[PC_ROWS][MAX_CHOICES];
    const recomp_utf16 *help[HELPS];
    const recomp_utf16 *categories[CATEGORIES];
    const recomp_utf16 *category_help[CATEGORIES];
} Language;

/* The labels of RECOMP_PC_HEIGHTS, RECOMP_PC_SAMPLES, RECOMP_PC_SHARPNESS and
   RECOMP_PC_SCREENS. */
#define RESOLUTIONS u"480p", u"720p", u"1080p", u"1440p", u"4K"
#define SAMPLES(off) off, u"2x", u"4x", u"8x", u"16x", u"32x"
#define SHARPEN(off, low, medium, high) off, low, medium, high
#define SHAPES(wide) u"4:3", u"16:9", u"16:10", wide, u"32:9"
#define RENDERERS u"Direct3D 11", u"Vulkan", u"Direct3D 12"

/* English, French, German, Italian and Spanish; Japanese shows English. */
static const Language LANGUAGES[5] = {
    {{u"Resolution", u"MSAA", u"SMAA", u"Sharpening", u"Ambient Occlusion", u"Screen", u"Renderer", u"VRR", u"Frame rate", u"Audio Output",
      u"Intro Movies", u"Unlocks", u"CPU Costumes", u"Free Camera", u"Dedication",
      u"Life Bar", u"Fight HUD", u"Hit Voices", u"KO Replay", u"Display Mode"},
     {{RESOLUTIONS}, {SAMPLES(u"Off")}, {u"Off", u"On"}, {SHARPEN(u"Off", u"Low", u"Medium", u"High")}, {u"Off", u"On"},
      {SHAPES(u"21:9")}, {RENDERERS},
      {u"Off", u"On"}, {u"Original", u"Display"}, {u"Stereo", u"5.1 Surround"}, {u"Play", u"Skip"}, {u"Everything", u"Earn"},
      {u"Default", u"Random"}, {u"Off", u"On"}, {u"Show", u"Hide"},
      {u"Default", u"Red", u"Orange", u"Gold", u"Yellow", u"Lime", u"Green", u"Teal", u"Cyan",
       u"Sky Blue", u"Blue", u"Indigo", u"Purple", u"Magenta", u"Pink", u"White", u"Silver", u"Custom"}, {u"Original", u"Damage", u"Stun Meter", u"Both"}, {u"Off", u"Quiet", u"Normal", u"Loud"}, {u"Original", u"Dynamic"}, {u"Windowed", u"Borderless", u"Fullscreen"}},
     {u"Takes effect immediately.",
      u"Takes effect the next time the game starts.",
      u"Takes effect when you leave DOA ONLINE."},
     {u"Display", u"Graphics", u"Audio", u"Gameplay", u"HUD"},
     {u"Display mode, resolution, screen shape, renderer and frame rate.",
      u"Anti-aliasing, sharpening and ambient occlusion.",
      u"Audio output and hit voices.",
      u"Unlocks, CPU costumes, free camera, KO replay, intro movies and dedication.",
      u"Life bar colour and fight HUD."}},
    {{u"R\u00e9solution", u"MSAA", u"SMAA", u"Nettet\u00e9", u"Occlusion ambiante", u"\u00c9cran", u"Rendu", u"VRR", u"Images/s", u"Sortie audio",
      u"Vid\u00e9os d'intro", u"D\u00e9blocages", u"Costumes CPU", u"Cam\u00e9ra libre", u"D\u00e9dicace",
      u"Barre de vie", u"Affichage combat", u"Voix des coups", u"Replay K.O.", u"Mode d'affichage"},
     {{RESOLUTIONS}, {SAMPLES(u"Non")}, {u"Non", u"Oui"}, {SHARPEN(u"Non", u"Faible", u"Moyenne", u"\u00c9lev\u00e9e")}, {u"Non", u"Oui"},
      {SHAPES(u"21:9")}, {RENDERERS},
      {u"Non", u"Oui"}, {u"Original", u"\u00c9cran"}, {u"St\u00e9r\u00e9o", u"5.1 Surround"}, {u"Lire", u"Passer"},
      {u"Tout", u"\u00c0 gagner"}, {u"Normaux", u"Al\u00e9atoires"}, {u"Non", u"Oui"}, {u"Afficher", u"Masquer"},
      {u"Normale", u"Rouge", u"Orange", u"Or", u"Jaune", u"Citron vert", u"Verte", u"Sarcelle", u"Cyan",
       u"Bleu ciel", u"Bleue", u"Indigo", u"Violette", u"Magenta", u"Rose", u"Blanche", u"Argent", u"Perso."}, {u"Original", u"D\u00e9g\u00e2ts", u"\u00c9tourdissement", u"Les deux"}, {u"Non", u"Faible", u"Normal", u"Fort"}, {u"Original", u"Dynamique"}, {u"Fen\u00eatr\u00e9", u"Sans bordure", u"Plein \u00e9cran"}},
     {u"Pris en compte imm\u00e9diatement.", u"Pris en compte au prochain lancement du jeu.",
      u"Pris en compte en quittant DOA ONLINE."},
     {u"Affichage", u"Graphismes", u"Audio", u"Jeu", u"Interface"},
     {u"Mode d'affichage, r\u00e9solution, format, rendu et images/s.",
      u"Anticr\u00e9nelage, nettet\u00e9 et occlusion ambiante.",
      u"Sortie audio et voix des coups.",
      u"D\u00e9blocages, costumes CPU, cam\u00e9ra libre, replay K.O., intro et d\u00e9dicace.",
      u"Couleur de la barre de vie et affichage du combat."}},
    {{u"Aufl\u00f6sung", u"MSAA", u"SMAA", u"Sch\u00e4rfe", u"Umgebungsverdeckung", u"Bildschirm", u"Renderer", u"VRR", u"Bildrate", u"Audioausgabe",
      u"Intro-Videos", u"Freischaltungen", u"CPU-Kost\u00fcme", u"Freie Kamera", u"Widmung",
      u"Lebensleiste", u"Kampfanzeige", u"Treffer-Stimmen", u"K.O.-Wiederholung", u"Anzeigemodus"},
     {{RESOLUTIONS}, {SAMPLES(u"Aus")}, {u"Aus", u"Ein"}, {SHARPEN(u"Aus", u"Niedrig", u"Mittel", u"Hoch")}, {u"Aus", u"Ein"},
      {SHAPES(u"21:9")}, {RENDERERS},
      {u"Aus", u"Ein"}, {u"Original", u"Bildschirm"}, {u"Stereo", u"5.1 Surround"}, {u"Abspielen", u"\u00dcberspringen"},
      {u"Alles", u"Erspielen"}, {u"Standard", u"Zuf\u00e4llig"}, {u"Aus", u"Ein"}, {u"Anzeigen", u"Ausblenden"},
      {u"Standard", u"Rot", u"Orange", u"Gold", u"Gelb", u"Hellgr\u00fcn", u"Gr\u00fcn", u"Petrol", u"Cyan",
       u"Himmelblau", u"Blau", u"Indigo", u"Lila", u"Magenta", u"Rosa", u"Wei\u00df", u"Silber", u"Eigene"}, {u"Original", u"Schaden", u"Bet\u00e4ubung", u"Beides"}, {u"Aus", u"Leise", u"Normal", u"Laut"}, {u"Original", u"Dynamisch"}, {u"Fenster", u"Rahmenlos", u"Vollbild"}},
     {u"Wirkt sofort.", u"Wirkt beim n\u00e4chsten Spielstart.",
      u"Wirkt beim Verlassen von DOA ONLINE."},
     {u"Anzeige", u"Grafik", u"Audio", u"Spiel", u"HUD"},
     {u"Anzeigemodus, Aufl\u00f6sung, Bildformat, Renderer und Bildrate.",
      u"Kantengl\u00e4ttung, Sch\u00e4rfe und Umgebungsverdeckung.",
      u"Audioausgabe und Treffer-Stimmen.",
      u"Freischaltungen, CPU-Kost\u00fcme, freie Kamera, K.O.-Wiederholung, Intro und Widmung.",
      u"Farbe der Lebensleiste und Kampfanzeige."}},
    {{u"Risoluzione", u"MSAA", u"SMAA", u"Nitidezza", u"Occlusione ambientale", u"Schermo", u"Renderer", u"VRR", u"Frame rate", u"Uscita audio",
      u"Filmati iniziali", u"Sblocchi", u"Costumi CPU", u"Telecamera libera", u"Dedica",
      u"Barra vita", u"HUD combattimento", u"Voci dei colpi", u"Replay K.O.", u"Modalit\u00e0 schermo"},
     {{RESOLUTIONS}, {SAMPLES(u"No")}, {u"No", u"S\u00ec"}, {SHARPEN(u"No", u"Bassa", u"Media", u"Alta")}, {u"No", u"S\u00ec"},
      {SHAPES(u"21:9")}, {RENDERERS},
      {u"No", u"S\u00ec"}, {u"Originale", u"Schermo"}, {u"Stereo", u"5.1 Surround"}, {u"Riproduci", u"Salta"},
      {u"Tutto", u"Da sbloccare"}, {u"Predefiniti", u"Casuali"}, {u"No", u"S\u00ec"}, {u"Mostra", u"Nascondi"},
      {u"Predefinita", u"Rossa", u"Arancione", u"Oro", u"Gialla", u"Lime", u"Verde", u"Verde acqua", u"Ciano",
       u"Azzurra", u"Blu", u"Indaco", u"Viola", u"Magenta", u"Rosa", u"Bianca", u"Argento", u"Personale"}, {u"Originale", u"Danni", u"Stordimento", u"Entrambi"}, {u"No", u"Basso", u"Normale", u"Alto"}, {u"Originale", u"Dinamico"}, {u"Finestra", u"Senza bordi", u"Schermo intero"}},
     {u"Effettivo immediatamente.",
      u"Effettivo al prossimo avvio del gioco.",
      u"Effettivo all'uscita da DOA ONLINE."},
     {u"Schermo", u"Grafica", u"Audio", u"Gioco", u"HUD"},
     {u"Modalit\u00e0 schermo, risoluzione, formato, renderer e frame rate.",
      u"Antialiasing, nitidezza e occlusione ambientale.",
      u"Uscita audio e voci dei colpi.",
      u"Sblocchi, costumi CPU, telecamera libera, replay K.O., filmati iniziali e dedica.",
      u"Colore della barra vita e HUD combattimento."}},
    {{u"Resoluci\u00f3n", u"MSAA", u"SMAA", u"Nitidez", u"Oclusi\u00f3n ambiental", u"Pantalla", u"Renderizador", u"VRR", u"Fotogramas", u"Salida de audio",
      u"V\u00eddeos de inicio", u"Desbloqueos", u"Trajes CPU", u"C\u00e1mara libre", u"Dedicatoria",
      u"Barra de vida", u"HUD de combate", u"Voces de golpes", u"Repetici\u00f3n K.O.", u"Modo de pantalla"},
     {{RESOLUTIONS}, {SAMPLES(u"No")}, {u"No", u"S\u00ed"}, {SHARPEN(u"No", u"Baja", u"Media", u"Alta")}, {u"No", u"S\u00ed"},
      {SHAPES(u"21:9")}, {RENDERERS},
      {u"No", u"S\u00ed"}, {u"Original", u"Pantalla"}, {u"Est\u00e9reo", u"5.1 Surround"}, {u"Reproducir", u"Saltar"},
      {u"Todo", u"Ganar"}, {u"Normales", u"Aleatorios"}, {u"No", u"S\u00ed"}, {u"Mostrar", u"Ocultar"},
      {u"Normal", u"Roja", u"Naranja", u"Oro", u"Amarilla", u"Lima", u"Verde", u"Verde azulado", u"Cian",
       u"Celeste", u"Azul", u"\u00cdndigo", u"Morada", u"Magenta", u"Rosa", u"Blanca", u"Plata", u"Propia"}, {u"Original", u"Da\u00f1o", u"Aturdimiento", u"Ambos"}, {u"No", u"Bajo", u"Normal", u"Alto"}, {u"Original", u"Din\u00e1mica"}, {u"Ventana", u"Sin bordes", u"Pantalla completa"}},
     {u"Se aplica de inmediato.",
      u"Se aplica la pr\u00f3xima vez que inicies el juego.",
      u"Se aplica al salir de DOA ONLINE."},
     {u"Pantalla", u"Gr\u00e1ficos", u"Audio", u"Juego", u"HUD"},
     {u"Modo de pantalla, resoluci\u00f3n, formato, renderizador y fotogramas.",
      u"Antialiasing, nitidez y oclusi\u00f3n ambiental.",
      u"Salida de audio y voces de golpes.",
      u"Desbloqueos, trajes CPU, c\u00e1mara libre, repetici\u00f3n K.O., v\u00eddeos de inicio y dedicatoria.",
      u"Color de la barra de vida y HUD de combate."}},
};

/* The game's language (0x212e0): Japanese, English, French, German, Italian, Spanish. */
static const Language *language_text(unsigned language)
{
    return &LANGUAGES[language >= 2u && language <= 5u ? language - 1u : 0u];
}

/* Message numbers from RECOMP_RUNNER_TEXT_PC: rows, then each row's values
   one after another, then the help lines, the categories and their help. */
enum {
    VALUE_COUNT = 5 + 6 + 2 + 4 + 2 + 5 + 3 + 2 + 2 + 2 + 2 + 2 + 2 + 2 + 2 + 18 + 4 + 4 + 2 + 3, /* CHOICES' sum */
    MSG_ROWS = 0, MSG_VALUES = PC_ROWS, MSG_HELP = MSG_VALUES + VALUE_COUNT,
    MSG_CATEGORIES = MSG_HELP + HELPS, MSG_CATEGORY_HELP = MSG_CATEGORIES + CATEGORIES,
    MSG_COUNT = MSG_CATEGORY_HELP + CATEGORIES
};
_Static_assert(RECOMP_RUNNER_TEXT_PC + MSG_COUNT <= RECOMP_RUNNER_TEXT_BUTTON_CONFIG, "PC text stays below the next module's");

/* A setting's first value message. */
static unsigned value_message(unsigned setting)
{
    unsigned message = MSG_VALUES;
    for (unsigned i = 0u; i < setting; ++i) message += CHOICES[i];
    return message;
}

static const recomp_utf16 *message_text(const Language *text, unsigned message)
{
    if (message < MSG_VALUES) return text->rows[message - MSG_ROWS];
    if (message < MSG_HELP) {
        unsigned setting = 0u;
        while (setting + 1u < PC_ROWS && message >= value_message(setting + 1u)) ++setting;
        return text->values[setting][message - value_message(setting)];
    }
    if (message < MSG_CATEGORIES) return text->help[message - MSG_HELP];
    if (message < MSG_CATEGORY_HELP) return text->categories[message - MSG_CATEGORIES];
    return text->category_help[message - MSG_CATEGORY_HELP];
}

/* ---- State -------------------------------------------------------- */

static RecompPcSettingsHost host;
static uint32_t values;          /* guest u16 per setting: the choices */
static uint32_t shown;           /* guest u16 per shown row: its setting's choice */
static unsigned category;        /* the category whose settings page 19 shows */
static bool on_categories;       /* the cursor is in the category list, not the settings */
static bool drawing_categories;  /* 0x91440 is drawing the category list */
static uint32_t layout;          /* guest page layout: up to LAYOUT_RECORDS of 28 bytes, then 0 */
static uint32_t category_layout; /* the category list's row records, then 0 */
static uint32_t probe;           /* a guest u16 that tells left from right */
static bool drawing_top;
static bool drawing_pc;          /* page 19's own draw is running */
static bool pc_requested;        /* the main menu's PC SETTINGS asked for page 19 */
static bool jukebox_mode;        /* page 19 shows the Jukebox, not PC Settings */
static bool from_menu;           /* page 19 opened by the main menu: back leaves Options */
static uint32_t jb_values;       /* guest u16 per Jukebox row: its choice */
static uint32_t jb_layout;       /* guest layout of the Jukebox's rows */
static uint32_t jb_labels, jb_texts, jb_help; /* guest UTF-16 buffers */

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
void recomp_pc_settings_set_after_online(unsigned setting, bool on) { if (setting < PC_ROWS) after_online[setting] = on; }
uint8_t recomp_pc_settings_choice(unsigned setting)
{
    return setting < PC_ROWS && values != 0u ? (uint8_t)u16(values + 2u * setting) : 0u;
}

/* The settings a category holds. */
static unsigned category_rows(unsigned which)
{
    unsigned rows = 0u;
    while (rows < SHOWN && CATEGORY_SETTINGS[which][rows] >= 0) ++rows;
    return rows;
}

/* The rows 0x91440 and the others ask about: the list being drawn. */
static unsigned page_rows(void)
{
    return jukebox_mode ? JB_ROWS : drawing_categories ? CATEGORIES : category_rows(category);
}

/* The setting a shown row stands for, or -1. */
static int setting_at(unsigned row)
{
    return row < category_rows(category) ? CATEGORY_SETTINGS[category][row] : -1;
}

static void refresh_shown(void)
{
    for (unsigned row = 0u; row < category_rows(category); ++row)
        set_u16(shown + 2u * row, u16(values + 2u * (unsigned)setting_at(row)));
}

/* The category's records, 26 apart, then the end word; also restarts the
   category list's slide. */
static void reset_layout(void)
{
    const unsigned rows = category_rows(category);
    for (unsigned record = 0u; record < 2u * rows; ++record) {
        const unsigned row = record % rows;
        const bool value = record >= rows;
        const uint32_t to = layout + 28u * record, at = (0x5du + 26u * row) << 16 | (value ? 0x1e0u : 0x10au);
        memcpy(recomp_memory(to, 28u), recomp_memory(LAYOUT_TEMPLATE + 28u * (value ? 4u : 0u), 28u), 28u);
        set_u32(to, at);
        set_u32(to + 4u, at);
        set_u32(to + 0xcu, 0u);
    }
    set_u32(layout + 28u * rows * 2u, 0u);
}

static void reset_category_layout(void)
{
    for (unsigned row = 0u; row < CATEGORIES; ++row) {
        const uint32_t to = category_layout + 28u * row, at = (0x5du + 26u * row) << 16 | CATEGORY_X;
        memcpy(recomp_memory(to, 28u), recomp_memory(LAYOUT_TEMPLATE, 28u), 28u);
        set_u32(to, at);
        set_u32(to + 4u, at);
        set_u32(to + 0xcu, 0u);
    }
    set_u32(category_layout + 28u * CATEGORIES, 0u);
}

static void reset_jb_layout(void)
{
    for (unsigned record = 0u; record < 2u * JB_ROWS; ++record) set_u32(jb_layout + record * 28u + 0xcu, 0u);
}

/* UTF-8 into a guest UTF-16 buffer of size units; returns it. */
static uint32_t put_text(uint32_t buffer, unsigned size, const char *text)
{
    unsigned n = 0u;
    while (*text != '\0' && n + 1u < size) {
        uint32_t value = recomp_utf8_next(&text);
        if (value > 0xffffu) {
            if (n + 2u >= size) break;
            value -= 0x10000u;
            set_u16(buffer + 2u * n++, (uint16_t)(0xd800u + (value >> 10)));
            value = 0xdc00u + (value & 0x3ffu);
        }
        set_u16(buffer + 2u * n++, (uint16_t)value);
    }
    set_u16(buffer + 2u * n, 0u);
    return buffer;
}

static void refresh_jukebox(void)
{
    for (unsigned row = 0u; row < JB_ROWS; ++row) set_u16(jb_values + 2u * row, (uint16_t)recomp_jukebox_choice(row));
}

void recomp_pc_settings_request_page(void) { pc_requested = true; }

void recomp_pc_settings_install(const RecompPcSettingsHost *with, const uint8_t choices[RECOMP_PC_SETTING_COUNT])
{
    host = with != NULL ? *with : (RecompPcSettingsHost){0};
    drawing_top = false;
    drawing_pc = false;
    pc_requested = false;
    jukebox_mode = from_menu = false;
    memcpy(live, ALWAYS_LIVE, sizeof live);
    category = 0u;
    on_categories = true;
    drawing_categories = false;
    values = recomp_runner_alloc(2u * PC_ROWS, 4u);
    shown = recomp_runner_alloc(2u * SHOWN, 4u);
    layout = recomp_runner_alloc(28u * LAYOUT_RECORDS + 4u, 4u);
    category_layout = recomp_runner_alloc(28u * CATEGORIES + 4u, 4u);
    probe = recomp_runner_alloc(4u, 4u);
    for (unsigned row = 0u; row < PC_ROWS; ++row)
        set_u16(values + 2u * row, choices[row] < CHOICES[row] ? choices[row] : 0u);

    /* Page 8's row record (0) and value record (4), 26 apart, then the end. */
    reset_layout();
    reset_category_layout();

    for (unsigned m = 0u; m < MSG_COUNT; ++m) {
        const recomp_utf16 *text[6];
        for (unsigned l = 0u; l < 6u; ++l) text[l] = message_text(language_text(l), m);
        recomp_runner_text_set(RECOMP_RUNNER_TEXT_PC + m, text);
    }

    /* As page 8: start and enter reset the layout, draw shows the page, and
       in and leave draw it sliding. 0x95a70 waits on a null entry forever. */
    /* The Jukebox's buffers and layout, as PC Settings' for its rows. */
    jb_values = recomp_runner_alloc(2u * JB_ROWS, 4u);
    jb_layout = recomp_runner_alloc(28u * 2u * JB_ROWS + 4u, 4u);
    jb_labels = recomp_runner_alloc(2u * JB_TEXT * JB_ROWS, 4u);
    jb_texts = recomp_runner_alloc(2u * JB_TEXT * JB_ROWS, 4u);
    jb_help = recomp_runner_alloc(2u * JB_HELP, 4u);
    for (unsigned record = 0u; record < 2u * JB_ROWS; ++record) {
        const unsigned row = record % JB_ROWS;
        const bool value = record >= JB_ROWS;
        const uint32_t to = jb_layout + 28u * record, at = (0x5du + 26u * row) << 16 | (value ? 0x1e0u : 0x10au);
        memcpy(recomp_memory(to, 28u), recomp_memory(LAYOUT_TEMPLATE + 28u * (value ? 4u : 0u), 28u), 28u);
        set_u32(to, at);
        set_u32(to + 4u, at);
    }
    set_u32(jb_layout + 28u * 2u * JB_ROWS, 0u);
    recomp_runner_text_set_address(RECOMP_RUNNER_TEXT_JUKEBOX_HELP, jb_help);

    set_u32(PAGE_START + 4u * PC_PAGE, PC_ENTER);
    set_u32(PAGE_ENTER + 4u * PC_PAGE, PC_ENTER);
    set_u32(PAGE_DRAW + 4u * PC_PAGE, PC_DRAW);
    set_u32(PAGE_LEAVE + 4u * PC_PAGE, PC_LEAVE);
    set_u32(PAGE_IN + 4u * PC_PAGE, PC_LEAVE);
}

/* ---- Input ---------------------------------------------------------- */

/* As the top list's back: out of Options to the main menu. Page 19 stays in
   charge (nothing drawn, no input) while Options fades out; entering Options
   again clears it. */
static void leave_options(void)
{
    call1(sub_00134950, 1u);
    kernel_call_lifted(sub_00020A70, NULL, 0u);
}

static void open_page(void)
{
    from_menu = true;
    set_u16(DEPTH, 1u);
    set_u16(CURSORS + 2u, 0u);
    set_u16(PAGE, PC_PAGE);
}

/* The category list: up and down choose, and its settings show beside it;
   confirm or right goes into them. */
static void category_input(void)
{
    const uint32_t navigate[2] = {CURSORS + 2u, CATEGORIES};
    kernel_call_lifted(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS + 2u);
    const uint32_t back[2] = {row, CATEGORIES};
    if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        set_u16(DEPTH, 0u);
        set_u16(PAGE, 0u);
        if (host.closed != NULL) host.closed(host.context);
        leave_options();
        return;
    }
    if (row >= CATEGORIES) return;
    if (row != category) {
        category = row;
        reset_layout();
    }
    set_u16(probe, 1u);
    const uint32_t step[4] = {probe, 3u, 1u, 0u};
    kernel_call_lifted(sub_0011B9F0, step, 4u);
    if (u16(probe) == 2u || (call1(sub_0011BDA0, 1u) & 0xffu) != 0u) {
        on_categories = false;
        set_u16(CURSORS + 2u, 0u);
    }
}

/* The settings: left and right step the selected one; back returns to the categories. */
static void pc_input(void)
{
    if (on_categories) {
        category_input();
        return;
    }
    const uint32_t count = category_rows(category);
    const uint32_t navigate[2] = {CURSORS + 2u, count};
    kernel_call_lifted(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS + 2u);
    const uint32_t back[2] = {row, count};
    if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        on_categories = true;
        set_u16(CURSORS + 2u, (uint16_t)category);
        return;
    }
    const int setting = setting_at(row);
    if (setting < 0) return;
    const uint32_t value = values + 2u * (unsigned)setting;
    const uint16_t before = u16(value);
    const uint32_t step[4] = {value, CHOICES[setting], 1u, 0u};
    kernel_call_lifted(sub_0011B9F0, step, 4u);
    const uint16_t after = u16(value);
    if (after != before && host.changed != NULL) host.changed((unsigned)setting, after, host.context);
}

/* The Jukebox's rows: up and down, left and right step a row's choice, A acts. */
static void jukebox_input(void)
{
    const uint32_t navigate[2] = {CURSORS + 2u, JB_ROWS};
    kernel_call_lifted(sub_0011B8D0, navigate, 2u);
    const uint16_t row = u16(CURSORS + 2u);
    const uint32_t back[2] = {row, JB_ROWS};
    if ((kernel_call_lifted(sub_0011BCA0, back, 2u) & 0xffu) != 0u) {
        recomp_jukebox_page_closed();
        set_u16(DEPTH, 0u);
        set_u16(PAGE, 0u);
        leave_options();
        return;
    }
    if (row >= JB_ROWS) return;
    refresh_jukebox();
    const uint32_t value = jb_values + 2u * row;
    const uint16_t before = u16(value);
    const uint32_t step[4] = {value, recomp_jukebox_choices(row), 1u, 0u};
    kernel_call_lifted(sub_0011B9F0, step, 4u);
    const uint16_t after = u16(value);
    if (after != before) recomp_jukebox_choose(row, after);
    if ((call1(sub_0011BDA0, 1u) & 0xffu) != 0u) recomp_jukebox_confirm(row);
    refresh_jukebox();
}

static void options_update(void)
{
    if (recomp_jukebox_take_request()) {
        /* The main menu's JUKEBOX: straight into its page. */
        jukebox_mode = true;
        recomp_jukebox_page_opened();
        reset_jb_layout();
        refresh_jukebox();
        open_page();
    } else if (pc_requested) {
        /* The main menu's PC SETTINGS: straight into its category list. */
        pc_requested = jukebox_mode = false;
        category = 0u;
        on_categories = true;
        reset_layout();
        reset_category_layout();
        open_page();
    }
    if (from_menu && u16(DEPTH) == 0u) {
        /* Leaving for the main menu: the Options list never shows. */
        kernel_return_caller_cleanup(1u);
        return;
    }
    if (u16(DEPTH) != 0u && u16(PAGE) == PC_PAGE) {
        if (jukebox_mode) jukebox_input();
        else pc_input();
        kernel_return_caller_cleanup(1u);
    } else if (recomp_button_config_input()) {
        kernel_return_caller_cleanup(1u);
    } else {
        sub_0011CFC0();
    }
}

static void row_count(void (*original)(void))
{
    const uint16_t page = (uint16_t)kernel_arg(1u);
    uint32_t rows;
    if (page == PC_PAGE) kernel_return_caller_cleanup(page_rows());
    else if (recomp_button_config_rows(page, &rows)) kernel_return_caller_cleanup(rows);
    else original();
}
static void row_count_main(void) { row_count(sub_0011AC90); }
static void row_count_alias(void) { row_count(sub_0011AD10); }

/* Options starts: whatever page 19 left behind is over. */
static void options_init(void)
{
    if (jukebox_mode) recomp_jukebox_page_closed(); /* releases its mute */
    jukebox_mode = from_menu = false;
    sub_0011AFD0();
}

static void options_draw(void)
{
    if (from_menu && u16(DEPTH) == 0u) {
        kernel_return_caller_cleanup(0u);
        return;
    }
    drawing_top = true;
    sub_00095A70();
    drawing_top = false;
}

/* 0x97830 (sprite, x, y, layer, fade, colour, scale, mode, flags) */
static void sprite(void)
{
    /* Page 19 stands alone: no top list beside it. */
    if (drawing_top && from_menu && (kernel_arg(2u) == 0xf0u || kernel_arg(2u) == 0xf6u)) {
        kernel_return_caller_cleanup(0u);
        return;
    }
    sub_00097830();
}

/* 0x978e0: the category list's labels draw left of the settings'. Button
   Config's hook (button_config.c) still sees every text. */
static void text_draw(void)
{
    if (drawing_categories && kernel_arg(2u) == LABEL_X) set_arg(2u, LABEL_X - 0x10au + CATEGORY_X);
    const RecompFunction next = recomp_button_config_lookup_manual(TEXT_DRAW);
    if (next != NULL) next();
    else sub_000978E0();
}

/* 0x91560 (page, cursor, fade): the help line. */
static void help(void)
{
    const uint16_t page = (uint16_t)kernel_arg(1u), row = (uint16_t)kernel_arg(2u);
    uint32_t args[3] = {kernel_arg(1u), kernel_arg(2u), kernel_arg(3u)};
    unsigned message = 0u; /* the game's own */
    if (page == PC_PAGE && jukebox_mode) {
        args[0] = 0u;
        args[1] = 0u;
        put_text(jb_help, JB_HELP, recomp_jukebox_help(row));
        recomp_runner_text_redirect(0xc4u, RECOMP_RUNNER_TEXT_JUKEBOX_HELP);
        const uint32_t eax = kernel_call_lifted(sub_00091560, args, 3u);
        recomp_runner_text_redirect(0u, 0u);
        kernel_return_caller_cleanup(eax);
        return;
    } else if (page == PC_PAGE) {
        args[0] = 0u;
        args[1] = 0u;
        const int setting = setting_at(row);
        message = RECOMP_RUNNER_TEXT_PC + (on_categories ? MSG_CATEGORY_HELP + category : MSG_HELP +
            (setting < 0 ? HELP_NEXT : live[setting] ? HELP_NOW : after_online[setting] ? HELP_LEAVE : HELP_NEXT));
    } else {
        message = recomp_button_config_help(page, row, args);
    }
    /* The help line draws message 0xc4 for the row passed. */
    if (message != 0u) recomp_runner_text_redirect(0xc4u, message);
    const uint32_t eax = kernel_call_lifted(sub_00091560, args, 3u);
    recomp_runner_text_redirect(0u, 0u);
    kernel_return_caller_cleanup(eax);
}

/* ---- Page 19 ----------------------------------------------------------- */

static void page_layout(void)
{
    const uint16_t page = (uint16_t)kernel_arg(1u);
    const uint32_t own = recomp_button_config_layout(page);
    if (page == PC_PAGE) kernel_return_caller_cleanup(jukebox_mode ? jb_layout : drawing_categories ? category_layout : layout);
    else if (own != 0u) kernel_return_caller_cleanup(own);
    else sub_00090AD0();
}

/* 0x90f00 (context, layout, value mask, cursor, rows, visible). */
static void page_cursor(void)
{
    const uint32_t own = recomp_button_config_cursor(kernel_arg(2u));
    if (own != 0u) set_arg(2u, own);
    sub_00090F00();
}

/* 0x910a0 (page, row) for labels; for values 0x93270 asks 0x91370 (page,
   value row) for a kind and then 0x910a0 (kind, choice) for the text. */
static void page_text(void)
{
    if (recomp_button_config_text()) return;
    if (drawing_pc && jukebox_mode) {
        const uint16_t key = (uint16_t)kernel_arg(1u);
        if (key == PC_PAGE && kernel_arg(2u) < JB_ROWS) {
            const unsigned row = kernel_arg(2u);
            kernel_return_caller_cleanup(put_text(jb_labels + 2u * JB_TEXT * row, JB_TEXT, recomp_jukebox_label(row)));
            return;
        }
        if (key >= VALUE_KIND && key < VALUE_KIND + JB_ROWS) {
            const unsigned row = key - VALUE_KIND;
            kernel_return_caller_cleanup(put_text(jb_texts + 2u * JB_TEXT * row, JB_TEXT, recomp_jukebox_value(row)));
            return;
        }
    }
    if (drawing_pc) {
        const uint16_t key = (uint16_t)kernel_arg(1u);
        if (key == PC_PAGE && drawing_categories && kernel_arg(2u) < CATEGORIES) {
            kernel_return_caller_cleanup(text_for(MSG_CATEGORIES + kernel_arg(2u)));
            return;
        }
        const int setting = setting_at(key == PC_PAGE ? kernel_arg(2u) : key - VALUE_KIND);
        if (key == PC_PAGE && setting >= 0) {
            kernel_return_caller_cleanup(text_for(MSG_ROWS + (unsigned)setting));
            return;
        }
        if (key >= VALUE_KIND && key < VALUE_KIND + SHOWN && setting >= 0) {
            const uint32_t choice = kernel_arg(2u) & 0xffffu;
            kernel_return_caller_cleanup(text_for(value_message((unsigned)setting) + (choice < CHOICES[setting] ? choice : 0u)));
            return;
        }
    }
    sub_000910A0();
}

/* Every shown row has a value, so the value row is the row. */
static void value_text(void)
{
    if ((uint16_t)kernel_arg(1u) == PC_PAGE && kernel_arg(2u) < page_rows())
        kernel_return_caller_cleanup(VALUE_KIND + kernel_arg(2u));
    else sub_00091370();
}

static void pc_enter(void)
{
    if (jukebox_mode) {
        reset_jb_layout();
    } else {
        reset_layout();
        reset_category_layout();
    }
    kernel_return_caller_cleanup(0u);
}

/* As page 8's 0x93df0 and 0x93e90: rows, values, then the cursor. PC
   Settings draws its category list first the same way, without values; the
   list the cursor is not in shows no cursor. */
static void pc_page_draw(uint32_t visible)
{
    const uint32_t context = kernel_arg(1u), fade = *recomp_memory_u32(context + 0x10u);
    const uint32_t cursor = u16(CURSORS + 2u);
    static unsigned reported;
    if (reported < 2u) {
        fprintf(stderr, "recomp pc settings: page draws, visible %u, cursor %u\n", visible, cursor);
        ++reported;
    }
    drawing_pc = true;
    if (!jukebox_mode) {
        drawing_categories = true;
        const uint32_t labels[4] = {PC_PAGE, category, fade, visible};
        kernel_call_lifted(sub_00091440, labels, 4u);
        const uint32_t mark[6] = {context, category_layout, 0u, on_categories ? category : NO_CURSOR, CATEGORIES, visible};
        kernel_call_lifted(sub_00090F00, mark, 6u);
        drawing_categories = false;
        refresh_shown();
    } else {
        refresh_jukebox();
    }
    const unsigned rows_shown = page_rows();
    const uint32_t row_cursor = jukebox_mode || !on_categories ? cursor : NO_CURSOR, mask = (1u << rows_shown) - 1u;
    const uint32_t rows[4] = {PC_PAGE, row_cursor, fade, visible};
    kernel_call_lifted(sub_00091440, rows, 4u);
    const uint32_t value_args[6] = {PC_PAGE, row_cursor, fade, mask, jukebox_mode ? jb_values : shown, visible};
    kernel_call_lifted(sub_00093270, value_args, 6u);
    /* (context, layout, value mask, cursor, rows, visible): the bars and the
       cursor slide out from the left while visible and back while not. */
    const uint32_t mark[6] = {context, jukebox_mode ? jb_layout : layout, mask, row_cursor, rows_shown, visible};
    const uint32_t eax = kernel_call_lifted(sub_00090F00, mark, 6u);
    drawing_pc = false;
    kernel_return_caller_cleanup(eax);
}
static void pc_draw(void) { pc_page_draw(1u); }
static void pc_leave(void) { pc_page_draw(0u); }

RecompFunction recomp_pc_settings_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case OPTIONS_INIT: return options_init;
    case OPTIONS_UPDATE: return options_update;
    case ROW_COUNT: return row_count_main;
    case ROW_COUNT_ALIAS: return row_count_alias;
    case OPTIONS_DRAW: return options_draw;
    case SPRITE: return sprite;
    case TEXT_DRAW: return text_draw;
    case HELP: return help;
    case PAGE_LAYOUT: return page_layout;
    case PAGE_TEXT: return page_text;
    case VALUE_TEXT: return value_text;
    case PAGE_CURSOR: return page_cursor;
    case PC_ENTER: return pc_enter;
    case PC_DRAW: return pc_draw;
    case PC_LEAVE: return pc_leave;
    default: return recomp_button_config_lookup_manual(guest_address);
    }
}

