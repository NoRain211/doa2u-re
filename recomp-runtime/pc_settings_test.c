/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_settings.h"
#include "button_config.h"
#include "input_host_win32.h"
#include "jukebox.h"
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
static uint32_t help_page, help_cursor, help_text;
static uint32_t labels[16], values[16], label_count, value_count, cursor_marks;
/* Each 0x91440 call of a draw: its labels, their count and its cursor. */
static uint32_t label_lists[2][16], label_list_counts[2], label_cursors[2], label_calls;

void sub_0011CFC0(void) { ++original_update; ret(1u); }
static unsigned options_inits;
void sub_0011AFD0(void) { ++options_inits; ret(0u); }
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
static unsigned scene_leaves;
void sub_00134950(void) { ++scene_leaves; ret(0u); }
void sub_00134F50(void) { scene = arg(1u); ret(0u); }
void sub_00020A70(void) { ++left_options; ret(0u); }
void sub_000212E0(void) { ret(language); }
void sub_00097830(void)
{
    if (drawn_count < 64u) drawn[drawn_count++] = (Drawn){arg(1u), arg(2u), arg(3u), arg(6u)};
    ret(0u);
}
/* The top list as 0x95a70 draws it: a glow at the cursor and seven rows. */
void sub_00095A70(void)
{
    const uint32_t white[6] = {0x3000du, 0x3000du, 0x3001bu, 0x30029u, 0x30045u, 0x30037u};
    const uint32_t glow[6] = {0x30006u, 0x30006u, 0x30014u, 0x30022u, 0x3003eu, 0x30030u};
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
void sub_000910A0(void) { ret(0x9900u + (arg(2u) & 0xffu)); } /* the game's text for that row */
void sub_00091370(void) { ret(3u); }
/* 0x91440 and 0x93270 ask for each row's text the way the game does; 0x91440
   draws each label right-aligned at x 0x1d4. */
void sub_00091440(void)
{
    const uint32_t page = arg(1u), cursor = arg(2u);
    const uint32_t count = hook(0x0011ac90u, &page, 1u);
    label_count = 0u;
    for (uint32_t row = 0u; row < count && row < 16u; ++row) {
        const uint32_t key[2] = {page, row};
        labels[label_count++] = hook(0x000910a0u, key, 2u);
        const uint32_t text[8] = {labels[row], 0x1d4u, 0x60u + 0x1au * row, 3u, 0x3f800000u, 0x3f800000u, 2u, 0x3f59999au};
        hook(0x000978e0u, text, 8u);
    }
    if (label_calls < 2u) {
        memcpy(label_lists[label_calls], labels, sizeof labels);
        label_list_counts[label_calls] = label_count;
        label_cursors[label_calls] = cursor;
    }
    ++label_calls;
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
static uint32_t layout_walked, cursor_visible = 99u, mark_cursors[2];
void sub_00090F00(void)
{
    mark_cursors[cursor_marks % 2u] = arg(4u);
    ++cursor_marks;
    cursor_visible = arg(6u);
    layout_walked = 0u;
    for (uint32_t record = arg(2u); rd32(record) != 0u && layout_walked < 64u; record += 28u, ++layout_walked)
        wr32(record + 0xcu, rd32(record + 0xcu) + 1u);
    ret(0u);
}
void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size) { (void)base; (void)size; ++invalidated; }

/* Button Config, as far as its rebinding hooks see it. */
static unsigned config_updates, config_leaves, config_sprites, config_lines, list_sprites, pad_type_seen;
typedef struct { uint32_t text, x, y; } Text;
static Text texts[16];
static unsigned text_count;
void sub_0011D480(void) { ++config_updates; ret(1u); }
void sub_0011B260(void) { ++config_leaves; ret(1u); }
void sub_0011AB90(void) { ret(0u); }
void sub_000FAB80(void) { ret(1u); }
void sub_00097C10(void) { ++list_sprites; ret(0u); }
void sub_000978E0(void) { if (text_count < 16u) texts[text_count++] = (Text){arg(1u), arg(2u), arg(3u)}; ret(0u); }
void sub_00097790(void) { ++config_sprites; ret(0u); }
void sub_000B1700(void) { ++config_lines; ret(0u); }
/* The left list: its rows follow the pad type; Type A's label, then Free Settings'. */
void sub_000955E0(void)
{
    const uint32_t port = 0u;
    pad_type_seen = hook(0x000fab80u, &port, 1u);
    const uint32_t type_a[9] = {0x19a0000u, 0x82u, 0x6du, 1u, 0u, 0x3f800000u, 0x3f800000u, 0u, 0u};
    const uint32_t free_settings[9] = {0x19a0004u, 0x45u, 0x88u, 1u, 0u, 0x3f800000u, 0x3f800000u, 0u, 0u};
    hook(0x00097c10u, type_a, 9u);
    hook(0x00097c10u, free_settings, 9u);
    ret(0u);
}
/* The scene's draw: the list, then plates 0 and 6 (text, icon) and a line. */
void sub_00096720(void)
{
    const uint32_t context = arg(1u);
    hook(0x000955e0u, &context, 1u);
    for (uint32_t row = 0u; row < 8u; row += 6u) {
        const uint32_t text[8] = {0x9999u, 0x252u, 0x58u + 0x22u * row, 3u, 0u, 0x3f800000u, 1u, 0x3f59999au};
        hook(0x000978e0u, text, 8u);
        const uint32_t icon[7] = {0x19a0009u + row, 500u, 0x4fu + 0x22u * row, 1u, 0u, 0x3f800000u, 0x3f800000u};
        hook(0x00097790u, icon, 7u);
    }
    const uint32_t line[2] = {0u, 0u};
    hook(0x000b1700u, line, 2u);
    ret(1u);
}
void recomp_stop(int code, const char *format, ...) { fprintf(stderr, "stop %d: %s\n", code, format); abort(); }

/* ---- The Jukebox, faked ----------------------------------------------------- */
static bool jb_requested;
static unsigned jb_opened, jb_closed, jb_choice[RECOMP_JUKEBOX_ROWS], jb_confirmed = 99u;
void recomp_jukebox_request_page(void) { jb_requested = true; }
bool recomp_jukebox_take_request(void) { const bool r = jb_requested; jb_requested = false; return r; }
void recomp_jukebox_page_opened(void) { ++jb_opened; }
void recomp_jukebox_page_closed(void) { ++jb_closed; }
const char *recomp_jukebox_label(unsigned row) { static const char *const l[5] = {"ALBUM", "TRACK", "PLAY MODE", "ASSIGN TO", "ASSIGNED"}; return l[row]; }
unsigned recomp_jukebox_choices(unsigned row) { return row == 1u ? 46u : 3u; }
unsigned recomp_jukebox_choice(unsigned row) { return jb_choice[row]; }
void recomp_jukebox_choose(unsigned row, unsigned choice) { jb_choice[row] = choice; }
void recomp_jukebox_confirm(unsigned row) { jb_confirmed = row; }
static const char *jb_track = "Ayane";
const char *recomp_jukebox_value(unsigned row) { return row == 1u ? jb_track : "DOA4"; }
const char *recomp_jukebox_help(unsigned row) { (void)row; return "Help"; }

/* ---- Host ---------------------------------------------------------------------- */
static unsigned changed_setting = 99u, changed_choice = 99u, closed;
static unsigned rebinds;
static void host_changed(unsigned setting, unsigned choice, void *context) { (void)context; changed_setting = setting; changed_choice = choice; }
static void host_closed(void *context) { (void)context; ++closed; }
static unsigned listened = 99u, listened_pad = 99u, reset_pad = 99u;
static bool listening_now;
static const char *host_bound(unsigned binding, bool pad)
{
    return binding == RECOMP_INPUT_BIND_A ? (pad ? "LB" : "Space") : "W";
}
static void host_listen(unsigned binding, bool pad) { ++rebinds; listened = binding; listened_pad = pad; }
static bool host_listening(void) { return listening_now; }
static void host_reset(bool pad) { reset_pad = pad; }
static unsigned rebind_saves;
static void host_rebind_closed(void) { ++rebind_saves; }

static int fails;
static void expect(const char *what, int ok) { if (!ok) { fprintf(stderr, "FAIL pc settings: %s\n", what); ++fails; } }

static char ascii(uint32_t text)
{
    return (char)rd16(text);
}

static void make_layout(void)
{
    /* Page 8's layout, which page 19 copies. */
    for (unsigned r = 0; r < 5u; ++r) {
        wr32(0x0067a540u + 28u * r, 0x5d010au + r);
        wr32(0x0067a540u + 28u * r + 0x14u, 0x40000u + (r == 4u));
        wr32(0x0067a540u + 28u * r + 0x18u, 0x883ec0u);
    }
    /* Page 4's: four rows, then rows 1 and 2's values. */
    for (unsigned r = 0; r < 6u; ++r) {
        const uint32_t at = r < 4u ? (0x5du + 26u * r) << 16 | 0x10au : (0x77u + 26u * (r - 4u)) << 16 | 0x1e0u;
        wr32(0x0067adf0u + 28u * r, at);
        wr32(0x0067adf0u + 28u * r + 4u, at);
        wr32(0x0067adf0u + 28u * r + 0x14u, 0x40000u + (r >= 4u));
    }
}

int main(void)
{
    recomp_runtime.registers.esp = STACK;
    make_layout();
    const uint8_t choices[RECOMP_PC_SETTING_COUNT] = {2, 5, 1, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0};
    const RecompPcSettingsHost host = {host_changed, host_closed, NULL};
    const RecompButtonConfigHost rebind = {host_bound, host_listen, host_listening, host_reset, host_rebind_closed};
    recomp_runner_assets_reset();
    recomp_pc_settings_install(&host, choices);
    recomp_button_config_install(&rebind);
    /* The layout: Display's 6 row records, 6 value records, then the end word. */
    uint32_t page = 19u;
    const uint32_t layout = hook(0x00090ad0u, &page, 1u);
    expect("layout rows", rd32(layout) == (0x5du << 16 | 0x10au) && rd32(layout + 5u * 28u) == ((0x5du + 130u) << 16 | 0x10au));
    expect("layout values", rd32(layout + 6u * 28u) == (0x5du << 16 | 0x1e0u) &&
        rd32(layout + 6u * 28u + 0x14u) == 0x40001u && rd32(layout + 11u * 28u + 0x18u) == 0x883ec0u);
    expect("layout end", rd32(layout + 12u * 28u) == 0u && layout >= XBOX_RUNNER_ASSET_BASE &&
        layout + 12u * 28u + 4u <= XBOX_RUNNER_ASSET_BASE + XBOX_RUNNER_ASSET_SIZE);
    expect("page tables", rd32(0x0067af50u + 4u * 19u) != 0u && rd32(0x0067aff0u + 4u * 19u) != 0u &&
        rd32(0x0067afa0u + 4u * 19u) != 0u && rd32(0x0067af00u + 4u * 19u) != 0u &&
        rd32(0x0067b040u + 4u * 19u) != 0u);
    expect("choices kept", recomp_pc_settings_choice(RECOMP_PC_SCREEN) == 4u &&
        recomp_pc_settings_choice(RECOMP_PC_INTRO) == 1u && recomp_pc_settings_choice(RECOMP_PC_SHARPEN) == 0u &&
        recomp_pc_settings_choice(RECOMP_PC_AO) == 0u);
    expect("choice counts", recomp_pc_settings_choices(RECOMP_PC_MSAA) == 6u &&
        recomp_pc_settings_choices(RECOMP_PC_FRAME_RATE) == 2u &&
        recomp_pc_settings_choices(RECOMP_PC_SHARPEN) == 4u && recomp_pc_settings_choices(RECOMP_PC_AO) == 2u &&
        recomp_pc_settings_choices(RECOMP_PC_RENDERER) == 3u && recomp_pc_settings_choices(RECOMP_PC_DISPLAY_MODE) == 3u &&
        recomp_pc_settings_choices(RECOMP_PC_HITBOXES) == 3u &&
        recomp_pc_settings_choices(99u) == 0u);
    expect("live rows", recomp_pc_settings_applies_now(RECOMP_PC_UNLOCKS) &&
        recomp_pc_settings_applies_now(RECOMP_PC_FREE_CAMERA) &&
        recomp_pc_settings_applies_now(RECOMP_PC_DEDICATION) && recomp_pc_settings_applies_now(RECOMP_PC_FIGHT_HUD) &&
        recomp_pc_settings_applies_now(RECOMP_PC_HITBOXES) &&
        !recomp_pc_settings_applies_now(RECOMP_PC_SCREEN) &&
        !recomp_pc_settings_applies_now(RECOMP_PC_SHARPEN) && !recomp_pc_settings_applies_now(RECOMP_PC_AO) &&
        !recomp_pc_settings_applies_now(RECOMP_PC_MSAA));
    recomp_pc_settings_set_live(RECOMP_PC_MSAA, true);
    recomp_pc_settings_set_live(RECOMP_PC_SHARPEN, true);
    recomp_pc_settings_set_live(RECOMP_PC_AO, true);
    recomp_pc_settings_set_live(RECOMP_PC_FREE_CAMERA, false); /* as online */
    expect("renderer rows", recomp_pc_settings_applies_now(RECOMP_PC_MSAA) &&
        recomp_pc_settings_applies_now(RECOMP_PC_SHARPEN) && recomp_pc_settings_applies_now(RECOMP_PC_AO) &&
        !recomp_pc_settings_applies_now(RECOMP_PC_FREE_CAMERA) && recomp_pc_settings_applies_now(RECOMP_PC_UNLOCKS));

    /* Row counts: the top list is the game's, page 19 Display's six settings, others the game's. */
    page = 0u;
    expect("top rows", hook(0x0011ac90u, &page, 1u) == 4u && hook(0x0011ad10u, &page, 1u) == 4u);
    page = 19u;
    expect("pc rows", hook(0x0011ac90u, &page, 1u) == 6u);
    page = 8u;
    expect("game rows", hook(0x0011ac90u, &page, 1u) == 4u);

    /* The top list is the game's: seven rows and a glow, its input and help untouched. */
    wr16(DEPTH, 0u); wr16(PAGE, 0u); wr16(CURSORS, 5u);
    const uint32_t context = 0x00300000u;
    drawn_count = 0u;
    hook(0x00095a70u, &context, 1u);
    expect("game top list", drawn_count == 8u && drawn[6].id == 0x3000du + 5u && drawn[6].y == 0x5du + 5u * 26u);
    expect("sprite rect", recomp_runner_assets_lookup_manual(0x000aa970u) != NULL &&
        recomp_pc_settings_lookup_manual(0x000aa970u) == NULL && recomp_pc_settings_lookup_manual(0x0006a3e0u) == NULL);
    expect("no top-list hooks", recomp_pc_settings_lookup_manual(0x000976f0u) == NULL &&
        recomp_pc_settings_lookup_manual(0x00095a10u) == NULL);
    const uint32_t top_help[3] = {0u, 5u, 0u};
    hook(0x00091560u, top_help, 3u);
    expect("game help", help_page == 0u && help_cursor == 5u && help_text == 0x1234u);
    uint32_t none[2] = {0u, 0u};
    hook(0x0011cfc0u, none, 2u);
    expect("game top input", original_update == 1u);
    /* Texts other modules add through runner_assets: per language, past PC Settings' own. */
    static const recomp_utf16 quit_english[] = u"Quit", quit_french_text[] = u"Quitter";
    const recomp_utf16 *const quit_texts[6] = {quit_english, quit_english, quit_french_text,
        quit_english, quit_english, quit_english};
    recomp_runner_text_set(RECOMP_RUNNER_TEXT_QUIT, quit_texts);
    const uint32_t quit_french[2] = {RECOMP_RUNNER_TEXT_QUIT, 2u}, quit_unknown[2] = {RECOMP_RUNNER_TEXT_QUIT + 1u, 2u};
    const uint32_t french = hook(0x0006a3e0u, quit_french, 2u);
    expect("runner texts", ascii(french) == 'Q' && rd16(french + 8u) == 't' &&
        hook(0x0006a3e0u, quit_unknown, 2u) == 0x1234u);

    /* The main menu's PC SETTINGS: the next update opens page 19 alone, on its categories. */
    recomp_pc_settings_request_page();
    hook(0x0011cfc0u, none, 2u);
    expect("pc opens", rd16(DEPTH) == 1u && rd16(PAGE) == 19u && rd16(CURSORS + 2u) == 0u && original_update == 1u);
    drawn_count = 0u;
    hook(0x00095a70u, &context, 1u);
    expect("no top list beside it", drawn_count == 0u);
    const uint32_t category_help[3] = {19u, 0u, 0u};
    hook(0x00091560u, category_help, 3u);
    expect("category help", help_page == 0u && help_cursor == 0u && ascii(help_text) == 'D'); /* "Display mode, ..." */

    /* Page 19's draw: the category list, then Display's settings, without a cursor. */
    wr32(context + 0x10u, 0x3f800000u);
    const uint32_t draw = rd32(0x0067aff0u + 4u * 19u);
    label_calls = 0u; cursor_marks = 0u; text_count = 0u;
    hook(draw, &context, 1u);
    expect("categories", label_calls == 2u && label_list_counts[0] == 5u && ascii(label_lists[0][0]) == 'D' &&
        ascii(label_lists[0][1]) == 'G' && ascii(label_lists[0][4]) == 'H' && label_cursors[0] == 0u &&
        mark_cursors[0] == 0u);
    expect("categories drawn left", text_count == 11u && texts[0].x == 0x1d4u - 0x10au + 0x24u &&
        texts[4].x == texts[0].x && texts[5].x == 0x1d4u);
    expect("display settings", label_list_counts[1] == 6u && ascii(label_lists[1][0]) == 'D' &&
        ascii(label_lists[1][1]) == 'R' && label_cursors[1] == 0xffffu && mark_cursors[1] == 0xffffu);
    expect("display values", value_count == 6u && ascii(values[0]) == 'B' && ascii(values[1]) == '1'); /* Borderless, 1080p */
    expect("cursor marks", cursor_marks == 2u && layout_walked == 12u && cursor_visible == 1u);
    const uint32_t category_layout_page = 19u;
    expect("labels only while drawing", hook(0x000910a0u, (uint32_t[2]){19u, 0u}, 2u) == 0x9900u &&
        hook(0x00090ad0u, &category_layout_page, 1u) == layout);

    /* Down to Graphics shows its four settings; right goes into them. */
    nav_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    nav_delta = 0;
    label_calls = 0u;
    hook(draw, &context, 1u);
    expect("graphics", rd16(CURSORS + 2u) == 1u && label_cursors[0] == 1u && label_list_counts[1] == 4u &&
        ascii(label_lists[1][0]) == 'M' && changed_setting == 99u);
    step_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    step_delta = 0;
    label_calls = 0u; cursor_marks = 0u;
    hook(draw, &context, 1u);
    expect("into the settings", rd16(CURSORS + 2u) == 0u && label_cursors[0] == 1u && mark_cursors[0] == 0xffffu &&
        label_cursors[1] == 0u && mark_cursors[1] == 0u && changed_setting == 99u);
    const uint32_t msaa_help[3] = {19u, 0u, 0u}, sharpen_help[3] = {19u, 2u, 0u};
    hook(0x00091560u, msaa_help, 3u);
    expect("live help", ascii(help_text) == 'T' && rd16(help_text + 26u) == 'i'); /* "Takes effect immediately." */
    wr16(CURSORS + 2u, 2u); step_delta = 1; /* the Sharpening row */
    hook(0x0011cfc0u, none, 2u);
    step_delta = 0;
    expect("sharpen change", changed_setting == RECOMP_PC_SHARPEN && changed_choice == 1u &&
        recomp_pc_settings_choice(RECOMP_PC_SHARPEN) == 1u);
    hook(0x00091560u, sharpen_help, 3u);
    expect("sharpen help", rd16(help_text + 26u) == 'i');
    hook(draw, &context, 1u);
    expect("values", value_count == 4u && ascii(values[2]) == 'L'); /* Low, stepped above */
    /* Back returns to the categories, on Graphics; confirm goes in again. */
    back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    back_pressed = 0u;
    expect("back to categories", rd16(CURSORS + 2u) == 1u && closed == 0u && rd16(DEPTH) == 1u);
    nav_delta = 1;
    hook(0x0011cfc0u, none, 2u); /* Audio */
    hook(0x0011cfc0u, none, 2u); /* Gameplay */
    nav_delta = 0;
    confirm_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    confirm_pressed = 0u;
    const uint32_t free_help[3] = {19u, 2u, 0u};
    hook(0x00091560u, free_help, 3u);
    expect("online run next start", rd16(CURSORS + 2u) == 0u && rd16(help_text + 26u) == 't'); /* Free Camera, off as online */
    recomp_pc_settings_set_after_online(RECOMP_PC_FREE_CAMERA, true); /* as Live logged on */
    hook(0x00091560u, free_help, 3u);
    expect("leave online help", rd16(help_text + 26u) == 'w'); /* "Takes effect when you leave ..." */
    recomp_pc_settings_set_after_online(RECOMP_PC_FREE_CAMERA, false);
    back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    back_pressed = 0u;

    /* German: categories, settings and the last setting's values. */
    language = 3u;
    nav_delta = -3; /* up to Display */
    hook(0x0011cfc0u, none, 2u);
    nav_delta = 0;
    label_calls = 0u;
    hook(draw, &context, 1u);
    expect("german", ascii(label_lists[0][0]) == 'A' && ascii(label_lists[1][0]) == 'A' && rd16(label_lists[1][1] + 8u) == 0xf6u &&
        ascii(values[0]) == 'R'); /* Anzeige, Anzeigemodus, Aufl\u00f6sung, Rahmenlos */
    language = 1u;
    /* HUD holds the last rows. */
    nav_delta = -1; /* wraps to HUD */
    hook(0x0011cfc0u, none, 2u);
    nav_delta = 0;
    label_calls = 0u;
    hook(draw, &context, 1u);
    expect("hud", rd16(CURSORS + 2u) == 4u && label_list_counts[1] == 3u && ascii(label_lists[1][0]) == 'L' &&
        ascii(label_lists[1][1]) == 'F' && ascii(label_lists[1][2]) == 'H' && ascii(values[1]) == 'D' &&
        ascii(values[2]) == 'O' && layout_walked == 6u); /* Life Bar, Fight HUD Damage, Hitboxes Off */
    expect("leave draw", rd32(0x0067afa0u + 4u * 19u) != draw);
    hook(rd32(0x0067af50u + 4u * 19u), &context, 1u);
    expect("enter resets", rd32(layout + 0xcu) == 0u && rd32(layout + 6u * 28u) == 0u);

    /* Back on the categories leaves Options for the main menu and saves. */
    const unsigned pc_left_before = left_options;
    back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    expect("pc close", closed == 1u && left_options == pc_left_before + 1u && rd16(DEPTH) == 0u && rd16(PAGE) == 0u);
    /* While Options fades out the top list neither draws nor takes input. */
    hook(0x0011cfc0u, none, 2u);
    back_pressed = 0u;
    expect("no options list while leaving", left_options == pc_left_before + 1u && original_update == 1u);
    hook(0x0011afd0u, none, 0u);
    expect("options init clears page 19", options_inits == 1u);

    /* The Controller page (4): Rebind is row 3, before Default. */
    language = 1u;
    page = 4u;
    expect("controller rows", hook(0x0011ac90u, &page, 1u) == 5u && hook(0x0011ad10u, &page, 1u) == 5u);
    const uint32_t controller = hook(0x00090ad0u, &page, 1u);
    expect("controller layout", controller != 0x0067adf0u &&
        rd32(controller + 3u * 28u) == (0xabu << 16 | 0x10au) && rd32(controller + 4u * 28u) == (0xc5u << 16 | 0x10au) &&
        rd32(controller + 4u * 28u + 4u) == (0xc5u << 16 | 0x10au) && rd32(controller + 5u * 28u) == (0x77u << 16 | 0x1e0u) &&
        rd32(controller + 6u * 28u + 0x14u) == 0x40001u && rd32(controller + 7u * 28u) == 0u);
    /* The page's draw passes the game's layout to the cursor by address. */
    const uint32_t mark[6] = {context, 0x0067adf0u, 6u, 3u, 5u, 1u};
    hook(0x00090f00u, mark, 6u);
    expect("controller cursor", layout_walked == 7u && rd32(controller + 6u * 28u + 0xcu) == 1u);
    hook(0x00091ca0u, NULL, 0u);
    expect("controller reset", rd32(controller + 0xcu) == 0u && rd32(controller + 6u * 28u + 0xcu) == 0u);
    const uint32_t controller_labels[4] = {4u, 3u, 0x3f800000u, 0u};
    hook(0x00091440u, controller_labels, 4u);
    expect("controller labels", label_count == 5u && labels[2] == 0x9902u && ascii(labels[3]) == 'R' &&
        rd16(labels[3] + 2u) == 'e' && labels[4] == 0x9903u);
    const uint32_t default_key[2] = {4u, 3u};
    expect("controller labels only while drawing", hook(0x000910a0u, default_key, 2u) == 0x9903u);
    const uint32_t rebind_help[3] = {4u, 3u, 0u}, default_help[3] = {4u, 4u, 0u}, vibration_help[3] = {4u, 1u, 0u};
    hook(0x00091560u, rebind_help, 3u);
    expect("rebind help", help_page == 0u && help_cursor == 0u && ascii(help_text) == 'C'); /* "Change ..." */
    hook(0x00091560u, default_help, 3u);
    expect("default help", help_page == 4u && help_cursor == 3u && help_text == 0x1234u);
    hook(0x00091560u, vibration_help, 3u);
    expect("vibration help", help_cursor == 1u && help_text == 0x1234u);
    /* A on Rebind opens Button Config on its device list; the game's handler keeps the other rows. */
    const unsigned updates = original_update;
    wr16(DEPTH, 1u); wr16(PAGE, 4u); wr16(CURSORS + 2u, 3u); confirm_pressed = 1u;
    expect("rebind opens", hook(0x0011cfc0u, none, 2u) == 1u && scene == 0x19u && rd16(DEPTH) == 3u &&
        rd16(PAGE) == 0x10u && rd16(CURSORS + 6u) == 0u && original_update == updates);
    scene = 0u;
    wr16(DEPTH, 1u); wr16(PAGE, 4u);
    wr16(CURSORS + 2u, 2u);
    hook(0x0011cfc0u, none, 2u);
    wr16(CURSORS + 2u, 3u); confirm_pressed = 0u;
    hook(0x0011cfc0u, none, 2u);
    expect("game rows", scene == 0u && original_update == updates + 2u);

    /* Button Config's device list: Down picks Controller; the list shows both devices. */
    const uint32_t config = 0x00260000u;
    wr16(DEPTH, 3u); wr16(PAGE, 0x10u); nav_delta = 1;
    hook(0x0011d480u, &config, 1u);
    expect("device pick", config_updates == 0u && rd16(CURSORS + 6u) == 1u);
    nav_delta = 0; text_count = 0u;
    hook(0x00096720u, &config, 1u);
    expect("device list", pad_type_seen == 4u && list_sprites == 0u && text_count == 4u &&
        ascii(texts[0].text) == 'K' && ascii(texts[1].text) == 'C' && texts[1].y > texts[0].y);
    expect("plate names", ascii(texts[2].text) == 'L' && rd16(texts[2].text + 2u) == 'B' && ascii(texts[3].text) == 'W' &&
        config_sprites == 2u && config_lines == 1u);
    /* A opens the plates; A on a plate listens on the chosen device. */
    confirm_pressed = 1u;
    hook(0x0011d480u, &config, 1u);
    expect("plates", rd16(DEPTH) == 4u && rd16(PAGE) == 0x11u && rd16(CURSORS + 8u) == 0u);
    hook(0x0011d480u, &config, 1u);
    expect("listen", rebinds == 1u && listened == RECOMP_INPUT_BIND_A && listened_pad == 1u);
    listening_now = true; confirm_pressed = 0u; nav_delta = 1; text_count = 0u;
    hook(0x0011d480u, &config, 1u);
    hook(0x00096720u, &config, 1u);
    expect("listening", rd16(CURSORS + 8u) == 0u && ascii(texts[2].text) == 'P'); /* "Press a button" */
    listening_now = false; nav_delta = 0;
    /* Right turns to the d-pad set: labels for icons, no lines, plates 6 and 7 hidden and skipped. */
    step_delta = 1;
    hook(0x0011d480u, &config, 1u);
    step_delta = 0; text_count = 0u; config_sprites = config_lines = 0u;
    hook(0x00096720u, &config, 1u);
    expect("second set", config_sprites == 0u && config_lines == 0u && text_count == 4u &&
        ascii(texts[2].text) == 'W' && ascii(texts[3].text) == 'U' && texts[3].x == 500u);
    wr16(CURSORS + 8u, 5u); nav_delta = 1;
    hook(0x0011d480u, &config, 1u);
    expect("skip missing plates", rd16(CURSORS + 8u) == 8u);
    nav_delta = 0; confirm_pressed = 1u;
    hook(0x0011d480u, &config, 1u);
    expect("default", reset_pad == 1u && rebinds == 1u);
    /* B steps back to the devices, then out of the scene; leaving saves once. */
    confirm_pressed = 0u; back_pressed = 1u;
    const unsigned leaves = scene_leaves;
    hook(0x0011d480u, &config, 1u);
    expect("plates back", rd16(DEPTH) == 3u && rd16(PAGE) == 0x10u && scene_leaves == leaves);
    hook(0x0011d480u, &config, 1u);
    back_pressed = 0u;
    expect("devices back", scene_leaves == leaves + 1u);
    hook(0x0011b260u, NULL, 0u);
    hook(0x0011d480u, &config, 1u);
    expect("leave saves", rebind_saves == 1u && config_leaves == 1u && config_updates == 1u);

    expect("unhooked", recomp_pc_settings_lookup_manual(0x0011c590u) == NULL);

    /* The Jukebox: the main menu asks, the next update opens page 19 as it. */
    language = 1u;
    wr16(DEPTH, 0u); wr16(PAGE, 0u);
    recomp_jukebox_request_page();
    hook(0x0011cfc0u, none, 2u);
    expect("jukebox opens", jb_opened == 1u && rd16(DEPTH) == 1u && rd16(PAGE) == 19u && rd16(CURSORS + 2u) == 0u);
    const uint32_t page19 = 19u;
    expect("jukebox rows", hook(0x0011ac90u, &page19, 1u) == RECOMP_JUKEBOX_ROWS);
    hook(draw, &context, 1u);
    expect("jukebox labels", label_count == RECOMP_JUKEBOX_ROWS && ascii(labels[0]) == 'A' && ascii(labels[3]) == 'A' &&
        rd16(labels[2] + 2u) == 'L');
    expect("jukebox values", value_count == RECOMP_JUKEBOX_ROWS && ascii(values[1]) == 'A' && rd16(values[1] + 10u) == 0u &&
        ascii(values[0]) == 'D');
    jb_track = "Caf\xc3\xa9 \xe6\x97\xa5\xf0\x9f\x8e\xb5";
    hook(draw, &context, 1u);
    expect("UTF-8 names become UTF-16", rd16(values[1] + 6u) == 0xe9u &&
        rd16(values[1] + 10u) == 0x65e5u && rd16(values[1] + 12u) == 0xd83cu && rd16(values[1] + 14u) == 0xdfb5u);
    jb_track = "bad\xc0x";
    hook(draw, &context, 1u);
    expect("invalid UTF-8 is replaced", rd16(values[1] + 6u) == '?' && rd16(values[1] + 8u) == 'x');
    jb_track = "Ayane";
    /* The top list stays hidden beside it. */
    drawn_count = 0u;
    hook(0x00095a70u, &context, 1u);
    expect("no top list", drawn_count == 0u);
    /* Down to Track, right steps it, A plays. */
    nav_delta = 1; step_delta = 1;
    hook(0x0011cfc0u, none, 2u);
    nav_delta = 0; step_delta = 0;
    expect("track stepped", rd16(CURSORS + 2u) == 1u && jb_choice[1] == 1u);
    confirm_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    confirm_pressed = 0u;
    expect("track confirmed", jb_confirmed == 1u);
    const uint32_t help_args[3] = {19u, 1u, 0u};
    hook(0x00091560u, help_args, 3u);
    expect("jukebox help", help_page == 0u && ascii(help_text) == 'H');
    /* Back leaves Options for the main menu, as the top list's back does. */
    const unsigned left_before = left_options;
    back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    back_pressed = 0u;
    expect("jukebox back", jb_closed == 1u && left_options == left_before + 1u && rd16(DEPTH) == 0u && rd16(PAGE) == 0u);
    /* While Options fades out the top list neither draws nor takes input. */
    const unsigned leaving = left_options;
    back_pressed = 1u;
    hook(0x0011cfc0u, none, 2u);
    back_pressed = 0u;
    expect("no options list while leaving", left_options == leaving);
    hook(0x0011afd0u, none, 0u);
    expect("options init clears jukebox", options_inits == 2u);
    expect("pc page again", hook(0x0011ac90u, &page19, 1u) == 3u); /* HUD, the category it left on */
    return fails == 0 ? 0 : 1;
}

