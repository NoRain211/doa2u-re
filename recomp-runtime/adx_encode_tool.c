/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* atomic_file.h's fsync and fileno */
#endif
/* recomp_adx_encode in.wav out.adx [loop start] [loop end]: a WAV as the ADX
   the game plays, with adx_encoder.c. tools/import_soundtrack.py uses it for
   DOA4's music; loop points are in samples, end exclusive, and override the
   WAV's own smpl loop.

   recomp_adx_encode --level dB [loop start] [loop end] in.adx out.adx: the ADX
   again, its loudness (RMS over the whole track) brought to dB (dBFS), keeping
   its loop: the given samples as tracks.txt holds them, else its own header's
   loop (-1) or none (-2). Peaks the gain would push past -1 dBFS are rounded
   off rather than clipped. It prints the gain it applied. import_soundtrack.py
   levels every album with it so they play as loud as DOA2U's own music, and a
   loop only tracks.txt knows (a DOA4 album ffmpeg encoded before the game was
   built) reaches the game through its ADX header this way. */
#include "adx_decoder.h"
#include "adx_encoder.h"
#include "atomic_file.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static unsigned char *read_file(const char *path, long *length)
{
    FILE *in = fopen(path, "rb");
    if (in == NULL) { perror(path); return NULL; }
    fseek(in, 0, SEEK_END);
    *length = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *data = *length > 0 ? malloc((size_t)*length) : NULL;
    if (data == NULL || fread(data, 1u, (size_t)*length, in) != (size_t)*length) {
        fprintf(stderr, "%s: cannot read\n", path);
        fclose(in);
        free(data);
        return NULL;
    }
    fclose(in);
    return data;
}

/* A failed or interrupted write leaves the file it replaces intact. */
static int write_file(const char *path, const unsigned char *data, size_t size)
{
    char temporary[4096];
    FILE *out = recomp_atomic_open(path, temporary, sizeof temporary);
    if (out == NULL) return 0;
    const bool written = fwrite(data, 1u, size, out) == size;
    return recomp_atomic_commit(out, temporary, path, written) ? 1 : 0;
}

/* Tracks' loop points in tracks.txt's terms: a start of -1 keeps the header's own
   loop, -2 (or any other negative) plays once, and samples given as >= 0 win.
   Points the caller gave (loop_given) must fit the track: one that does not is an
   error, so the album stays as it was and its row keeps the points. An own header
   loop that does not fit is dropped, as it would never trigger in the game. */
static int level(double target, int64_t loop_start, int64_t loop_end, bool loop_given,
    const char *from, const char *to)
{
    long length = 0;
    unsigned char *data = read_file(from, &length);
    if (data == NULL) return 1;
    RecompAdxDecoder decoder;
    uint32_t at = recomp_adx_open(&decoder, data, (uint32_t)length);
    if (at == 0u) { fprintf(stderr, "%s: not an ADX file the game plays\n", from); free(data); return 1; }
    if (loop_start == -1) {
        const uint32_t loop = data[0x12] == 3u ? 0x18u : data[0x12] == 4u ? 0x24u : 0u;
        if (loop != 0u && loop + 0x14u <= at && be32(data + loop) != 0u) {
            loop_start = be32(data + loop + 4u);
            loop_end = be32(data + loop + 12u);
        }
    } else if (loop_start < 0) {
        loop_start = loop_end = -1; /* -2: no loop */
    }
    const uint32_t channels = decoder.channels, rate = decoder.sample_rate;
    int16_t *pcm = malloc(((size_t)decoder.total_samples + RECOMP_ADX_FRAME_SAMPLES) * channels * sizeof *pcm);
    if (pcm == NULL) { free(data); return 1; }
    uint32_t frames = 0u;
    double sum = 0.0;
    int16_t block[RECOMP_ADX_FRAME_SAMPLES * RECOMP_ADX_MAX_CHANNELS];
    while (at + RECOMP_ADX_FRAME_BYTES * channels <= (uint32_t)length) {
        const uint32_t count = recomp_adx_decode(&decoder, data + at, block);
        if (count == 0u) break;
        at += RECOMP_ADX_FRAME_BYTES * channels;
        memcpy(pcm + (size_t)frames * channels, block, (size_t)count * channels * sizeof *pcm);
        for (uint32_t i = 0u; i < count * channels; ++i) sum += (double)block[i] * block[i];
        frames += count;
    }
    free(data);
    if (frames == 0u || sum == 0.0) { fprintf(stderr, "%s: silent\n", from); free(pcm); return 1; }
    const double rms = 20.0 * log10(sqrt(sum / ((double)frames * channels)) / 32768.0);
    const double gain = pow(10.0, (target - rms) / 20.0), knee = 0.891 * 32767.0; /* -1 dBFS */
    for (size_t i = 0u; i < (size_t)frames * channels; ++i) {
        double v = pcm[i] * gain;
        const double size = fabs(v);
        if (size > knee) v = (v < 0 ? -1.0 : 1.0) * (knee + (32767.0 - knee) * tanh((size - knee) / (32767.0 - knee)));
        pcm[i] = (int16_t)lrint(v > 32767.0 ? 32767.0 : v < -32768.0 ? -32768.0 : v);
    }
    const int64_t wanted_start = loop_start, wanted_end = loop_end;
    if (loop_end > (int64_t)frames) loop_end = frames;
    if (loop_start >= 0 && loop_end <= loop_start) {
        if (loop_given) {
            fprintf(stderr, "%s: its loop %lld..%lld does not fit its %u samples\n",
                from, (long long)wanted_start, (long long)wanted_end, (unsigned)frames);
            free(pcm);
            return 1;
        }
        fprintf(stderr, "%s: its own loop %lld..%lld does not fit its %u samples; levelled without one\n",
            from, (long long)wanted_start, (long long)wanted_end, (unsigned)frames);
        loop_start = loop_end = -1;
    }
    size_t size = 0u;
    unsigned char *adx = recomp_adx_encode(pcm, frames, channels, rate, loop_start, loop_end, &size);
    free(pcm);
    const bool written = adx != NULL && write_file(to, adx, size);
    free(adx);
    if (!written) { fprintf(stderr, "%s: cannot write\n", to); return 1; }
    printf("%.1f dB -> %.1f dB (%+.1f dB)\n", rms, target, target - rms);
    return 0;
}

static int usage(const char *program)
{
    fprintf(stderr, "usage: %s in.wav out.adx [loop_start loop_end]\n"
        "       %s --level dB [loop_start loop_end] in.adx out.adx\n", program, program);
    return 2;
}

/* Whether the arguments at [at] and [at + 1] are whole sample numbers. */
static bool loop_arguments(char **argv, int at, int64_t *start, int64_t *end)
{
    char *start_end = NULL, *end_end = NULL;
    errno = 0;
    *start = strtoll(argv[at], &start_end, 10);
    *end = strtoll(argv[at + 1], &end_end, 10);
    return *start_end == '\0' && *end_end == '\0' && errno != ERANGE;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--level") == 0) {
        if (argc != 5 && argc != 7) return usage(argv[0]);
        char *given = NULL;
        const double target = strtod(argv[2], &given);
        if (given == argv[2] || *given != '\0' || !isfinite(target)) {
            fprintf(stderr, "%s: --level wants a number of dB\n", argv[0]);
            return 2;
        }
        if (argc == 5) return level(target, -1, -1, false, argv[3], argv[4]);
        int64_t loop_start, loop_end;
        if (!loop_arguments(argv, 3, &loop_start, &loop_end)) {
            fprintf(stderr, "%s: loop points are whole samples, not %s and %s\n",
                argv[0], argv[3], argv[4]);
            return 2;
        }
        return level(target, loop_start, loop_end, true, argv[5], argv[6]);
    }
    if (argc != 3 && argc != 5) return usage(argv[0]);
    int64_t given_start = -1, given_end = -1;
    if (argc == 5 && !loop_arguments(argv, 3, &given_start, &given_end)) {
        fprintf(stderr, "%s: loop points are whole samples, not %s and %s\n", argv[0], argv[3], argv[4]);
        return 2;
    }
    long length = 0;
    unsigned char *data = read_file(argv[1], &length);
    if (data == NULL) return 1;
    uint32_t frames, channels, rate;
    int64_t loop_start, loop_end;
    int16_t *pcm = recomp_wav_read(data, (size_t)length, &frames, &channels, &rate, &loop_start, &loop_end);
    free(data);
    if (pcm == NULL) { fprintf(stderr, "%s: not a mono or stereo PCM WAV\n", argv[1]); return 1; }
    if (argc == 5 && (given_start < 0 || given_end <= given_start || given_end > (int64_t)frames)) {
        fprintf(stderr, "%s: its loop %lld..%lld does not fit its %u samples\n",
            argv[0], (long long)given_start, (long long)given_end, (unsigned)frames);
        free(pcm);
        return 1;
    }
    if (argc == 5) {
        loop_start = given_start;
        loop_end = given_end;
    }
    size_t size = 0u;
    unsigned char *adx = recomp_adx_encode(pcm, frames, channels, rate, loop_start, loop_end, &size);
    free(pcm);
    const bool written = adx != NULL && write_file(argv[2], adx, size);
    free(adx);
    if (!written) { fprintf(stderr, "%s: cannot write\n", argv[2]); return 1; }
    return 0;
}
