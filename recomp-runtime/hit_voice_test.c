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
enum { BASE = 0x0087ae30u };
uint8_t *recomp_memory(uint32_t address, size_t width)
{
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

static unsigned submits[2], resets[2], first_bytes;
void recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes, uint32_t rate,
    uint32_t channels, uint32_t bits, int32_t volume)
{
    (void)pcm; (void)volume;
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

int main(void)
{
    /* The 15 fighters and their DOA4 banks; no others. */
    const unsigned ids[15] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 17, 21, 24};
    const char *names[15] = {"zac", "tin", "jan", "ein", "ryu", "kas", "gen", "hel", "bay", "bas",
        "lei", "aya", "tng", "hak", "leo"};
    for (unsigned i = 0; i < 15u; ++i)
        expect(names[i], recomp_hit_voice_prefix((uint8_t)ids[i]) != NULL &&
            strcmp(recomp_hit_voice_prefix((uint8_t)ids[i]), names[i]) == 0);
    expect("no clone", recomp_hit_voice_prefix(10u) == NULL && recomp_hit_voice_prefix(25u) == NULL &&
        recomp_hit_voice_prefix(255u) == NULL);

    expect("levels", recomp_hit_voice_level(5u) == 1u && recomp_hit_voice_level(15u) == 2u &&
        recomp_hit_voice_level(30u) == 3u && recomp_hit_voice_level(45u) == 4u && recomp_hit_voice_level(80u) == 5u);

    /* 13 clips: level 1 draws from 0..6, level 5 from 6..12, never the last one twice. */
    for (uint32_t roll = 0u; roll < 2000u; roll += 7u) {
        const unsigned light = recomp_hit_voice_pick(13u, 1u, roll, -1);
        const unsigned heavy = recomp_hit_voice_pick(13u, 5u, roll, -1);
        expect("light window", light <= 6u);
        expect("heavy window", heavy >= 6u && heavy <= 12u);
        const unsigned again = recomp_hit_voice_pick(13u, 3u, roll, (int)recomp_hit_voice_pick(13u, 3u, roll, -1));
        expect("no repeat", again != recomp_hit_voice_pick(13u, 3u, roll, -1));
    }
    expect("tiny pools", recomp_hit_voice_pick(0u, 3u, 9u, -1) == 0u && recomp_hit_voice_pick(1u, 5u, 9u, 0) == 0u &&
        recomp_hit_voice_pick(2u, 5u, 1u, -1) < 2u);

    /* The hook: Kasumi (5) on side 1 grunts on a hit and cries at the KO. */
    char root[] = "hit_voice_test_clips";
    char path[128];
    remove("hit_voice_test_clips/kas/hit_00.wav");
    remove("hit_voice_test_clips/kas/ko_00.wav");
#ifdef _WIN32
    _mkdir(root);
    _mkdir("hit_voice_test_clips/kas");
#else
    mkdir(root, 0755);
    mkdir("hit_voice_test_clips/kas", 0755);
#endif
    snprintf(path, sizeof path, "%s/kas/hit_00.wav", root);
    write_wav(path, 24000u);
    snprintf(path, sizeof path, "%s/kas/ko_00.wav", root);
    write_wav(path, 96000u);
    recomp_hit_voice_install(false, root);
    ram[0x01u + 0x94u] = 5u; /* side 1 */
    ram[0x01u] = 99u;        /* side 0: no bank */
    land(0u, 100u, 30u);
    expect("unknown character silent", submits[0] == 0u);
    land(1u, 100u, 30u);
    expect("hit grunts", submits[1] == 1u && first_bytes == 4000u * 2u && resets[1] == 0u);
    land(1u, 70u, 10u);
    expect("light hit right after waits", resets[1] == 0u);
    land(1u, 60u, 60u);
    expect("ko cries", resets[1] == 1u && submits[1] >= 2u);
    land(1u, 50u, 60u);
    expect("ko not cut", resets[1] == 1u);
    recomp_hit_voice_install(true, root);
    land(1u, 100u, 30u);
    expect("online silent", resets[1] == 1u);
    remove("hit_voice_test_clips/kas/hit_00.wav");
    remove("hit_voice_test_clips/kas/ko_00.wav");

    if (failures == 0) puts("hit voice tests passed");
    return failures != 0;
}
