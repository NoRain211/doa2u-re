/* SPDX-License-Identifier: GPL-3.0-or-later */
/* recomp_adx_encode in.wav out.adx [loop start] [loop end]: a WAV as the ADX
   the game plays, with adx_encoder.c. tools/import_soundtrack.py uses it for
   DOA4's music; loop points are in samples, end exclusive, and override the
   WAV's own smpl loop.

   recomp_adx_encode --level dB in.adx out.adx: the ADX again, its loudness
   (RMS over the whole track) brought to dB (dBFS), keeping its loop. Peaks the
   gain would push past -1 dBFS are rounded off rather than clipped. It prints
   the gain it applied. import_soundtrack.py levels every album with it so they
   play as loud as DOA2U's own music. */
#include "adx_decoder.h"
#include "adx_encoder.h"

#include <math.h>
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

static int level(double target, const char *from, const char *to)
{
    long length = 0;
    unsigned char *data = read_file(from, &length);
    if (data == NULL) return 1;
    RecompAdxDecoder decoder;
    uint32_t at = recomp_adx_open(&decoder, data, (uint32_t)length);
    if (at == 0u) { fprintf(stderr, "%s: not an ADX file the game plays\n", from); free(data); return 1; }
    int64_t loop_start = -1, loop_end = -1;
    const uint32_t loop = data[0x12] == 3u ? 0x18u : data[0x12] == 4u ? 0x24u : 0u;
    if (loop != 0u && loop + 0x14u <= at && be32(data + loop) != 0u) {
        loop_start = be32(data + loop + 4u);
        loop_end = be32(data + loop + 12u);
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
    if (loop_end > (int64_t)frames) loop_end = frames;
    size_t size = 0u;
    unsigned char *adx = recomp_adx_encode(pcm, frames, channels, rate, loop_start, loop_end, &size);
    free(pcm);
    FILE *out = adx != NULL ? fopen(to, "wb") : NULL;
    if (out == NULL || fwrite(adx, 1u, size, out) != size) { fprintf(stderr, "%s: cannot write\n", to); return 1; }
    fclose(out);
    free(adx);
    printf("%.1f dB -> %.1f dB (%+.1f dB)\n", rms, target, target - rms);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--level") == 0) return level(strtod(argv[2], NULL), argv[3], argv[4]);
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: %s in.wav out.adx [loop_start loop_end]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (in == NULL) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    const long length = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *data = length > 0 ? malloc((size_t)length) : NULL;
    if (data == NULL || fread(data, 1u, (size_t)length, in) != (size_t)length) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }
    fclose(in);
    uint32_t frames, channels, rate;
    int64_t loop_start, loop_end;
    int16_t *pcm = recomp_wav_read(data, (size_t)length, &frames, &channels, &rate, &loop_start, &loop_end);
    free(data);
    if (pcm == NULL) { fprintf(stderr, "%s: not a mono or stereo PCM WAV\n", argv[1]); return 1; }
    if (argc == 5) {
        loop_start = strtoll(argv[3], NULL, 10);
        loop_end = strtoll(argv[4], NULL, 10);
    }
    size_t size = 0u;
    unsigned char *adx = recomp_adx_encode(pcm, frames, channels, rate, loop_start, loop_end, &size);
    free(pcm);
    FILE *out = adx != NULL ? fopen(argv[2], "wb") : NULL;
    if (out == NULL || fwrite(adx, 1u, size, out) != size) { fprintf(stderr, "%s: cannot write\n", argv[2]); return 1; }
    fclose(out);
    free(adx);
    return 0;
}
