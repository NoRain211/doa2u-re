/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "adx_decoder.h"
#include "adx_encoder.h"
#include "mod_loader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir(p, 0777)
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file == NULL) return;
    fwrite(data, 1u, size, file);
    fclose(file);
}

static FILE *base_file;
static bool base_read(void *context, uint64_t offset, void *buffer, uint32_t length, uint32_t *read)
{
    (void)context;
    fseek(base_file, (long)offset, SEEK_SET);
    *read = (uint32_t)fread(buffer, 1u, length, base_file);
    return true;
}

/* An archive of three named entries: "aaa" (5 bytes), "bbb" (3000) and "ccc" (7). */
static void make_archive(const char *path)
{
    static uint8_t afs[4 * 2048 + 2048];
    memset(afs, 0, sizeof afs);
    memcpy(afs, "AFS\0", 4u);
    put32(afs + 4, 3u);
    const uint32_t offsets[3] = {2048u, 4096u, 8192u}, sizes[3] = {5u, 3000u, 7u};
    for (unsigned i = 0u; i < 3u; ++i) {
        put32(afs + 8 + 8 * i, offsets[i]);
        put32(afs + 12 + 8 * i, sizes[i]);
        memset(afs + offsets[i], 'a' + (int)i, sizes[i]);
    }
    put32(afs + 8 + 8 * 3, 8192u + 2048u - 3u * 48u);
    put32(afs + 12 + 8 * 3, 3u * 48u);
    uint8_t *names = afs + 8192u + 2048u - 3u * 48u;
    const char *n[3] = {"aaa.adx", "bbb.adx", "ccc.bin"};
    for (unsigned i = 0u; i < 3u; ++i) {
        strcpy((char *)names + 48 * i, n[i]);
        put32(names + 48 * i + 44, sizes[i]);
    }
    write_file(path, afs, sizeof afs);
}

static void test_archive_overlay(const char *root)
{
    char disc[512], mods[512], path[512];
    snprintf(disc, sizeof disc, "%s/disc", root);
    snprintf(mods, sizeof mods, "%s/mods", root);
    make_dir(disc);
    snprintf(path, sizeof path, "%s/doa2", disc);
    make_dir(path);
    snprintf(path, sizeof path, "%s/doa2/bgm.afs", disc);
    make_archive(path);
    snprintf(path, sizeof path, "%s/doa2/movie.sfd", disc);
    write_file(path, "original", 8u);
    make_dir(mods);
    /* Two mods: "b" replaces bbb by name; "a" replaces it too but loads first, and replaces ccc by index. */
    snprintf(path, sizeof path, "%s/a", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/a/doa2", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/a/doa2/bgm.afs", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/a/doa2/bgm.afs/bbb.adx", mods); write_file(path, "AAAA", 4u);
    snprintf(path, sizeof path, "%s/a/doa2/bgm.afs/2_anything.bin", mods); write_file(path, "zz", 2u);
    snprintf(path, sizeof path, "%s/b", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/b/doa2", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/b/doa2/bgm.afs", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/b/doa2/bgm.afs/bbb.wav.adx", mods); write_file(path, "unused", 6u);
    snprintf(path, sizeof path, "%s/b/doa2/bgm.afs/BBB.ADX", mods); write_file(path, "BBBBBBBBBB", 10u);
    snprintf(path, sizeof path, "%s/b/doa2/movie.sfd", mods); write_file(path, "modded!", 7u);
    snprintf(path, sizeof path, "%s/_off", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/_off/doa2", mods); make_dir(path);
    snprintf(path, sizeof path, "%s/_off/doa2/movie.sfd", mods); write_file(path, "off", 3u);

    CHECK(recomp_mod_loader_install(mods, disc));
    CHECK(recomp_mod_loader_count() == 2u);
    CHECK(recomp_mod_loader_digest() != 0u);
    const char *replacement = recomp_mod_replacement("doa2/movie.sfd");
    CHECK(replacement != NULL && strstr(replacement, "/b/") != NULL);
    CHECK(recomp_mod_overlay("doa2/bgm.afs", 1234u) == NULL); /* not the original's size */
    const RecompModOverlay *overlay = recomp_mod_overlay("doa2/bgm.afs", 4u * 2048u + 2048u);
    CHECK(overlay != NULL);
    if (overlay == NULL) return;

    snprintf(path, sizeof path, "%s/doa2/bgm.afs", disc);
    base_file = fopen(path, "rb");
    const uint64_t size = recomp_mod_overlay_size(overlay);
    uint8_t *data = calloc(1u, (size_t)size + 16u);
    uint32_t got = 0u;
    /* Read in odd pieces, across piece boundaries. */
    for (uint64_t at = 0u; at < size;) {
        uint32_t step = 0u;
        CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, at, data + at, 777u, &step));
        if (step == 0u) break;
        at += step;
        got += step;
    }
    CHECK(got == size);
    CHECK(memcmp(data, "AFS\0", 4u) == 0 && get32(data + 4) == 3u);
    const uint32_t o0 = get32(data + 8), s0 = get32(data + 12), o1 = get32(data + 16), s1 = get32(data + 20),
                   o2 = get32(data + 24), s2 = get32(data + 28), on = get32(data + 32), sn = get32(data + 36);
    CHECK(s0 == 5u && memcmp(data + o0, "aaaaa", 5u) == 0);
    CHECK(s1 == 10u && memcmp(data + o1, "BBBBBBBBBB", 10u) == 0); /* the later mod's */
    CHECK(s2 == 2u && memcmp(data + o2, "zz", 2u) == 0);
    CHECK(o0 % 2048u == 0u && o1 % 2048u == 0u && o2 % 2048u == 0u && o0 < o1 && o1 < o2);
    CHECK(sn == 3u * 48u && strcmp((char *)data + on + 48, "bbb.adx") == 0 && get32(data + on + 48 + 44) == 10u);
    CHECK(on + sn <= size);
    uint32_t past = 99u;
    CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, size, data, 10u, &past) && past == 0u);
    free(data);
    fclose(base_file);
    recomp_mod_loader_shutdown();
    CHECK(recomp_mod_loader_count() == 0u && recomp_mod_replacement("doa2/movie.sfd") == NULL);
}

static const RecompModSource *once_source;
static unsigned starts;
static const RecompModSource *on_start(const char *key, unsigned index, const RecompModSource *wanted)
{
    (void)key; (void)wanted;
    ++starts;
    return index == 2u ? once_source : NULL;
}

static void read_all(const RecompModOverlay *overlay, uint8_t *data, uint64_t size)
{
    for (uint64_t at = 0u; at < size;) {
        uint32_t step = 0u;
        CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, at, data + at, 4096u, &step));
        if (step == 0u) break;
        at += step;
    }
}

/* A reserved archive: every entry gets the same room, and a track assigned
   while the game runs plays from that entry's next start. */
static void test_reserved_slots(const char *root)
{
    char disc[512], mods[512], path[512], track[512];
    snprintf(disc, sizeof disc, "%s/disc2", root);
    snprintf(mods, sizeof mods, "%s/nomods", root);
    make_dir(disc);
    snprintf(path, sizeof path, "%s/doa2", disc);
    make_dir(path);
    snprintf(path, sizeof path, "%s/doa2/bgm.afs", disc);
    make_archive(path);
    snprintf(track, sizeof track, "%s/track.adx", root);
    write_file(track, "QQQQQQ", 6u);
    recomp_mod_loader_reserve("doa2/bgm.afs", 5000u);
    CHECK(recomp_mod_loader_install(mods, disc));
    CHECK(recomp_mod_loader_count() == 0u && recomp_mod_loader_digest() == 0u);
    const RecompModOverlay *overlay = recomp_mod_overlay("doa2/bgm.afs", 4u * 2048u + 2048u);
    CHECK(overlay != NULL);
    if (overlay == NULL) return;
    CHECK(recomp_mod_entry_index("doa2/bgm.afs", "ccc.bin") == 2 && recomp_mod_slot_size("doa2/bgm.afs", 1u) == 6144u);
    const uint64_t size = recomp_mod_overlay_size(overlay);
    uint8_t *data = calloc(1u, (size_t)size + 16u);
    base_file = fopen(path, "rb");
    read_all(overlay, data, size);
    const uint32_t o1 = get32(data + 16), s1 = get32(data + 20);
    CHECK(s1 == 6144u && data[o1] == 'b' && data[o1 + 2999u] == 'b' && data[o1 + 3000u] == 0u);
    const RecompModSource *q = recomp_mod_source(track, 0u, 0u);
    CHECK(q != NULL && recomp_mod_assign("doa2/bgm.afs", 1u, q));
    /* A read past the start keeps what is playing; the next start plays the new track. */
    uint8_t probe[8] = {0};
    uint32_t got = 0u;
    CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, o1 + 4u, probe, 4u, &got) && probe[0] == 'b');
    CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, o1, probe, 8u, &got) && memcmp(probe, "QQQQQQ\0\0", 8u) == 0);
    CHECK(recomp_mod_assign("doa2/bgm.afs", 1u, NULL));
    CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, o1, probe, 4u, &got) && probe[0] == 'b');
    /* The start hook plays a track once instead. */
    once_source = q;
    recomp_mod_entry_start = on_start;
    const uint32_t o2 = get32(data + 24);
    CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, o2, probe, 2u, &got) && probe[0] == 'Q' && starts == 1u);
    recomp_mod_entry_start = NULL;
    CHECK(recomp_mod_overlay_read(overlay, base_read, NULL, o2, probe, 2u, &got) && probe[0] == 'c');
    fclose(base_file);
    free(data);
    recomp_mod_loader_shutdown();
    recomp_mod_loader_reserve(NULL, 0u);
}

static void test_keys(void)
{
    char key[256];
    CHECK(recomp_mod_key("C:\\Game\\disc\\", "c:\\game\\disc\\DOA2\\BGM.afs", key, sizeof key) && strcmp(key, "doa2/bgm.afs") == 0);
    CHECK(recomp_mod_key("C:\\Game\\disc", "C:\\Game\\disc\\.recomp-storage\\partition5\\voice.afs", key, sizeof key) &&
        strcmp(key, "doa2/voice.afs") == 0);
    CHECK(!recomp_mod_key("C:\\Game\\disc", "C:\\Game\\discs\\x.afs", key, sizeof key));
    CHECK(!recomp_mod_key("C:\\Game\\disc", "C:\\Game\\disc\\.recomp-storage\\partition1\\save.dat", key, sizeof key));
}

static void test_adx_round_trip(void)
{
    enum { FRAMES = 44100 / 2, RATE = 44100 };
    int16_t *pcm = malloc(FRAMES * 2 * sizeof *pcm);
    for (unsigned i = 0u; i < FRAMES; ++i) {
        pcm[2 * i] = (int16_t)(12000.0 * sin(2.0 * 3.14159265 * 440.0 * i / RATE));
        pcm[2 * i + 1] = (int16_t)(9000.0 * sin(2.0 * 3.14159265 * 1250.0 * i / RATE + 1.0));
    }
    size_t size = 0u;
    uint8_t *adx = recomp_adx_encode(pcm, FRAMES, 2u, RATE, 1000, FRAMES, &size);
    CHECK(adx != NULL);
    if (adx == NULL) { free(pcm); return; }
    RecompAdxDecoder decoder;
    const uint32_t data = recomp_adx_open(&decoder, adx, (uint32_t)size);
    CHECK(data != 0u && decoder.channels == 2u && decoder.sample_rate == RATE);
    /* The loop: 24 samples of silence first, so sample 1000 becomes 1024, frame 32, on a sector. */
    const uint32_t lead = (adx[0x14] << 8) | adx[0x15];
    const uint32_t begin = (uint32_t)adx[0x1c] << 24 | (uint32_t)adx[0x1d] << 16 | (uint32_t)adx[0x1e] << 8 | adx[0x1f];
    const uint32_t begin_byte = (uint32_t)adx[0x20] << 24 | (uint32_t)adx[0x21] << 16 | (uint32_t)adx[0x22] << 8 | adx[0x23];
    CHECK(lead == 24u && begin == 1024u && begin_byte % 2048u == 0u && begin_byte == data + 32u * 36u);
    CHECK(decoder.total_samples == FRAMES + lead);
    double error = 0.0, signal = 0.0;
    uint32_t at = data, decoded = 0u;
    int16_t out[RECOMP_ADX_FRAME_SAMPLES * 2];
    for (uint32_t n; (n = recomp_adx_decode(&decoder, adx + at, out)) != 0u; at += 36u) {
        for (uint32_t i = 0u; i < n; ++i) {
            const int64_t source = (int64_t)decoded + i - lead;
            for (unsigned ch = 0u; ch < 2u; ++ch) {
                const double want = source >= 0 ? pcm[source * 2 + ch] : 0.0, diff = out[i * 2 + ch] - want;
                error += diff * diff;
                signal += want * want;
            }
        }
        decoded += n;
    }
    CHECK(decoded == FRAMES + lead);
    const double snr = 10.0 * log10(signal / (error + 1.0));
    if (snr < 20.0) fprintf(stderr, "ADX round trip SNR %.1f dB\n", snr);
    CHECK(snr >= 20.0);
    free(adx);
    free(pcm);
}

static void test_wav(void)
{
    uint8_t wav[44 + 8 + 68];
    memset(wav, 0, sizeof wav);
    memcpy(wav, "RIFF", 4u);
    put32(wav + 4, sizeof wav - 8u);
    memcpy(wav + 8, "WAVEfmt ", 8u);
    put32(wav + 16, 16u);
    wav[20] = 1u; wav[22] = 1u;
    put32(wav + 24, 22050u);
    put32(wav + 28, 44100u);
    wav[32] = 2u; wav[34] = 16u;
    memcpy(wav + 36, "data", 4u);
    put32(wav + 40, 8u);
    wav[44] = 0x34; wav[45] = 0x12; wav[50] = 0xff; wav[51] = 0xff;
    memcpy(wav + 52, "smpl", 4u);
    put32(wav + 56, 60u);
    put32(wav + 60 + 28, 1u);
    put32(wav + 60 + 36 + 8, 1u);
    put32(wav + 60 + 36 + 12, 2u);
    uint32_t frames, channels, rate;
    int64_t start, end;
    int16_t *pcm = recomp_wav_read(wav, sizeof wav, &frames, &channels, &rate, &start, &end);
    CHECK(pcm != NULL && frames == 4u && channels == 1u && rate == 22050u);
    CHECK(pcm != NULL && pcm[0] == 0x1234 && pcm[3] == -1);
    CHECK(start == 1 && end == 3);
    free(pcm);
}

int main(void)
{
    char root[512];
#ifdef _WIN32
    snprintf(root, sizeof root, "%s\\mod_loader_test_%lu", getenv("TEMP") ? getenv("TEMP") : ".", (unsigned long)rand());
#else
    snprintf(root, sizeof root, "/tmp/mod_loader_test_%ld", (long)getpid());
#endif
    make_dir(root);
    test_keys();
    test_archive_overlay(root);
    test_reserved_slots(root);
    test_adx_round_trip();
    test_wav();
    if (failures == 0) printf("mod loader: ok\n");
    return failures == 0 ? 0 : 1;
}
