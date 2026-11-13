/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "button_config.h"
#include "input_host_win32.h"
#include "kernel_abi.h"
#include "runner_assets.h"

#include <string.h>

/* The Controller page (4, frontend_options_controller_update at 0x11c330)
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
    CONTROLLER_PAGE = 4,
    REBIND_ROW = 3,
    CONTROLLER_ROWS = 5,
    CONTROLLER_RECORDS = CONTROLLER_ROWS + 2, /* rows, then rows 1 and 2's values */

    CURSORS = 0x00df4ac8u,
    DEPTH = 0x00df4ad4u,
    PAGE = 0x00df4ad8u,
    CONTROLLER_LAYOUT = 0x0067adf0u, /* page 4's: four rows, then two values */

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

    CONTROLLER_RESET = 0x00091ca0u, /* page 4's start and enter: zeroes its layout's animation */
    PAGE_LABELS = 0x00091440u,
    CONFIG_UPDATE = 0x0011d480u,
    CONFIG_LEAVE = 0x0011b260u,    /* back to page 4 at depth 1 */
    CONFIG_DRAW = 0x00096720u,
    CONFIG_LIST = 0x000955e0u,     /* the left list */
    PAD_TYPE = 0x000fab80u,        /* (port): 4 makes the list two rows */
    LIST_SPRITE = 0x00097c10u,
    TEXT_DRAW = 0x000978e0u,       /* (text, x, y, layer, fade, brightness, align, scale) */
    CONFIG_SPRITE = 0x00097790u,   /* (sprite, x, y, layer, fade, ...) */
    CONFIG_LINE = 0x000b1700u,     /* a line from the chosen plate to its button */
};

extern void sub_000212E0(void);
extern void sub_00091440(void);
extern void sub_000955E0(void);
extern void sub_00096720(void);
extern void sub_00097790(void);
extern void sub_000978E0(void);
extern void sub_00097C10(void);
extern void sub_000FAB80(void);
extern void sub_000B1700(void);
extern void sub_0011AB90(void);
extern void sub_0011B260(void);
extern void sub_0011B8D0(void);
extern void sub_0011B9F0(void);
extern void sub_0011BCA0(void);
extern void sub_0011BDA0(void);
extern void sub_0011D480(void);
extern void sub_00134950(void);
extern void sub_00134F50(void);

/* ---- Text ------------------------------------------------------------ */

typedef struct RebindText {
    const recomp_utf16 *rebind, *rebind_help; /* the Controller page's row */
    /* The devices, the second plates, the help lines (devices, plates,
       listening) and the listening plate. */
    const recomp_utf16 *devices[2], *labels[SECOND_SET], *helps[3], *press[2];
} RebindText;

/* English, French, German, Italian and Spanish; Japanese shows English. */
static const RebindText REBIND_LANGUAGES[5] = {
    {u"Rebind", u"Change the keyboard keys and controller buttons.",
     {u"Keyboard", u"Controller"}, {u"Up", u"Down", u"Left", u"Right", u"Start", u"Back"},
     {u"Choose the keyboard or the controllers.", u"A: change. Left/Right: more buttons.",
      u"Press the new key or button. The other device cancels."},
     {u"Press a key", u"Press a button"}},
    {u"R\u00e9assigner", u"Modifier les touches du clavier et les boutons de la manette.",
     {u"Clavier", u"Manette"}, {u"Haut", u"Bas", u"Gauche", u"Droite", u"Start", u"Back"},
     {u"Choisissez le clavier ou les manettes.", u"A : modifier. Gauche/Droite : autres boutons.",
      u"Appuyez sur la nouvelle touche ou le nouveau bouton. L'autre appareil annule."},
     {u"Une touche...", u"Un bouton..."}},
    {u"Tastenbelegung", u"Tastatur- und Controllerbelegung \u00e4ndern.",
     {u"Tastatur", u"Controller"}, {u"Hoch", u"Runter", u"Links", u"Rechts", u"Start", u"Back"},
     {u"W\u00e4hle die Tastatur oder die Controller.", u"A: \u00e4ndern. Links/Rechts: weitere Tasten.",
      u"Dr\u00fccke die neue Taste. Das andere Ger\u00e4t bricht ab."},
     {u"Taste...", u"Knopf..."}},
    {u"Riassegna", u"Cambia i tasti della tastiera e i pulsanti del controller.",
     {u"Tastiera", u"Controller"}, {u"Su", u"Gi\u00f9", u"Sinistra", u"Destra", u"Start", u"Back"},
     {u"Scegli la tastiera o i controller.", u"A: cambia. Sinistra/Destra: altri pulsanti.",
      u"Premi il nuovo tasto o pulsante. L'altro dispositivo annulla."},
     {u"Un tasto...", u"Un pulsante..."}},
    {u"Reasignar", u"Cambia las teclas y los botones del mando.",
     {u"Teclado", u"Mando"}, {u"Arriba", u"Abajo", u"Izquierda", u"Derecha", u"Start", u"Back"},
     {u"Elige el teclado o los mandos.", u"A: cambiar. Izquierda/Derecha: m\u00e1s botones.",
      u"Pulsa la nueva tecla o bot\u00f3n. El otro dispositivo cancela."},
     {u"Una tecla...", u"Un bot\u00f3n..."}},
};

/* The messages this screen adds after PC SETTINGS': the Controller page's
   row, then its devices, the second plates, the help lines and the
   listening plate. */
enum {
    MSG_REBIND = 0, MSG_REBIND_HELP, MSG_DEVICES,
    MSG_LABELS = MSG_DEVICES + 2, MSG_HELPS = MSG_LABELS + SECOND_SET,
    MSG_PRESS = MSG_HELPS + 3, MSG_COUNT = MSG_PRESS + 2
};
enum { HELP_DEVICES, HELP_PLATES, HELP_LISTEN };

/* ---- State ----------------------------------------------------------- */

static RecompPcSettingsHost host;
static unsigned text_base;
static uint32_t controller_layout; /* page 4's with Rebind: CONTROLLER_RECORDS, then 0 */
static bool rebinding;             /* Button Config shows the bindings */
static bool drawing_config;        /* ... and its draw is running */
static bool drawing_list;          /* ... and its left list's */
static bool drawing_labels;        /* the Controller page's labels are being drawn */
static uint32_t plate_set;         /* guest u16: 0 the eight buttons, 1 the d-pad, Start and Back */
static uint32_t plate_names;       /* guest UTF-16, NAME_CHARS per plate */

/* ---- Guest helpers --------------------------------------------------- */

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
    recomp_runner_text_lookup(text_base + message, game_language(), &text);
    return text;
}

/* ---- Install --------------------------------------------------------- */

void recomp_button_config_install(const RecompPcSettingsHost *with, unsigned base)
{
    host = *with;
    text_base = base;
    rebinding = drawing_config = drawing_list = drawing_labels = false;
    controller_layout = recomp_runner_alloc(28u * CONTROLLER_RECORDS + 4u, 4u);
    plate_set = recomp_runner_alloc(2u, 4u);
    plate_names = recomp_runner_alloc(2u * NAME_CHARS * PLATES, 4u);

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

    static const recomp_utf16 *texts[5][MSG_COUNT];
    for (unsigned l = 0u; l < 5u; ++l) {
        const RebindText *t = &REBIND_LANGUAGES[l];
        unsigned m = 0u;
        texts[l][m++] = t->rebind;
        texts[l][m++] = t->rebind_help;
        for (unsigned i = 0u; i < 2u; ++i) texts[l][m++] = t->devices[i];
        for (unsigned i = 0u; i < SECOND_SET; ++i) texts[l][m++] = t->labels[i];
        for (unsigned i = 0u; i < 3u; ++i) texts[l][m++] = t->helps[i];
        for (unsigned i = 0u; i < 2u; ++i) texts[l][m++] = t->press[i];
    }
    for (unsigned m = 0u; m < MSG_COUNT; ++m) {
        const recomp_utf16 *text[6];
        for (unsigned l = 0u; l < 6u; ++l) text[l] = texts[l >= 2u ? l - 1u : 0u][m];
        recomp_runner_text_set(text_base + m, text);
    }
}

/* ---- The Controller page's row --------------------------------------- */

bool recomp_button_config_input(void)
{
    if (u16(PAGE) != CONTROLLER_PAGE || host.listen == NULL ||
        u16(CURSORS + 2u * u16(DEPTH)) != REBIND_ROW || (call1(sub_0011BDA0, 1u) & 0xffu) == 0u)
        return false;
    /* As row 0 opens Button Config, on the device list. */
    call1(sub_00134F50, BUTTON_CONFIG_SCENE);
    set_u16(DEPTH, DEVICE_DEPTH);
    set_u16(PAGE, DEVICE_PAGE);
    set_u16(CURSORS + 2u * DEVICE_DEPTH, 0u);
    rebinding = true;
    return true;
}

bool recomp_button_config_rows(uint16_t page, uint32_t *rows)
{
    if (page != CONTROLLER_PAGE) return false;
    *rows = CONTROLLER_ROWS;
    return true;
}

uint32_t recomp_button_config_layout(uint16_t page)
{
    return page == CONTROLLER_PAGE ? controller_layout : 0u;
}

uint32_t recomp_button_config_cursor(uint32_t template_address)
{
    return template_address == CONTROLLER_LAYOUT ? controller_layout : 0u;
}

/* 0x910a0 (page, row): the Rebind row's label, and the rows after it are the
   game's one row earlier. */
bool recomp_button_config_text(void)
{
    if (!drawing_labels || (uint16_t)kernel_arg(1u) != CONTROLLER_PAGE) return false;
    const uint32_t row = kernel_arg(2u);
    if (row == REBIND_ROW) {
        kernel_return_caller_cleanup(text_for(MSG_REBIND));
        return true;
    }
    if (row > REBIND_ROW) set_arg(2u, row - 1u);
    return false;
}

unsigned recomp_button_config_help(uint16_t page, uint16_t row, uint32_t args[3])
{
    if (page == CONTROLLER_PAGE) {
        if (row == REBIND_ROW) {
            args[0] = 0u;
            args[1] = 0u;
            return text_base + MSG_REBIND_HELP;
        }
        if (row > REBIND_ROW) args[1] = row - 1u;
    } else if (rebinding && (page == DEVICE_PAGE || page == PLATE_PAGE)) {
        args[0] = 0u;
        args[1] = 0u;
        return text_base + MSG_HELPS +
            (page == DEVICE_PAGE ? HELP_DEVICES : host.listening() ? HELP_LISTEN : HELP_PLATES);
    }
    return 0u;
}

/* ---- Button Config, rebinding ---------------------------------------- */

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

/* 0x91440 draws a page's labels through 0x910a0; the Controller page's
   Rebind row and the rows after it are ours. */
static void page_labels(void)
{
    drawing_labels = (uint16_t)kernel_arg(1u) == CONTROLLER_PAGE;
    sub_00091440();
    drawing_labels = false;
}

static void controller_reset(void)
{
    for (unsigned record = 0u; record < CONTROLLER_RECORDS; ++record)
        set_u32(controller_layout + record * 28u + 0xcu, 0u);
    kernel_return_caller_cleanup(0u);
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

RecompFunction recomp_button_config_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case PAGE_LABELS: return page_labels;
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
    default: return NULL;
    }
}
