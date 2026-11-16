/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* Exercise the player and assignment failure paths without a game or audio device. */
#include "jukebox.c"

static unsigned resets;
void recomp_audio_output_reset_voice(uint32_t slot) { (void)slot; ++resets; }
void recomp_dsound_buffer_set_output_muted(bool muted) { (void)muted; }
void recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t rate, uint32_t channels, uint32_t bits, int32_t volume)
{
    (void)slot; (void)pcm; (void)bytes; (void)rate; (void)channels; (void)bits; (void)volume;
    abort(); /* These tests must never emit audio. */
}

int main(void)
{
    unsigned failures = 0u;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "jukebox: %s\n", #c); ++failures; } } while (0)
    album_count = 1u;
    player.playing = true;
    player.data = malloc(1u);
    player.album = 3u;
    advance();
    CHECK(!player.playing && resets == 1u);
    player.playing = true;
    player.data = malloc(1u);
    player.album = 0u;
    mode = MODE_SHUFFLE;
    advance();
    CHECK(!player.playing && resets == 2u);

    Track missing = {0};
    snprintf(missing.path, sizeof missing.path, "jukebox-test-missing-track.adx");
    snprintf(missing.name, sizeof missing.name, "Missing");
    albums[0].tracks = &missing;
    albums[0].count = 1u;
    Target target = {"original.adx", "Original", 0, 0, 0};
    CHECK(!apply(&target) && target.album == -1 && target.track == -1);
    albums[0].tracks = NULL;
    albums[0].count = 0u;

    char out[96];
    const char *name = "abcdefghijklmnopqrstuvwx\xc3\xa9\xc3\xa9";
    CHECK(strcmp(fit(out, sizeof out, name), name) == 0);
    fit(out, sizeof out, "abcdefghijklmnopqrstuv\xe6\x97\xa5\xc3\xa9\xc3\xa9-more");
    CHECK(strcmp(out, "abcdefghijklmnopqrstuv\xe6\x97\xa5...") == 0);
    char small[5];
    fit(small, sizeof small, "\xe6\x97\xa5\xe6\x97\xa5");
    CHECK(strcmp(small, "...") == 0);
    return failures != 0u;
}
