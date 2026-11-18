/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "menu_quit.h"
#include "quit_icon.h"
#include "quit_prompt.h"
#include "runner_assets.h"
#include <stdarg.h>
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
    STACK = 0x00100f00u, MENU = 0x00df5698u, LIST = 0x00df5d18u,
    COUNT = 0x006e2260u, SELECTED = 0x00df57b4u,
    /* In the confirm's availability table (modes 0-0x18, all 2 but mode 5), used by no entry. */
    QUIT_MODE = 0x13u,
};

static int failures;
static void expect(const char *what, int ok)
{
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static uint32_t hook(uint32_t address, const uint32_t *args, unsigned count)
{
    RecompFunction function = recomp_menu_quit_lookup_manual(address);
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

/* The game, faked. */
static unsigned language = 1u, unlocked = 16u, invalidated;
static uint32_t sounds[16], sound_count;
static bool confirm_this_frame;
void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size) { (void)base; (void)size; ++invalidated; }
void recomp_stop(int code, const char *format, ...) { fprintf(stderr, "stop %d: %s\n", code, format); abort(); }
void sub_000212E0(void) { ret(language); }
void sub_0006A3E0(void) { ret(0x1234u); }
void sub_00134330(void)
{
    for (unsigned i = 0; i < unlocked; ++i) *recomp_memory(LIST + i, 1u) = (uint8_t)i;
    *recomp_memory(COUNT, 1u) = (uint8_t)unlocked;
    ret(0u);
}
void sub_001343A0(void) { ret(0x7777u); }
void sub_000AA970(void) { ret(0x5555u); }
void sub_000AA9B0(void) { ret(0x6666u); }
void sub_001343D0(void) { ret(5u); }
void sub_001345C0(void) { ret(0x95u); }
static unsigned cancels;
void sub_00135AE0(void) { ++cancels; ret(1u); }
void sub_000FED70(void) { if (sound_count < 16u) sounds[sound_count++] = arg(1u); ret(0u); }
/* The menu's update: the confirm plays sound 0x24 through the hooked call. */
void sub_0013B850(void)
{
    if (confirm_this_frame) {
        const uint32_t id = 0x24u;
        const uint32_t saved = recomp_runtime.registers.esp;
        hook(0x000fed70u, &id, 1u);
        recomp_runtime.registers.esp = saved;
    }
    ret(1u);
}

/* The main menu's label art: group 7's words, 200x24, all one P8 texture whose
   palette entry 1 is bright opaque and 2 dark shadow. */
static uint32_t swizzle(uint32_t x, uint32_t y)
{
    uint32_t index = 0u, bit = 0u, w = 256u, h = 256u;
    while (w > 1u || h > 1u) {
        if (w > 1u) { index |= (x & 1u) << bit++; x >>= 1; w >>= 1; }
        if (h > 1u) { index |= (y & 1u) << bit++; y >>= 1; h >>= 1; }
    }
    return index;
}

static void make_art(void)
{
    const uint32_t defs = 0x00200000u, entries = 0x00210000u, texture = 0x00220000u, palette = 0x00221000u,
                   pixels = 0x00230000u, colours = 0x00240000u;
    wr32(0x00686698u + 4u * 7u, defs);
    wr32(0x0067ce78u + 4u * 7u, entries);
    for (unsigned i = 0; i < 0x20u; ++i) {
        const int16_t rect[4] = {0, (int16_t)(24 * (i % 10u)), 200, (int16_t)(24 * (i % 10u) + 24)};
        memcpy(recomp_memory(defs + 12u * i, 8u), rect, 8u);
        wr16(defs + 12u * i + 8u, 2u);
        wr16(defs + 12u * i + 10u, 7u);
    }
    wr32(entries + 8u * 2u, texture);
    wr32(entries + 8u * 2u + 4u, palette);
    wr32(texture + 4u, pixels);
    wr32(texture + 12u, 8u << 24 | 8u << 20 | 1u << 16 | 0x0bu << 8 | 0x2au);
    wr32(palette + 4u, colours);
    wr32(colours + 4u, 0xffe0e0e0u);
    wr32(colours + 8u, 0xc0202020u);
    for (unsigned a = 1u; a <= 8u; ++a) wr32(colours + 4u * (8u + a), (a * 16u) << 24 | 0x231f1fu); /* shadow */
    wr32(colours + 4u * 20u, 0xffa8a8a8u); /* the grey of a word's left end */
    /* Each word: letters in rows 2-15, its own shadow below. */
    for (uint32_t y = 0; y < 256u; ++y)
        for (uint32_t x = 0; x < 256u; ++x)
            *recomp_memory(pixels + swizzle(x, y), 1u) = (uint8_t)(y % 24u >= 2u && y % 24u < 16u ? 1u : y % 24u >= 16u ? 12u : 0u);
}

static void frame(void)
{
    const uint32_t ctx = MENU;
    hook(0x0013b850u, &ctx, 1u);
}

static RecompInputGamepad pad_with(uint16_t buttons, uint8_t a, uint8_t b)
{
    RecompInputGamepad pad;
    memset(&pad, 0, sizeof pad);
    pad.buttons = buttons;
    pad.analog_buttons[0] = a;
    pad.analog_buttons[1] = b;
    return pad;
}

static unsigned jukebox_opens;
static void fake_jukebox_request(void) { ++jukebox_opens; }

static bool idle(RecompInputGamepad pad) { return pad.buttons == 0u && pad.analog_buttons[0] == 0u && pad.analog_buttons[1] == 0u; }

int main(void)
{
    recomp_runtime.registers.esp = STACK;

    /* The picture: the door is opaque and bright, the corners clear. */
    static uint8_t grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE], alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE];
    recomp_quit_icon_draw(1.0f, grey, alpha);
    expect("picture door", alpha[40 * 128 + 38] == 255u && grey[40 * 128 + 38] > 150u);
    expect("picture clear corner", alpha[0] == 0u && alpha[127 * 128 + 127] < 40u);
    static uint8_t wide_grey[QUIT_ICON_SIZE * QUIT_ICON_SIZE], wide_alpha[QUIT_ICON_SIZE * QUIT_ICON_SIZE];
    recomp_quit_icon_draw(0.75f, wide_grey, wide_alpha);
    unsigned plain = 0u, narrow = 0u;
    for (unsigned i = 0; i < 128u * 128u; ++i) { plain += alpha[i] > 128u; narrow += wide_alpha[i] > 128u; }
    expect("wide picture narrower", narrow * 10u < plain * 8u && narrow * 10u > plain * 6u);

    recomp_runner_assets_reset();
    recomp_menu_quit_install();
    const unsigned installed = invalidated;
    expect("pictures drawn", installed == 2u);
    const uint32_t picture = 0x1f0u << 16 | 3u;
    const uint32_t rect = hook(0x000aa970u, &picture, 1u), entry = hook(0x000aa9b0u, &picture, 1u);
    expect("picture sprite", rd16(rect + 4u) == 128u && rd16(rect + 6u) == 128u && rd16(rect + 8u) == 3u &&
        rd16(rect + 10u) == 0x1f0u);
    const uint32_t texture = rd32(entry), palette = rd32(entry + 4u);
    expect("picture texture", rd32(texture + 12u) == 0x07710b2au &&
        rd32(rd32(palette + 4u) + 4u * 255u) == 0xffffffffu && rd32(rd32(palette + 4u)) == 0u);
    expect("picture pixels", *recomp_memory(rd32(texture + 4u), 1u) == 0u);

    /* All 16 unlocked: Quit Game is position 16, past the full list. */
    *recomp_memory(LIST + 16u, 1u) = 0xabu; /* the carousel object's first byte */
    const uint32_t none = 0u;
    hook(0x00134330u, &none, 1u);
    expect("count", *recomp_memory(COUNT, 1u) == 17u && *recomp_memory(LIST + 16u, 1u) == 0xabu);
    uint32_t position = 16u;
    const uint32_t quit_entry = hook(0x001343a0u, &position, 1u);
    expect("label waits for the art", quit_entry != 0x7777u && invalidated == installed);
    /* The confirm only passes modes whose 0xdf5d70 slot is 2: modes 0-0x18 but 5.
       It must also be one the switch ignores (7, 0xc-0x10, 0x13, 0x14). */
    const uint32_t quit_mode = rd32(quit_entry);
    expect("selectable mode", quit_mode < 0x19u && quit_mode != 5u &&
        (quit_mode == 7u || (quit_mode >= 0xcu && quit_mode <= 0x10u) || quit_mode == 0x13u || quit_mode == 0x14u));
    expect("entry", quit_mode == QUIT_MODE && rd32(quit_entry + 4u) == (0x1f0u << 16 | 2u) &&
        rd32(quit_entry + 8u) == (0x1f0u << 16 | 3u) && rd32(quit_entry + 12u) == (0x1f0u << 16 | 4u));
    make_art();
    expect("quit entry", hook(0x001343a0u, &position, 1u) == quit_entry && invalidated == installed + 1u);
    /* 6 lean + Q 13+1, U 11, I 6, T 12, space 9, G 12.5, A 12.5, M 14, E 11 = 108, then 8 for the shadow. */
    const uint32_t label = 0x1f0u << 16 | 2u;
    const uint32_t label_rect = hook(0x000aa970u, &label, 1u), label_entry = hook(0x000aa9b0u, &label, 1u);
    const uint32_t label_pixels = rd32(rd32(label_entry) + 4u);
    expect("label width", rd16(label_rect + 4u) == 116u && rd16(label_rect + 6u) == 24u);
    expect("label palette", rd32(label_entry + 4u) == 0x00221000u);
    expect("label pixels", *recomp_memory(label_pixels, 1u) == 0u);
    unsigned inked = 0u, cast = 0u, darkened = 0u;
    for (uint32_t i = 0; i < 128u * 32u; ++i) {
        const uint8_t index = *recomp_memory(label_pixels + i, 1u);
        inked += index == 1u || index == 20u;
        darkened += index == 20u;
        cast += index >= 9u && index <= 16u;
    }
    expect("label inked", inked > 1000u && inked < 116u * 24u);
    /* The Q, from ONLINE's bright end, takes the grey of the label's left end. */
    expect("label brightness ramp", darkened > 50u && darkened < inked / 2u);
    /* The words' own shadow stays behind; the label casts one (4 px down, soft). */
    uint32_t li = 0u, bit = 0u, lx = 90u, ly = 17u, lw = 128u, lh = 32u;
    while (lw > 1u || lh > 1u) {
        if (lw > 1u) { li |= (lx & 1u) << bit++; lx >>= 1; lw >>= 1; }
        if (lh > 1u) { li |= (ly & 1u) << bit++; ly >>= 1; lh >>= 1; }
    }
    const uint8_t probe = *recomp_memory(label_pixels + li, 1u); /* 0.436 of near-full cover, not the art's 64 */
    expect("label shadow", cast > 300u && (probe == 14u || probe == 15u));
    position = 3u;
    expect("game entries", hook(0x001343a0u, &position, 1u) == 0x7777u);
    *recomp_memory(MENU + 0x120u, 1u) = 16u;
    const uint32_t ctx = MENU;
    expect("quit mode", hook(0x001343d0u, &ctx, 1u) == QUIT_MODE);
    *recomp_memory(MENU + 0x120u, 1u) = 2u;
    expect("game modes", hook(0x001343d0u, &ctx, 1u) == 5u);
    const uint32_t mode = QUIT_MODE, other_mode = 3u;
    const uint32_t help = hook(0x001345c0u, &mode, 1u);
    uint32_t german = 0u;
    expect("help", help == RECOMP_RUNNER_TEXT_QUIT && hook(0x001345c0u, &other_mode, 1u) == 0x95u &&
        recomp_runner_text_lookup(help, 3u, &german) && rd16(german) == 'D');

    /* Fewer unlocked: Quit Game follows the listed ones. */
    unlocked = 12u;
    hook(0x00134330u, &none, 1u);
    position = 12u;
    expect("short list", *recomp_memory(COUNT, 1u) == 13u && hook(0x001343a0u, &position, 1u) == quit_entry);

    /* Confirming another entry does nothing; confirming Quit Game asks with
       the runner's quit prompt. */
    wr32(SELECTED, 3u);
    confirm_this_frame = true;
    frame();
    expect("other confirm", !recomp_quit_prompt_open() && sound_count == 1u && sounds[0] == 0x24u);
    wr32(SELECTED, QUIT_MODE);
    frame();
    confirm_this_frame = false;
    expect("asks", recomp_quit_prompt_open());
    RecompInputGamepad pad = pad_with(0u, 0xffu, 0u);
    expect("prompt hides the A that chose it", recomp_quit_prompt_filter(0u, &pad, 0u) == RECOMP_QUIT_OPENED &&
        idle(pad));
    frame();
    expect("menu waits while asked", cancels == 0u);
    pad = pad_with(0u, 0u, 0u);
    recomp_quit_prompt_filter(0u, &pad, 10u);
    pad = pad_with(0u, 0u, 0xffu);
    expect("B keeps playing", recomp_quit_prompt_filter(0u, &pad, 20u) == RECOMP_QUIT_CANCELLED && idle(pad));
    frame();
    expect("menu brought back", cancels == 1u && sounds[sound_count - 1u] == 0x29u);
    frame();
    expect("brought back once", cancels == 1u);

    /* Again, and A quits: the runner stops, and the menu is left alone. */
    pad = pad_with(0u, 0u, 0u);
    recomp_quit_prompt_filter(0u, &pad, 30u);
    confirm_this_frame = true;
    frame();
    confirm_this_frame = false;
    pad = pad_with(0u, 0u, 0u);
    expect("asks again", recomp_quit_prompt_filter(0u, &pad, 40u) == RECOMP_QUIT_OPENED);
    pad = pad_with(0u, 0xffu, 0u);
    expect("A quits", recomp_quit_prompt_filter(0u, &pad, 50u) == RECOMP_QUIT_CHOSEN);
    frame();
    expect("no cancel after quitting", cancels == 1u);

    /* The Jukebox: one more entry before Quit Game, with its own pictures and
       label; its confirm opens it, and its close brings the menu back. */
    recomp_runner_assets_reset();
    recomp_menu_quit_set_jukebox(fake_jukebox_request);
    const unsigned before_jukebox = invalidated;
    recomp_menu_quit_install();
    expect("jukebox pictures drawn", invalidated == before_jukebox + 4u);
    unlocked = 16u;
    hook(0x00134330u, &none, 1u);
    expect("count with the jukebox", *recomp_memory(COUNT, 1u) == 18u);
    position = 16u;
    const uint32_t jukebox_entry = hook(0x001343a0u, &position, 1u);
    expect("jukebox entry", rd32(jukebox_entry) == 0x0au && rd32(jukebox_entry + 4u) == (0x1f0u << 16 | 5u) &&
        rd32(jukebox_entry + 8u) == (0x1f0u << 16 | 6u) && rd32(jukebox_entry + 12u) == (0x1f0u << 16 | 7u));
    const uint32_t jukebox_label = 0x1f0u << 16 | 5u;
    const uint32_t jukebox_rect = hook(0x000aa970u, &jukebox_label, 1u);
    expect("jukebox label", rd16(jukebox_rect + 4u) > 70u && rd16(jukebox_rect + 4u) <= 128u && rd16(jukebox_rect + 6u) == 24u);
    position = 17u;
    expect("quit after the jukebox", rd32(hook(0x001343a0u, &position, 1u)) == QUIT_MODE);
    *recomp_memory(MENU + 0x120u, 1u) = 16u;
    expect("jukebox mode is Options'", hook(0x001343d0u, &ctx, 1u) == 0x0au);
    const uint32_t options_mode = 0x0au;
    expect("jukebox help", hook(0x001345c0u, &options_mode, 1u) == RECOMP_RUNNER_TEXT_JUKEBOX);
    const unsigned cancels_before = cancels;
    wr32(SELECTED, 0x0au);
    confirm_this_frame = true;
    frame();
    confirm_this_frame = false;
    expect("jukebox asked for", jukebox_opens == 1u && cancels == cancels_before);
    /* Options itself (another position, the same mode) asks for nothing. */
    *recomp_memory(MENU + 0x120u, 1u) = 9u;
    expect("options help", hook(0x001345c0u, &options_mode, 1u) == 0x95u);
    confirm_this_frame = true;
    frame();
    confirm_this_frame = false;
    expect("options alone", jukebox_opens == 1u);
    recomp_menu_quit_set_jukebox(NULL);

    if (failures == 0) printf("menu quit: all tests passed\n");
    return failures == 0 ? 0 : 1;
}

