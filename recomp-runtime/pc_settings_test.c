/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings.h"
#include "runner_assets.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest memory in 64 KiB pages, made on first touch. */
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t *pages[0x4000000u >> 16];
uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x4000000u || width > 0x10000u || ((address + width - 1u) >> 16) != (address >> 16)) {
        if (address < 0x4000000u && address + width <= 0x4000000u && width <= 0x10000u) {
            /* Spans two pages: keep them adjacent by allocating as one block is not
               possible, so tests keep their accesses inside a page. */
        }
        fprintf(stderr, "bad access %08x+%zu\n", address, width);
        abort();
    }
    uint8_t **page = &pages[address >> 16];
    if (*page == NULL) *page = calloc(1u, 0x10000u);
    return *page + (address & 0xffffu);
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)(void *)recomp_memory(address, 4u); }
static uint32_t rd32(uint32_t a) { return *recomp_memory_u32(a); }
static void wr32(uint32_t a, uint32_t v) { *recomp_memory_u32(a) = v; }
static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, recomp_memory(a, 2u), 2u); return v; }
static void wr16(uint32_t a, uint16_t v) { memcpy(recomp_memory(a, 2u), &v, 2u); }

enum {
    CURSORS = 0x00df4ac8u, DEPTH = 0x00df4ad4u, PAGE = 0x00df4ad8u, CONTROLLER_STATE = 0x00df4adcu,
    STACK = 0x00100f00u,
};

/* What a call to a hook looks like: arguments above a return address. */
static uint32_t hook(uint32_t address, const uint32_t *args, unsigned count)
{
    RecompFunction function = recomp_pc_settings_lookup_manual(address);
    if (function == NULL) function = recomp_runner_assets_lookup_manual(address);
    if (function == NULL) { fprintf(stderr, "no hook %08x\n", address); abort(); }
    const uint32_t saved = recomp_runtime.registers.esp;
    uint32_t esp = saved - 0x40u;
    for (unsigned i = 0; i < count; ++i) wr32(esp + 4u + 4u * i, args[i]);
    wr32(esp, 0u);
    recomp_runtime.registers.esp = esp;
    function();
    if (recomp_runtime.registers.esp != esp + 4u) { fprintf(stderr, "hook %08x left esp off\n", address); abort(); }
    recomp_runtime.registers.esp = saved;
    return recomp_runtime.registers.eax;
}
static uint32_t arg(unsigned i) { return rd32(recomp_runtime.registers.esp + 4u * i); }
static void ret(uint32_t eax) { recomp_runtime.registers.eax = eax; recomp_runtime.registers.esp += 4u; }

/* ---- The game, as far as the hooks see it -------------------------------- */
static unsigned language = 1u;
static int nav_delta, step_delta;
static uint8_t back_pressed, confirm_pressed;
static unsigned scene, left_options, original_update, invalidated;
typedef struct { uint32_t id, x, y, color; } Drawn;
static Drawn drawn[64];
static unsigned drawn_count;
static uint32_t bars[16], bar_count, preview_count, help_page, help_cursor, help_text;
static uint32_t labels[16], values[16], label_count, value_count, cursor_marks;

void sub_0011CFC0(void) { ++original_update; ret(1u); }
void sub_0011AC90(void) { ret(4u); }
void sub_0011AD10(void) { ret(4u); }
void sub_0011B8D0(void)
{
    const uint32_t cursor = arg(1u), count = arg(2u);
    wr16(cursor, (uint16_t)((rd16(cursor) + count + (unsigned)nav_delta) % count));
    ret(0u);
}
void sub_0011BCA0(void) { ret(back_pressed); }
void sub_0011BDA0(void) { ret(confirm_pressed); }
void sub_0011B9F0(void)
{
    const uint32_t value = arg(1u), count = arg(2u);
    wr16(value, (uint16_t)((rd16(value) + count + (unsigned)step_delta) % count));
    ret(0u);
}
void sub_0011CF20(void) { ret(7u); }
void sub_00134950(void) { ret(0u); }
void sub_00134F50(void) { scene = arg(1u); ret(0u); }
void sub_00020A70(void) { ++left_options; ret(0u); }
void sub_000212E0(void) { ret(language); }
void sub_00097830(void)
{
    if (drawn_count < 64u) drawn[drawn_count++] = (Drawn){arg(1u), arg(2u), arg(3u), arg(6u)};
    ret(0u);
}
void sub_000976F0(void) { if (bar_count < 16u) bars[bar_count++] = arg(2u); ret(0u); }
/* The top list as 0x95a70 draws it: a glow at the cursor, seven rows and bars. */
void sub_00095A70(void)
{
    const uint32_t white[6] = {0x3000du, 0x3000du, 0x3001bu, 0x30029u, 0x30045u, 0x30037u};
    const uint32_t glow[6] = {0x30006u, 0x30006u, 0x30014u, 0x30022u, 0x3003eu, 0x30030u};
    for (uint32_t y = 0x5cu; y < 0x112u; y += 0x1au) {
        const uint32_t bar[7] = {0u, y, 0x21eu, 0x16u, 0u, 4u, 0u};
        hook(0x000976f0u, bar, 7u);
    }
    const uint32_t cursor = rd16(CURSORS);
    const uint32_t g[9] = {glow[language] + cursor, 0xf6u, 0x56u + cursor * 26u, 3u, 0u, 0xffffffu, 0x3f800000u, 2u, 0u};
    hook(0x00097830u, g, 9u);
    for (uint32_t row = 0u; row < 7u; ++row) {
        const uint32_t w[9] = {white[language] + row, 0xf0u, 0x5du + row * 26u, 3u, 0u,
            row == cursor ? 0xffffffu : 0x808080u, 0x3f800000u, 2u, 0u};
        hook(0x00097830u, w, 9u);
    }
    ret(1u);
}
void sub_00095A10(void)
{
    const uint32_t page = rd16(CURSORS) + 1u;
    preview_count = hook(0x0011ac90u, &page, 1u);
    ret(0u);
}
void sub_00091560(void)
{
    help_page = arg(1u);
    help_cursor = arg(2u);
    const uint32_t message[2] = {0xc4u + help_cursor, 0xffu};
    help_text = hook(0x0006a3e0u, message, 2u);
    ret(0u);
}
void sub_0006A3E0(void) { ret(0x1234u); }
void sub_000AA970(void) { ret(0x5555u); }
void sub_000AA9B0(void) { ret(0x6666u); }
void sub_00090AD0(void) { ret(0x0067a540u); }
void sub_000910A0(void) { ret(0x9999u); }
void sub_00091370(void) { ret(3u); }
/* 0x91440 and 0x93270 ask for each row's text the way the game does. */
void sub_00091440(void)
{
    const uint32_t page = arg(1u);
    const uint32_t count = hook(0x0011ac90u, &page, 1u);
    label_count = 0u;
    for (uint32_t row = 0u; row < count && row < 16u; ++row) {
        const uint32_t key[2] = {page, row};
        labels[label_count++] = hook(0x000910a0u, key, 2u);
    }
    ret(0u);
}
void sub_00093270(void)
{
    const uint32_t page = arg(1u), table = arg(5u);
    const uint32_t count = hook(0x0011ac90u, &page, 1u);
    value_count = 0u;
    for (uint32_t row = 0u; row < count && row < 16u; ++row) {
        /* As 0x93270: (page, value row) gives a kind; (kind, choice) the text. */
        const uint32_t which[2] = {page, row};
        const uint32_t kind = hook(0x00091370u, which, 2u);
        const uint32_t key[2] = {kind, rd16(table + 2u * row)};
        values[value_count++] = hook(0x000910a0u, key, 2u);
    }
    ret(0u);
}
/* Like 0x90b60: every layout record up to the end word gets animation state. */
static uint32_t layout_walked, cursor_visible = 99u;
void sub_00090F00(void)
{
    ++cursor_marks;
    cursor_visible = arg(6u);
    layout_walked = 0u;
    for (uint32_t record = arg(2u); rd32(record) != 0u && layout_walked < 64u; record += 28u, ++layout_walked)
        wr32(record + 0xcu, rd32(record + 0xcu) + 1u);
    ret(0u);
}
void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size) { (void)base; (void)size; ++invalidated; }
void recomp_stop(int code, const char *format, ...) { fprintf(stderr, "stop %d: %s\n", code, format); abort(); }

/* ---- Host ---------------------------------------------------------------------- */
static unsigned changed_setting = 99u, changed_choice = 99u, closed;
static void host_changed(unsigned setting, unsigned choice, void *context) { (void)context; changed_setting = setting; changed_choice = choice; }
static void host_closed(void *context) { (void)context; ++closed; }

static int fails;
static void expect(const char *what, int ok) { if (!ok) { fprintf(stderr, "FAIL pc settings: %s\n", what); ++fails; } }

static char ascii(uint32_t text)
{
    return (char)rd16(text);
}

/* The Options label art: group 3's words, each 250x18 and solid white, on
   one P8 256x256 texture whose palette entry 1 is opaque white. */
static void make_label_art(void)
{
    const uint32_t defs = 0x00200000u, entries = 0x00210000u, texture = 0x00220000u, palette = 0x00221000u,
                   pixels = 0x00230000u, colours = 0x00240000u;
    wr32(0x00686698u + 4u * 3u, defs);
    wr32(0x0067ce78u + 4u * 3u, entries);
    for (unsigned i = 0; i < 0x50u; ++i) {
        const int16_t rect[4] = {0, 0, 250, 18};
        memcpy(recomp_memory(defs + 12u * i, 8u), rect, 8u);
        wr16(defs + 12u * i + 8u, 4u);
        wr16(defs + 12u * i + 10u, 3u);
    }
    wr32(entries + 8u * 4u, texture);
    wr32(entries + 8u * 4u + 4u, palette);
    wr32(texture + 4u, pixels);
    wr32(texture + 12u, 8u << 24 | 8u << 20 | 1u << 16 | 0x0bu << 8 | 0x2au);
    wr32(palette + 4u, colours);
    wr32(colours + 4u, 0xffffffffu);
    memset(recomp_memory(pixels, 0x10000u), 1, 0x10000u);
}

static void make_layout(void)
{
    /* Page 8's layout, which page 19 copies. */
    for (unsigned r = 0; r < 5u; ++r) {
        wr32(0x0067a540u + 28u * r, 0x5d010au + r);
        wr32(0x0067a540u + 28u * r + 0x14u, 0x40000u + (r == 4u));
        wr32(0x0067a540u + 28u * r + 0x18u, 0x883ec0u);
    }
}

int main(void)
{
    recomp_runtime.registers.esp = STACK;
    make_layout();
    const uint8_t choices[RECOMP_PC_SETTING_COUNT] = {2, 5, 1, 4, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1};
    const RecompPcSettingsHost host = {host_changed, host_closed, NULL};
    recomp_runner_assets_reset();
    recomp_pc_settings_install(&host, choices);
    /* The layout: 8 row records (Page and a view's seven), 8 value records, then the end word. */
    uint32_t page = 19u;
    const uint32_t layout = hook(0x00090ad0u, &page, 1u);
    expect("layout rows", rd32(layout) == (0x5du << 16 | 0x10au) && rd32(layout + 7u * 28u) == ((0x5du + 182u) << 16 | 0x10au));
    expect("layout values", rd32(layout + 8u * 28u) == (0x5du << 16 | 0x1e0u) &&
        rd32(layout + 8u * 28u + 0x14u) == 0x40001u && rd32(layout + 15u * 28u + 0x18u) == 0x883ec0u);
    expect("layout end", rd32(layout + 16u * 28u) == 0u && layout >= XBOX_RUNNER_ASSET_BASE &&
        layout + 16u * 28u + 4u <= XBOX_RUNNER_ASSET_BASE + XBOX_RUNNER_ASSET_SIZE);
    expect("page tables", rd32(0x0067af50u + 4u * 19u) != 0u && rd32(0x0067aff0u + 4u * 19u) != 0u &&
        rd32(0x0067afa0u + 4u * 19u) != 0u && rd32(0x0067af00u + 4u * 19u) != 0u &&
        rd32(0x0067b040u + 4u * 19u) != 0u);
    expect("choices kept", recomp_pc_settings_choice(RECOMP_PC_SCREEN) == 4u &&
        recomp_pc_settings_choice(RECOMP_PC_INTRO) == 1u);
    expect("choice counts", recomp_pc_settings_choices(RECOMP_PC_MSAA) == 6u && recomp_pc_settings_choices(99u) == 0u);
    expect("live rows", recomp_pc_settings_applies_now(RECOMP_PC_UNLOCKS) &&
        recomp_pc_settings_applies_now(RECOMP_PC_FREE_CAMERA) && !recomp_pc_settings_applies_now(RECOMP_PC_SCREEN) &&
        !recomp_pc_settings_applies_now(RECOMP_PC_MSAA));
    recomp_pc_settings_set_live(RECOMP_PC_MSAA, true);
    recomp_pc_settings_set_live(RECOMP_PC_FREE_CAMERA, false); /* as online */
    expect("renderer rows", recomp_pc_settings_applies_now(RECOMP_PC_MSAA) &&
        !recomp_pc_settings_applies_now(RECOMP_PC_FREE_CAMERA) && recomp_pc_settings_applies_now(RECOMP_PC_UNLOCKS));

    /* Row counts: the top list has eight rows, page 19 eight (Page and seven), others the game's. */
    page = 0u;
    expect("top rows", hook(0x0011ac90u, &page, 1u) == 8u && hook(0x0011ad10u, &page, 1u) == 8u);
    page = 19u;
    expect("pc rows", hook(0x0011ac90u, &page, 1u) == 8u);
    page = 8u;
    expect("game rows", hook(0x0011ac90u, &page, 1u) == 4u);

    /* Drawing the top list with the cursor on PC SETTINGS: no label until the
       game's label art is loaded. */
    wr16(DEPTH, 0u); wr16(PAGE, 0u); wr16(CURSORS, 5u);
    const uint32_t context = 0x00300000u;
    hook(0x00095a70u, &context, 1u);
    expect("label waits for the art", invalidated == 0u);
    make_label_art();
    drawn_count = 0u; bar_count = 0u;
    hook(0x00095a70u, &context, 1u);
    /* P C _ S E T T I N G S: 3.24 lean + 11.5 11.5 7 11 10 10 10 7 11.5 12 11. */
    const uint32_t white_id = 0x1f0u << 16, glow_id = 0x1f0u << 16 | 1u;
    const uint32_t white_rect = hook(0x000aa970u, &white_id, 1u), glow_rect = hook(0x000aa970u, &glow_id, 1u);
    expect("label made", invalidated == 2u && rd16(white_rect + 4u) == 117u && rd16(glow_rect + 4u) == 117u + 12u);
    const uint32_t white_texture = rd32(hook(0x000aa9b0u, &white_id, 1u));
    const uint32_t glow_texture = rd32(hook(0x000aa9b0u, &glow_id, 1u));
    int white_pixels = 0, glow_pixels = 0;
    for (uint32_t i = 0; i < 0x2000u; ++i) {
        white_pixels += *recomp_memory(rd32(white_texture + 4u) + i, 1u) != 0u;
        glow_pixels += *recomp_memory(rd32(glow_texture + 4u) + i, 1u) != 0u;
    }
    expect("label pixels", white_pixels > 200 && glow_pixels > white_pixels);
    expect("eight bars", bar_count == 8u && bars[7] == 0x5cu + 7u * 0x1au);
    expect("pc glow", drawn[0].id == (0x1f0u << 16 | 1u) && drawn[0].y == 0x56u + 5u * 26u);
    /* Drawn: glow, rows 0-3, PC, Video, Warning, Exit. */
    expect("draw count", drawn_count == 9u);
    expect("pc label", drawn[5].id == 0x1f0u << 16 && drawn[5].y == 0x5du + 5u * 26u && drawn[5].color == 0xffffffu);
    expect("video stays", drawn[6].id == 0x3000du + 4u && drawn[6].y == 0x5du + 4u * 26u && drawn[6].color == 0x808080u);
    expect("warning moves", drawn[7].id == 0x3000du + 5u && drawn[7].y == 0x5du + 6u * 26u && drawn[7].color == 0x808080u);
    expect("exit moves", drawn[8].id == 0x3000du + 6u && drawn[8].y == 0x5du + 7u * 26u);
    expect("sprite rect", recomp_runner_assets_lookup_manual(0x000aa970u) != NULL &&
        recomp_pc_settings_lookup_manual(0x000aa970u) == NULL && recomp_pc_settings_lookup_manual(0x0006a3e0u) == NULL);
    const uint32_t other = 0x3000du;
    expect("own sprites", rd16(white_rect + 8u) == 0u && rd16(white_rect + 10u) == 0x1f0u &&
        rd16(glow_rect + 8u) == 1u && hook(0x000aa970u, &other, 1u) == 0x5555u);
    expect("own textures", rd32(glow_texture + 12u) == 0x05810b2au && hook(0x000aa9b0u, &other, 1u) == 0x6666u);
    /* The game has 0x1a5 sprite groups and unloads them all; the last stays its own. */
    const uint32_t game_group = 0x1a4u << 16 | 1u;
    expect("game groups untouched", hook(0x000aa9b0u, &game_group, 1u) == 0x6666u &&
        hook(0x000aa970u, &game_group, 1u) == 0x5555u);

    /* The cursor on Exit (row 7): its glow is the game's Exit. */
    drawn_count = 0u; bar_count = 0u; wr16(CURSORS, 7u);
    hook(0x00095a70u, &context, 1u);
    expect("exit glow", drawn[0].id == 0x30006u + 6u && drawn[0].y == 0x56u + 7u * 26u);
    expect("exit lit", drawn[8].color == 0xffffffu && drawn[5].color == 0x808080u);
    /* Sprites outside the top list are the game's. */
    drawn_count = 0u;
    const uint32_t arrow[9] = {0x3004fu, 0x1efu, 0x61u, 3u, 0u, 0xffffffu, 0x3f800000u, 2u, 0u};
    hook(0x00097830u, arrow, 9u);
    expect("other sprites", drawn_count == 1u && drawn[0].id == 0x3004fu && drawn[0].y == 0x61u);

    /* The preview of the selected row's page, and the help line. */
    wr16(CURSORS, 5u);
    hook(0x00095a10u, &context, 1u);
    expect("pc preview", preview_count == 8u && rd16(CURSORS) == 5u);
    wr16(CURSORS, 7u);
    hook(0x00095a10u, &context, 1u);
    expect("exit preview", preview_count == 4u); /* the game's page 7 */
    const uint32_t top_help[3] = {0u, 5u, 0u};
    hook(0x00091560u, top_help, 3u);
    expect("pc help", help_page == 0u && help_cursor == 0u && help_text != 0x1234u &&
        ascii(help_text) == 'G'); /* "Graphics, ..." */
    const uint32_t warning_help[3] = {0u, 6u, 0u};
    hook(0x00091560u, warning_help, 3u);
    expect("warning help", help_cursor == 5u && help_text == 0x1234u);
    const uint32_t page_row_help[3] = {19u, 0u, 0u}, resolution_help[3] = {19u, 1u, 0u};
    hook(0x00091560u, page_row_help, 3u);
    expect("page help", help_page == 0u && ascii(help_text) == 'L'); /* "Left or right ..." */
    hook(0x00091560u, resolution_help, 3u);
    expect("next help", help_page == 0u && ascii(help_text) == 'T' && rd16(help_text + 26u) == 't'); /* "Takes effect the next ..." */
    const uint32_t message[2] = {0xc4u, 0xffu};
    expect("help only while drawing", hook(0x0006a3e0u, message, 2u) == 0x1234u);
    /* Texts other modules add through runner_assets: per language, past PC Settings' own. */
    static const recomp_utf16 quit_english[] = u"Quit", quit_french_text[] = u"Quitter";
    const recomp_utf16 *const quit_texts[6] = {quit_english, quit_english, quit_french_text,
        quit_english, quit_english, quit_english};
    recomp_runner_text_set(RECOMP_RUNNER_TEXT_QUIT, quit_texts);
    const uint32_t quit_french[2] = {RECOMP_RUNNER_TEXT_QUIT, 2u}, quit_unknown[2] = {RECOMP_RUNNER_TEXT_QUIT + 1u, 2u};
    const uint32_t french = hook(0x0006a3e0u, quit_french, 2u);
    expect("runner texts", ascii(french) == 'Q' && rd16(french + 8u) == 't' &&
        hook(0x0006a3e0u, quit_unknown, 2u) == 0x1234u);

    /* Top-list input: PC SETTINGS opens page 19; Warning still opens its scene. */
    wr16(CURSORS, 4u); nav_delta = 1; confirm_pressed = 1u;
    uint32_t none[2] = {0u, 0u};
    expect("top input", hook(0x0011cfc0u, none, 2u) == 1u);
    expect("opens pc", rd16(CURSORS) == 5u && rd16(DEPTH) == 1u && rd16(PAGE) == 19u && rd16(CURSORS + 2u) == 0u);
    wr16(DEPTH, 0u); wr16(PAGE, 0u); wr16(CURSORS, 5u); nav_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    expect("warning scene", scene == 0x17u && rd16(DEPTH) == 4u && rd16(PAGE) == 6u);
    wr16(DEPTH, 0u); wr16(PAGE, 0u); wr16(CURSORS, 2u); nav_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    expect("controller", rd16(PAGE) == 4u && rd16(CONTROLLER_STATE) == 7u && rd16(DEPTH) == 1u);
    wr16(DEPTH, 0u); wr16(PAGE, 0u); wr16(CURSORS, 3u); nav_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    expect("video scene", scene == 0x18u && rd16(PAGE) == 0xfu);
    wr16(DEPTH, 0u); wr16(PAGE, 0u); wr16(CURSORS, 0u); nav_delta = 0; confirm_pressed = 0u; back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    expect("leave options", left_options == 1u);
    back_pressed = 0u;
    wr16(DEPTH, 1u); wr16(PAGE, 8u);
    hook(0x0011cfc0u, none, 2u);
    expect("other pages", original_update == 1u);

    /* Page 19: the Page row turns to the second view, a value there changes,
       confirm on the Page row turns back, then leave. */
    wr16(DEPTH, 1u); wr16(PAGE, 19u); wr16(CURSORS + 2u, 0u); nav_delta = 0; step_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    expect("page turns", changed_setting == 99u);
    wr16(CURSORS + 2u, 1u); nav_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    expect("pc change", changed_setting == RECOMP_PC_UNLOCKS && changed_choice == 1u &&
        recomp_pc_settings_choice(RECOMP_PC_UNLOCKS) == 1u);
    const uint32_t unlocks_help[3] = {19u, 2u, 0u};
    hook(0x00091560u, unlocks_help, 3u);
    expect("live help", ascii(help_text) == 'T' && rd16(help_text + 26u) == 'i'); /* "Takes effect immediately." */
    wr16(CURSORS + 2u, 0u); nav_delta = 0; step_delta = 0; confirm_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    confirm_pressed = 0u;
    hook(0x00091560u, resolution_help, 3u);
    expect("confirm turns back", rd16(help_text + 26u) == 't'); /* row 1 is Resolution again */
    back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    expect("pc close", closed == 1u && rd16(DEPTH) == 0u && rd16(PAGE) == 0u);
    back_pressed = 0u;

    /* Page 19's draw: the game's renderers get its labels and values. */
    wr32(context + 0x10u, 0x3f800000u);
    wr16(CURSORS + 2u, 3u);
    const uint32_t draw = rd32(0x0067aff0u + 4u * 19u);
    hook(draw, &context, 1u);
    expect("labels", label_count == 8u && ascii(labels[0]) == 'P' && ascii(labels[1]) == 'R' && ascii(labels[7]) == 'A');
    expect("values", value_count == 8u && ascii(values[0]) == '1' && ascii(values[1u + RECOMP_PC_SCREEN]) == '3'); /* 32:9 */
    expect("cursor mark", cursor_marks == 1u && layout_walked == 16u && cursor_visible == 1u);
    expect("sprites intact", rd32(rd32(hook(0x000aa9b0u, &white_id, 1u)) + 12u) == 0x05810b2au);
    language = 3u; /* German */
    hook(draw, &context, 1u);
    expect("german", ascii(labels[0]) == 'S' && ascii(labels[1]) == 'A' && rd16(labels[1] + 8u) == 0xf6u); /* "Aufl\u00f6sung" */
    /* The second view: Intro Movies to KO Replay. */
    wr16(DEPTH, 1u); wr16(PAGE, 19u); wr16(CURSORS + 2u, 0u); step_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    step_delta = 0;
    hook(draw, &context, 1u);
    expect("second view", ascii(labels[1]) == 'I' && ascii(values[0]) == '2' &&
        ascii(values[1u + RECOMP_PC_UNLOCKS - 7u]) == 'E' && rd16(values[1u + RECOMP_PC_UNLOCKS - 7u] + 2u) == 'r'); /* Erspielen */
    expect("leave draw", rd32(0x0067afa0u + 4u * 19u) != draw);
    const uint32_t key[2] = {19u, 0u};
    expect("labels only while drawing", hook(0x000910a0u, key, 2u) == 0x9999u);
    hook(rd32(0x0067af50u + 4u * 19u), &context, 1u);
    expect("enter resets", rd32(layout + 0xcu) == 0u && rd32(layout + 15u * 28u + 0xcu) == 0u);

    /* A new language remakes the label (Italian, which needs the Z of OPZIONI). */
    language = 4u;
    wr16(DEPTH, 0u); wr16(PAGE, 0u);
    hook(0x00095a70u, &context, 1u);
    expect("relabel", invalidated == 4u);
    expect("unhooked", recomp_pc_settings_lookup_manual(0x0011c590u) == NULL);
    return fails == 0 ? 0 : 1;
}

