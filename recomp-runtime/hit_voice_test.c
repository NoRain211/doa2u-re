/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hit_voice.h"

#include "audio_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static int failures;
static void expect(const char *name, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[0x100];
static uint8_t stack[0x40];
static uint8_t shown[4], ring[0x24]; /* 0x87ae2c; 0x8a7ee0.. (length +0xc, write +0x18, held +0x20) */
enum { BASE = 0x0087ae30u, STACK = 0x00100000u };
static uint32_t ram_u32_at_esp(uint32_t offset)
{
    const uint8_t *at = stack + (recomp_runtime.registers.esp + offset - STACK);
    return at[0] | at[1] << 8 | at[2] << 16 | (uint32_t)at[3] << 24;
}
uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= STACK && address + width <= STACK + sizeof stack) return stack + (address - STACK);
    if (address >= 0x0087ae2cu && address + width <= 0x0087ae30u) return shown + (address - 0x0087ae2cu);
    if (address >= 0x008a7ee0u && address + width <= 0x008a7f04u) return ring + (address - 0x008a7ee0u);
    if (address < BASE || address + width > BASE + sizeof ram) {
        fprintf(stderr, "unexpected read %08x\n", address);
        abort();
    }
    return ram + (address - BASE);
}

/* As 0x119420: each side's landed damage leaves its life and is cleared. */
void sub_00119420(void)
{
    for (unsigned side = 0; side < 2u; ++side) {
        uint8_t *block = ram + 0x24u + side * 0x94u;
        const unsigned life = block[0] | block[1] << 8, hit = block[2] | block[3] << 8;
        const unsigned left = hit >= life ? 0u : life - hit;
        block[0] = (uint8_t)left;
        block[1] = (uint8_t)(left >> 8);
        block[2] = block[3] = 0u;
    }
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

void recomp_ko_replay_note_ko(unsigned side) { (void)side; }

/* As 0x10ce60(side): the base damage of the hit the side takes, in al. */
static uint8_t test_base[2];
void sub_0010CE60(void)
{
    const uint32_t side = ram_u32_at_esp(4u);
    recomp_runtime.registers.eax = (recomp_runtime.registers.eax & ~0xffu) | (side < 2u ? test_base[side] : 0u);
}

static unsigned submits[2], resets[2], first_bytes;
static const uint8_t *last_pcm;
void recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes, uint32_t rate,
    uint32_t channels, uint32_t bits, int32_t volume)
{
    (void)volume;
    last_pcm = pcm;
    if (slot < RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT || slot > RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT + 1u ||
        rate != 48000u || channels != 1u || bits != 16u || bytes == 0u || bytes % 2u) abort();
    if (submits[slot - RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT]++ == 0u) first_bytes = bytes;
}
void recomp_audio_output_reset_voice(uint32_t slot) { ++resets[slot - RECOMP_AUDIO_OUTPUT_HIT_VOICE_SLOT]; }

static void put16(uint8_t *at, unsigned value) { at[0] = (uint8_t)value; at[1] = (uint8_t)(value >> 8); }
static void put32(uint8_t *at, uint32_t value) { put16(at, value & 0xffffu); put16(at + 2, value >> 16); }

/* A mono 48 kHz 16-bit WAV of `frames` silent frames. */
static void write_wav(const char *path, uint32_t frames)
{
    uint8_t header[44] = "RIFF....WAVEfmt ";
    put32(header + 4, 36u + frames * 2u);
    put32(header + 16, 16u);
    put16(header + 20, 1u);
    put16(header + 22, 1u);
    put32(header + 24, 48000u);
    put32(header + 28, 96000u);
    put16(header + 32, 2u);
    put16(header + 34, 16u);
    memcpy(header + 36, "data", 4);
    put32(header + 40, frames * 2u);
    FILE *file = fopen(path, "wb");
    if (file == NULL) abort();
    fwrite(header, 1, 44, file);
    for (uint32_t i = 0; i < frames; ++i) fputc(0, file), fputc(0, file);
    fclose(file);
}

static void land(unsigned side, unsigned life, unsigned damage)
{
    uint8_t *block = ram + 0x24u + side * 0x94u;
    put16(block, life);
    put16(block + 2, damage);
    recomp_hit_voice_lookup_manual(0x00119420u)();
}

static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* A hit: 0x10ce60 asked for the side's base damage, then 0x119420 lands `damage`. */
static void hit(unsigned side, unsigned life, unsigned base, unsigned damage)
{
    test_base[side] = (uint8_t)base;
    recomp_runtime.registers.esp = STACK + 0x10u;
    stack[0x14] = (uint8_t)side;
    stack[0x15] = stack[0x16] = stack[0x17] = 0u;
    recomp_hit_voice_lookup_manual(0x0010ce60u)();
    land(side, life, damage);
}

int main(void)
{
    /* The 15 fighters and their DOA4 banks; no others. */
    const unsigned ids[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 17, 21, 24};
    const char *names[15] = {"zac", "tin", "jan", "ein", "ryu", "kas", "gen", "hel", "bay", "bas",
        "lei", "aya", "tng", "kor", "leo"};
    for (unsigned i = 0; i < 15u; ++i)
        expect(names[i], recomp_hit_voice_prefix((uint8_t)ids[i]) != NULL &&
            strcmp(recomp_hit_voice_prefix((uint8_t)ids[i]), names[i]) == 0);
    expect("no clone", recomp_hit_voice_prefix(10u) == NULL && recomp_hit_voice_prefix(25u) == NULL &&
        recomp_hit_voice_prefix(255u) == NULL);

    RecompHitVoiceTuning tuning;
    recomp_hit_voice_default_tuning(&tuning);
    expect("levels", recomp_hit_voice_level(&tuning, 21u) == 1u && recomp_hit_voice_level(&tuning, 22u) == 2u &&
        recomp_hit_voice_level(&tuning, 34u) == 2u && recomp_hit_voice_level(&tuning, 35u) == 3u);
    expect("meter", recomp_hit_voice_should_grunt(&tuning, 40u, 35u, 1u, 1e9) &&
        !recomp_hit_voice_should_grunt(&tuning, 30u, 35u, 2u, 1e9));
    expect("heavy always", recomp_hit_voice_should_grunt(&tuning, 0u, 46u, 3u, 800.0));
    expect("gap", !recomp_hit_voice_should_grunt(&tuning, 99u, 28u, 3u, 600.0));
    recomp_hit_voice_parse_tuning("# mine\r\nmedium_from = 10\nheavy_from=20\ngap_ms=1500\nheavy_always=0\n"
        "volume_db=-3.5\nmeter_low=50\nmeter_high=40\nbogus=1\nnot a line\n", &tuning);
    expect("tuning read", tuning.medium_from == 10u && tuning.heavy_from == 20u && tuning.gap_ms == 1500u &&
        !tuning.heavy_always && tuning.volume == -350 && tuning.meter_low == 50u && tuning.meter_high == 50u);
    expect("heavy not always", !recomp_hit_voice_should_grunt(&tuning, 0u, 50u, 3u, 1e9));

    expect("kinds", recomp_hit_voice_kind("ayane_HIT_LV1_01.wav") == 1u &&
        recomp_hit_voice_kind("ayane_hit_lv3_02.WAV") == 3u && recomp_hit_voice_kind("ayane_KO_04.wav") == 4u &&
        recomp_hit_voice_kind("KO_01.wav") == 4u && recomp_hit_voice_kind("kokoro_01.wav") == 0u &&
        recomp_hit_voice_kind("ayane_HIT_LV4_01.wav") == 0u && recomp_hit_voice_kind("ayane_KO_01.txt") == 0u);

    for (uint32_t roll = 0u; roll < 2000u; roll += 7u) {
        const unsigned first = recomp_hit_voice_pick(4u, roll, -1);
        expect("in pool", first < 4u);
        expect("no repeat", recomp_hit_voice_pick(4u, roll, (int)first) != first);
    }
    expect("tiny pools", recomp_hit_voice_pick(0u, 9u, -1) == 0u && recomp_hit_voice_pick(1u, 9u, 0) == 0u);

    /* The hook: Kasumi (5) on side 1, with one heavy grunt and one KO cry. */
    const char *root = "hit_voice_test_clips";
    make_dir(root);
    make_dir("hit_voice_test_clips/kas");
    make_dir("hit_voice_test_clips/kas/selections");
    write_wav("hit_voice_test_clips/kas/selections/kasumi_HIT_LV3_01.wav", 24000u);
    write_wav("hit_voice_test_clips/kas/selections/kasumi_KO_01.wav", 96000u);
    write_wav("hit_voice_test_clips/kas/selections/kasumi_KO_02.wav", 90000u);
    recomp_hit_voice_install(false, root);
    ram[0x01u + 0x94u] = 5u; /* side 1 */
    ram[0x01u] = 99u;        /* side 0: no bank */
    for (unsigned i = 0u; i < 20u; ++i) land(0u, 200u, 60u);
    expect("unknown character silent", submits[0] == 0u);
    /* The replay ring: full, written at 0 when the fight below starts. */
    put16(ring + 0xc, 300u);
    put16(ring + 0x20, 300u);
    put16(ring + 0x18, 0u);
    recomp_hit_voice_frame();
    /* Scaled to 18, but a 40 base damage move: heavy, so it grunts at once. */
    hit(1u, 200u, 40u, 18u);
    expect("heavy base grunts", submits[1] >= 1u && first_bytes == 4000u * 2u && resets[1] == 0u);
    for (unsigned i = 0u; i < 20u; ++i) hit(1u, 200u, 40u, 18u);
    expect("gap holds", resets[1] == 0u);
    recomp_hit_voice_advance_clock(800.0);
    hit(1u, 200u, 40u, 18u);
    expect("grunts again after the gap", resets[1] == 1u);
    const uint8_t *grunt_pcm = last_pcm;
    for (unsigned write = 1u; write <= 92u; ++write) { /* the ring on to the KO */
        put16(ring + 0x18, write);
        recomp_hit_voice_frame();
    }
    hit(1u, 60u, 40u, 60u);
    expect("ko cries", resets[1] == 2u);
    const uint8_t *ko_pcm = last_pcm;
    hit(1u, 50u, 40u, 60u);
    expect("ko not cut", resets[1] == 2u);
    hit(1u, 0u, 40u, 60u);
    expect("juggle after the KO quiet", resets[1] == 2u);
    /* DOA2U's replay, as logged: the KO above landed at ring 92; the ring went
       on to 216, then the replay walked frames 60..299. The KO is frame 175, the
       grunt at ring 0 frame 83. */
    for (unsigned write = 93u; write <= 216u; ++write) {
        put16(ring + 0x18, write);
        recomp_hit_voice_frame();
    }
    recomp_hit_voice_advance_clock(3000.0);
    recomp_hit_voice_pump(); /* the cry has played out */
    unsigned sent = submits[1];
    for (unsigned frame = 60u; frame <= 299u; ++frame) {
        put16(shown, frame);
        recomp_hit_voice_frame();
        if (frame == 82u) expect("replay quiet before the grunt", submits[1] == sent);
        if (frame == 83u) expect("replay grunts as the fight did", submits[1] > sent && last_pcm == grunt_pcm);
        if (frame == 83u) sent = submits[1];
        if (frame == 174u) expect("replay quiet before the KO", resets[1] == 2u || last_pcm != ko_pcm);
        if (frame == 175u) expect("replay cries the KO's clip", last_pcm == ko_pcm);
    }
    /* A new round's recording forgets the last one's voices. */
    put16(ring + 0x20, 1u);
    recomp_hit_voice_frame();
    put16(shown, 0u);
    recomp_hit_voice_frame();
    const unsigned before_off = resets[1];
    recomp_hit_voice_set_choice(0u);
    expect("off stops", resets[1] == before_off + 1u);
    recomp_hit_voice_set_choice(2u);
    recomp_hit_voice_install(true, root);
    for (unsigned i = 0u; i < 20u; ++i) land(1u, 200u, 60u);
    expect("online silent", resets[1] == before_off + 1u);
    remove("hit_voice_test_clips/kas/selections/kasumi_HIT_LV3_01.wav");
    remove("hit_voice_test_clips/kas/selections/kasumi_KO_01.wav");
    remove("hit_voice_test_clips/kas/selections/kasumi_KO_02.wav");

    if (failures == 0) puts("hit voice tests passed");
    return failures != 0;
}
