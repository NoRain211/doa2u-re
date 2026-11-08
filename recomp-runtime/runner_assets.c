/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "runner_assets.h"

#include <string.h>

extern void sub_000AA970(void);
extern void sub_000AA9B0(void);

enum { SPRITE_DEF = 0x000aa970u, SPRITE_TEXTURE = 0x000aa9b0u, TEXTS = 8 };

static struct { uint32_t rectangle, entry; } sprites[RECOMP_RUNNER_SPRITE_SLOTS];
static struct { uint32_t number, text[6]; } texts[TEXTS];
static unsigned text_count;

void recomp_runner_assets_reset(void)
{
    memset(sprites, 0, sizeof sprites);
    memset(texts, 0, sizeof texts);
    text_count = 0u;
}

void recomp_runner_sprite_set(unsigned slot, uint32_t rectangle, uint32_t entry)
{
    if (slot >= RECOMP_RUNNER_SPRITE_SLOTS) return;
    sprites[slot].rectangle = rectangle;
    sprites[slot].entry = entry;
}

void recomp_runner_text_set(uint32_t number, const uint32_t text[6])
{
    unsigned i = 0u;
    while (i < text_count && texts[i].number != number) ++i;
    if (i == TEXTS) return;
    if (i == text_count) ++text_count;
    texts[i].number = number;
    memcpy(texts[i].text, text, sizeof texts[i].text);
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

static uint32_t arg1(void) { return *recomp_memory_u32(recomp_runtime.registers.esp + 4u); }

static void finish(uint32_t eax)
{
    recomp_runtime.registers.eax = eax;
    recomp_runtime.registers.esp += 4u;
}

/* A slot nobody set answers 0 rather than the game's tables, which have no
   such group: the caller would read past them. */
static void sprite_def(void)
{
    const uint32_t id = arg1();
    if ((id >> 16) != RECOMP_RUNNER_SPRITE_GROUP) {
        sub_000AA970();
        return;
    }
    const unsigned slot = id & 0xffffu;
    finish(slot < RECOMP_RUNNER_SPRITE_SLOTS ? sprites[slot].rectangle : 0u);
}

static void sprite_texture(void)
{
    const uint32_t key = arg1();
    if ((key >> 16) != RECOMP_RUNNER_SPRITE_GROUP) {
        sub_000AA9B0();
        return;
    }
    const unsigned slot = key & 0xffffu;
    finish(slot < RECOMP_RUNNER_SPRITE_SLOTS ? sprites[slot].entry : 0u);
}

RecompFunction recomp_runner_assets_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case SPRITE_DEF: return sprite_def;
    case SPRITE_TEXTURE: return sprite_texture;
    default: return NULL;
    }
}
