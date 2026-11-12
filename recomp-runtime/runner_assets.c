/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "runner_assets.h"
#include "d3d8_adapter.h"
#include "d3d_texture_model.h"
#include "kernel_abi.h"
#include "stop_report.h"
#include "xbox_memory_layout.h"

#include <string.h>

extern void sub_0006A3E0(void);
extern void sub_000212E0(void);
extern void sub_000AA970(void);
extern void sub_000AA9B0(void);

enum {
    MESSAGE = 0x0006a3e0u,
    SPRITE_DEF = 0x000aa970u,
    SPRITE_TEXTURE = 0x000aa9b0u,
    SPRITE_DEFS = 0x00686698u,     /* per group: 12-byte rectangles */
    SPRITE_TEXTURES = 0x0067ce78u, /* per group: texture, palette */
    TEXTS = 128,
};

static uint32_t cursor = XBOX_RUNNER_ASSET_BASE;
/* Each slot's rectangle, followed by its texture entry and texture header. */
static struct { uint32_t rectangle, pixels, width, height; } sprites[RECOMP_RUNNER_SPRITE_SLOTS];
static struct { uint32_t number, text[6]; } texts[TEXTS];
static unsigned text_count;
static uint32_t redirect_from, redirect_to;

void recomp_runner_assets_reset(void)
{
    for (uint32_t offset = 0u; offset < XBOX_RUNNER_ASSET_SIZE; offset += 0x1000u)
        memset(recomp_memory(XBOX_RUNNER_ASSET_BASE + offset, 0x1000u), 0, 0x1000u);
    cursor = XBOX_RUNNER_ASSET_BASE;
    memset(sprites, 0, sizeof sprites);
    memset(texts, 0, sizeof texts);
    text_count = 0u;
    redirect_from = redirect_to = 0u;
}

uint32_t recomp_runner_alloc(uint32_t size, uint32_t alignment)
{
    const uint32_t start = (cursor + alignment - 1u) & ~(alignment - 1u);
    if (start > XBOX_RUNNER_ASSET_BASE + XBOX_RUNNER_ASSET_SIZE - size) {
        recomp_stop(2, "runner-assets:full");
        return 0u;
    }
    cursor = start + size;
    return start;
}

/* ---- Sprites ---------------------------------------------------------- */

uint32_t recomp_runner_palette(uint32_t *colours)
{
    const uint32_t header = recomp_runner_alloc(12u, 4u);
    *colours = recomp_runner_alloc(1024u, 0x80u);
    *recomp_memory_u32(header) = 0x00830001u;
    *recomp_memory_u32(header + 4u) = *colours;
    return header;
}

void recomp_runner_sprite(unsigned slot, unsigned log_width, unsigned log_height, uint32_t palette)
{
    if (slot >= RECOMP_RUNNER_SPRITE_SLOTS) return;
    const uint32_t rectangle = recomp_runner_alloc(12u + 8u + 20u, 4u), entry = rectangle + 12u,
                   texture = entry + 8u;
    const uint32_t width = 1u << log_width, height = 1u << log_height;
    const uint32_t pixels = recomp_runner_alloc(width * height, 0x1000u);
    *recomp_memory_u16(rectangle + 8u) = (uint16_t)slot;
    *recomp_memory_u16(rectangle + 10u) = RECOMP_RUNNER_SPRITE_GROUP;
    *recomp_memory_u32(entry) = texture;
    *recomp_memory_u32(entry + 4u) = palette;
    *recomp_memory_u32(texture) = 0x00040001u;
    *recomp_memory_u32(texture + 4u) = pixels;
    *recomp_memory_u32(texture + 12u) = log_height << 24 | log_width << 20 | 1u << 16 | 0x0bu << 8 | 0x2au;
    sprites[slot].rectangle = rectangle;
    sprites[slot].pixels = pixels;
    sprites[slot].width = width;
    sprites[slot].height = height;
}

void recomp_runner_sprite_size(unsigned slot, int width, int height)
{
    if (slot >= RECOMP_RUNNER_SPRITE_SLOTS || sprites[slot].rectangle == 0u) return;
    *recomp_memory_u16(sprites[slot].rectangle + 4u) = (uint16_t)width;
    *recomp_memory_u16(sprites[slot].rectangle + 6u) = (uint16_t)height;
}

void recomp_runner_sprite_palette(unsigned slot, uint32_t palette)
{
    if (slot >= RECOMP_RUNNER_SPRITE_SLOTS || sprites[slot].rectangle == 0u) return;
    *recomp_memory_u32(sprites[slot].rectangle + 16u) = palette;
}

void recomp_runner_sprite_pixels(unsigned slot, const uint8_t *pixels)
{
    if (slot >= RECOMP_RUNNER_SPRITE_SLOTS || sprites[slot].rectangle == 0u) return;
    const uint32_t width = sprites[slot].width, height = sprites[slot].height;
    uint32_t mask_x, mask_y;
    recomp_d3d_texture_swizzle_masks(width, height, &mask_x, &mask_y);
    for (uint32_t y = 0u; y < height; ++y)
        for (uint32_t x = 0u; x < width; ++x)
            *recomp_memory(sprites[slot].pixels + recomp_d3d_texture_swizzle_offset(x, y, mask_x, mask_y), 1u) =
                pixels[y * width + x];
    recomp_d3d8_invalidate_textures(sprites[slot].pixels, width * height);
}

bool recomp_game_sprite(uint32_t id, int height, RecompGameSprite *sprite)
{
    const uint32_t group = id >> 16;
    const uint32_t defs = *recomp_memory_u32(SPRITE_DEFS + 4u * group);
    const uint32_t textures = *recomp_memory_u32(SPRITE_TEXTURES + 4u * group);
    if (defs == 0u || textures == 0u) return false;
    const uint32_t def = defs + 12u * (id & 0xffffu);
    int16_t rect[4];
    memcpy(rect, recomp_memory(def, 8u), 8u);
    const uint32_t entry = textures + 8u * *recomp_memory_u16(def + 8u);
    const uint32_t texture = *recomp_memory_u32(entry), palette = *recomp_memory_u32(entry + 4u);
    if (texture == 0u || palette == 0u) return false;
    const uint32_t format = *recomp_memory_u32(texture + 12u);
    if (((format >> 8) & 0xffu) != 0x0bu) return false;
    const uint32_t width = 1u << ((format >> 20) & 0xfu), texture_height = 1u << ((format >> 24) & 0xfu);
    *sprite = (RecompGameSprite){*recomp_memory_u32(texture + 4u) & 0x03ffffffu, palette,
        *recomp_memory_u32(palette + 4u) & 0x03ffffffu, 0u, 0u, rect[0], rect[1], rect[2], rect[3]};
    recomp_d3d_texture_swizzle_masks(width, texture_height, &sprite->mask_x, &sprite->mask_y);
    return sprite->right > sprite->left && sprite->left >= 0 && sprite->top >= 0 &&
        sprite->bottom - sprite->top == height &&
        (uint32_t)sprite->right <= width && (uint32_t)sprite->bottom <= texture_height;
}

uint8_t recomp_game_sprite_index(const RecompGameSprite *sprite, int x, int y)
{
    return *recomp_memory(sprite->pixels + recomp_d3d_texture_swizzle_offset((uint32_t)(sprite->left + x),
        (uint32_t)(sprite->top + y), sprite->mask_x, sprite->mask_y), 1u);
}

/* ---- Text ------------------------------------------------------------- */

void recomp_runner_text_set(uint32_t number, const recomp_utf16 *const text[6])
{
    unsigned i = 0u;
    while (i < text_count && texts[i].number != number) ++i;
    if (i == TEXTS) {
        recomp_stop(2, "runner-assets:texts");
        return;
    }
    if (i == text_count) ++text_count;
    texts[i].number = number;
    for (unsigned l = 0u; l < 6u; ++l) {
        unsigned same = 0u;
        while (same < l && text[same] != text[l]) ++same;
        if (same < l) {
            texts[i].text[l] = texts[i].text[same];
            continue;
        }
        size_t n = 0u;
        while (text[l][n] != 0u) ++n;
        const uint32_t address = recomp_runner_alloc((uint32_t)(n + 1u) * 2u, 4u);
        for (size_t c = 0u; c <= n; ++c) *recomp_memory_u16(address + 2u * (uint32_t)c) = (uint16_t)text[l][c];
        texts[i].text[l] = address;
    }
}

bool recomp_runner_text_lookup(uint32_t number, unsigned language, uint32_t *text)
{
    for (unsigned i = 0u; i < text_count; ++i) {
        if (texts[i].number != number) continue;
        *text = texts[i].text[language < 6u ? language : 1u];
        return *text != 0u;
    }
    return false;
}

void recomp_runner_text_redirect(uint32_t from, uint32_t to)
{
    redirect_from = from;
    redirect_to = to;
}

/* 0x6a3e0 (message, language or 0xff for the game's) */
static void message(void)
{
    uint32_t number = kernel_arg(1u) & 0xffffu;
    if (redirect_to != 0u && number == redirect_from) number = redirect_to;
    const uint32_t requested = kernel_arg(2u) & 0xffu;
    uint32_t text;
    if (number >= RECOMP_RUNNER_TEXT_PC && recomp_runner_text_lookup(number,
            requested == 0xffu ? kernel_call_lifted(sub_000212E0, NULL, 0u) & 0xffu : requested, &text)) {
        kernel_return_caller_cleanup(text);
        return;
    }
    sub_0006A3E0();
}

/* ---- Sprite lookups ----------------------------------------------------- */

/* A slot nobody set answers 0 rather than the game's tables, which have no
   such group: the caller would read past them. */
static void sprite_def(void)
{
    const uint32_t id = kernel_arg(1u);
    if ((id >> 16) != RECOMP_RUNNER_SPRITE_GROUP) {
        sub_000AA970();
        return;
    }
    const unsigned slot = id & 0xffffu;
    kernel_return_caller_cleanup(slot < RECOMP_RUNNER_SPRITE_SLOTS ? sprites[slot].rectangle : 0u);
}

static void sprite_texture(void)
{
    const uint32_t key = kernel_arg(1u);
    if ((key >> 16) != RECOMP_RUNNER_SPRITE_GROUP) {
        sub_000AA9B0();
        return;
    }
    const unsigned slot = key & 0xffffu;
    const uint32_t rectangle = slot < RECOMP_RUNNER_SPRITE_SLOTS ? sprites[slot].rectangle : 0u;
    kernel_return_caller_cleanup(rectangle != 0u ? rectangle + 12u : 0u);
}

RecompFunction recomp_runner_assets_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case MESSAGE: return message;
    case SPRITE_DEF: return sprite_def;
    case SPRITE_TEXTURE: return sprite_texture;
    default: return NULL;
    }
}
