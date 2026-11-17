/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* Exercise the player, the names and assignments' files, and assignment
   failure paths without a game or audio device. */
#include "jukebox.c"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "jukebox: %s\n", #c); ++failures; } } while (0)

static unsigned resets;
void recomp_audio_output_reset_voice(uint32_t slot) { (void)slot; ++resets; }
void recomp_dsound_buffer_set_output_muted(bool muted) { (void)muted; }
void recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t rate, uint32_t channels, uint32_t bits, int32_t volume)
{
    (void)slot; (void)pcm; (void)bytes; (void)rate; (void)channels; (void)bits; (void)volume;
    abort(); /* These tests must never emit audio. */
}

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file == NULL) return;
    CHECK(fwrite(data, 1u, size, file) == size);
    fclose(file);
}

static size_t read_text(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0u;
    const size_t got = fread(out, 1u, size - 1u, file);
    fclose(file);
    out[got] = '\0';
    return got;
}

static unsigned count_text(const char *text, const char *part)
{
    unsigned count = 0u;
    for (const char *at = strstr(text, part); at != NULL; at = strstr(at + 1, part)) ++count;
    return count;
}

/* names.txt keeps the player's own lines and only adds the missing ones. */
static void test_names(const char *base)
{
    static const char initial[] =
        "# keep me\nalbum = Player Title\none.adx = Renamed\nmystery = whatever\n";
    char path[512], text[4096], again[4096];
    make_folder(base);
    snprintf(jukebox_root, sizeof jukebox_root, "%s/names", base);
    make_folder(jukebox_root);
    snprintf(path, sizeof path, "%s/album", jukebox_root);
    make_folder(path);
    snprintf(path, sizeof path, "%s/album/names.txt", jukebox_root);
    write_file(path, initial, sizeof initial - 1u);

    Track tracks[2] = {{0}};
    snprintf(tracks[0].file, sizeof tracks[0].file, "one.adx");
    snprintf(tracks[0].name, sizeof tracks[0].name, "One");
    snprintf(tracks[1].file, sizeof tracks[1].file, "two.adx");
    snprintf(tracks[1].name, sizeof tracks[1].name, "Two");
    Album album = {0};
    album.tracks = tracks;
    album.count = 2u;
    snprintf(album.tab, sizeof album.tab, "TEST");
    snprintf(album.title, sizeof album.title, "Default");
    apply_names(&album, "album");
    CHECK(strcmp(album.title, "Player Title") == 0);
    CHECK(strcmp(tracks[0].name, "Renamed") == 0 && strcmp(tracks[1].name, "Two") == 0);
    (void)read_text(path, text, sizeof text);
    CHECK(strstr(text, "# keep me") != NULL);
    CHECK(strstr(text, "mystery = whatever") != NULL);
    CHECK(strstr(text, "album = Player Title") != NULL);
    CHECK(count_text(text, "one.adx") == 1u);
    CHECK(strstr(text, "two.adx = Two") != NULL);
    /* Complete: a second pass leaves every line where it was. */
    apply_names(&album, "album");
    CHECK(read_text(path, again, sizeof again) == strlen(text) && strcmp(text, again) == 0);
}

/* tracks.txt may only name files beside it. */
static void test_tracks(const char *base)
{
    static const char tracks[] =
        "#title\tBad Album\n"
        "good.adx\t-2\t-2\tGood\n"
        "../evil.adx\t-2\t-2\tEvil\n"
        "sub/bad.adx\t-2\t-2\tSub\n"
        "c:abs.adx\t-2\t-2\tAbs\n"
        "\t-2\t-2\tEmpty\n"
        "back\\slash.adx\t-2\t-2\tBack\n";
    char path[512];
    make_folder(base);
    snprintf(jukebox_root, sizeof jukebox_root, "%s/folders", base);
    make_folder(jukebox_root);
    snprintf(path, sizeof path, "%s/bad", jukebox_root);
    make_folder(path);
    snprintf(path, sizeof path, "%s/bad/tracks.txt", jukebox_root);
    write_file(path, tracks, sizeof tracks - 1u);

    Album album = {0};
    album.missing = "none";
    load_folder(&album, "bad", "BAD");
    CHECK(strcmp(album.title, "Bad Album") == 0);
    CHECK(album.count == 1u);
    if (album.count == 1u) {
        CHECK(strcmp(album.tracks[0].name, "Good") == 0);
        CHECK(strcmp(album.tracks[0].file, "good.adx") == 0);
        snprintf(path, sizeof path, "%s/bad/good.adx", jukebox_root);
        CHECK(strcmp(album.tracks[0].path, path) == 0);
    }
    free(album.tracks);
}

/* The player's files and the game's table name music in any case. */
static void test_target_names(void)
{
    target_count = 2u;
    targets[0] = (Target){"bgm_doa2_ks.adx", "Kasumi", 0, -1, -1};
    targets[1] = (Target){"RMX_KS.ADX", "Kasumi (Remix)", 1, -1, -1};
    CHECK(find_target("bgm_doa2_ks.adx") == 0);
    CHECK(find_target("BGM_DOA2_KS.ADX") == 0);
    CHECK(find_target("rmx_ks.adx") == 1);
    CHECK(find_target("nope.adx") == -1);
    target_count = 0u;
}

/* A reserved bgm.afs, so assignments really land in a slot: a synthetic disc
   with one named entry, "bgm_doa2_ks.adx", 4 bytes of 'a'. */
enum { AFS_SIZE = 4096, AFS_ENTRY = 2048, AFS_ENTRY_SIZE = 4, AFS_NAMES = AFS_SIZE - 48 };

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void make_archive(const char *path)
{
    uint8_t afs[AFS_SIZE];
    memset(afs, 0, sizeof afs);
    memcpy(afs, "AFS\0", 4u);
    put32(afs + 4, 1u);
    put32(afs + 8, AFS_ENTRY);
    put32(afs + 12, AFS_ENTRY_SIZE);
    memset(afs + AFS_ENTRY, 'a', AFS_ENTRY_SIZE);
    put32(afs + 16, AFS_NAMES);
    put32(afs + 20, 48u);
    strcpy((char *)afs + AFS_NAMES, "bgm_doa2_ks.adx");
    put32(afs + AFS_NAMES + 44u, AFS_ENTRY_SIZE);
    write_file(path, afs, sizeof afs);
}

static FILE *base_file;
static bool base_read(void *context, uint64_t offset, void *buffer, uint32_t length, uint32_t *read)
{
    (void)context;
    if (base_file == NULL || fseek(base_file, (long)offset, SEEK_SET) != 0) return false;
    *read = (uint32_t)fread(buffer, 1u, length, base_file);
    return true;
}

/* What the entry plays when its track starts now. */
static uint8_t entry_byte(const RecompModOverlay *overlay, uint64_t at)
{
    uint8_t probe[8] = {0};
    uint32_t got = 0u;
    if (!recomp_mod_overlay_read(overlay, base_read, NULL, at, probe, sizeof probe, &got) ||
        got != sizeof probe)
        return 0u;
    return probe[0];
}

/* Assigning keeps a failed change from losing the track that worked, and a
   reload puts a target the file no longer names back on its own music. */
static void test_assignments(const char *base)
{
    static uint8_t huge_data[7000];
    static const char tracks[] = "track.adx\t-2\t-2\tTrack\nhuge.adx\t-2\t-2\tHuge\n";
    char disc[512], mods[512], private_dir[512], path[512], text[512];
    memset(huge_data, 'Z', sizeof huge_data);
    make_folder(base);
    snprintf(disc, sizeof disc, "%s/disc", base);
    make_folder(disc);
    snprintf(path, sizeof path, "%s/DOA2", disc);
    make_folder(path);
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc);
    make_archive(path);
    snprintf(mods, sizeof mods, "%s/mods", base);
    make_folder(mods);
    snprintf(jukebox_root, sizeof jukebox_root, "%s/jukebox", base);
    make_folder(jukebox_root);
    snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
    remove(path);
    snprintf(path, sizeof path, "%s/doa3", jukebox_root);
    make_folder(path);
    snprintf(path, sizeof path, "%s/doa3/track.adx", jukebox_root);
    write_file(path, "QQQQQQ", 6u);
    snprintf(path, sizeof path, "%s/doa3/huge.adx", jukebox_root);
    write_file(path, huge_data, sizeof huge_data);
    snprintf(path, sizeof path, "%s/doa3/tracks.txt", jukebox_root);
    write_file(path, tracks, sizeof tracks - 1u);

    recomp_mod_loader_reserve(BGM, 5000u);
    CHECK(recomp_mod_loader_install(mods, disc));
    snprintf(private_dir, sizeof private_dir, "%s/private", base);
    recomp_jukebox_prepare(disc, private_dir);
    recomp_jukebox_install(false);
    CHECK(assigning);

    const RecompModOverlay *overlay = recomp_mod_overlay(BGM, AFS_SIZE);
    uint64_t at = 0u;
    CHECK(overlay != NULL && recomp_mod_entry_offset(BGM, AFS_SIZE, 0u, &at));
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc);
    base_file = fopen(path, "rb");
    CHECK(base_file != NULL);
    if (overlay == NULL || base_file == NULL) return;
    CHECK(entry_byte(overlay, at) == 'a');

    const int doa3 = find_album("DOA3");
    const int track = doa3 >= 0 ? find_track((unsigned)doa3, "track.adx") : -1;
    const int huge = doa3 >= 0 ? find_track((unsigned)doa3, "huge.adx") : -1;
    CHECK(track >= 0 && huge >= 0);
    if (doa3 >= 0 && track >= 0 && huge >= 0) {
        /* A track that fits plays from the slot. */
        page.album = (unsigned)doa3;
        page.track = (unsigned)track;
        page.target = 0u;
        recomp_jukebox_confirm(ROW_TARGET);
        CHECK(targets[0].album == doa3 && targets[0].track == track);
        CHECK(entry_byte(overlay, at) == 'Q');
        snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
        (void)read_text(path, text, sizeof text);
        CHECK(strstr(text, "bgm_doa2_ks.adx\tDOA3\ttrack.adx") != NULL);

        /* The page reload keeps an assignment that still resolves. */
        recomp_jukebox_page_opened();
        CHECK(targets[0].album == doa3 && targets[0].track == track);
        CHECK(entry_byte(overlay, at) == 'Q');

        /* The saved names match in any case. */
        static const char upper[] = "BGM_DOA2_KS.ADX\tdoa3\tTRACK.ADX\n";
        write_file(path, upper, sizeof upper - 1u);
        recomp_jukebox_page_opened();
        CHECK(targets[0].album == doa3 && targets[0].track == track);
        CHECK(entry_byte(overlay, at) == 'Q');

        /* A track too big for the slot fails and leaves the working one. */
        page.album = (unsigned)doa3;
        page.track = (unsigned)huge;
        page.target = 0u;
        recomp_jukebox_confirm(ROW_TARGET);
        CHECK(targets[0].album == doa3 && targets[0].track == track);
        CHECK(entry_byte(overlay, at) == 'Q');

        /* A target the reloaded file no longer names plays its own music. */
        remove(path);
        recomp_jukebox_page_opened();
        CHECK(targets[0].album == -1 && targets[0].track == -1);
        CHECK(entry_byte(overlay, at) == 'a');
    }
    fclose(base_file);
    base_file = NULL;
    recomp_mod_loader_shutdown();
    recomp_mod_loader_reserve(NULL, 0u);
}

int main(void)
{
    const char *root = "jukebox-test";
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
    /* A failed change leaves the target's assignment to its caller. */
    CHECK(!apply(&target) && target.album == 0 && target.track == 0);
    albums[0].tracks = NULL;
    albums[0].count = 0u;
    album_count = 0u;

    char out[96];
    const char *name = "abcdefghijklmnopqrstuvwx\xc3\xa9\xc3\xa9";
    CHECK(strcmp(fit(out, sizeof out, name), name) == 0);
    fit(out, sizeof out, "abcdefghijklmnopqrstuv\xe6\x97\xa5\xc3\xa9\xc3\xa9-more");
    CHECK(strcmp(out, "abcdefghijklmnopqrstuv\xe6\x97\xa5...") == 0);
    char tiny[5];
    fit(tiny, sizeof tiny, "\xe6\x97\xa5\xe6\x97\xa5");
    CHECK(strcmp(tiny, "...") == 0);

    test_names(root);
    test_tracks(root);
    test_target_names();
    test_assignments(root);
    return failures != 0u;
}
