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

    /* A line longer than PATH_SIZE stays one line: its tail that looks like an
       entry renames nothing, and the line is kept whole. */
    static char long_comment[1100];
    memset(long_comment, 'x', 1023u);
    long_comment[0] = '#';
    snprintf(long_comment + 1023, sizeof long_comment - 1023u, "two.adx = Sneaky\n");
    write_file(path, long_comment, strlen(long_comment));
    apply_names(&album, "album");
    CHECK(strcmp(tracks[1].name, "Two") == 0);
    CHECK(read_text(path, again, sizeof again) > 0u && strstr(again, long_comment) != NULL);

    /* Its last line without a newline is kept, and gains one. */
    write_file(path, "# last", 6u);
    apply_names(&album, "album");
    (void)read_text(path, again, sizeof again);
    CHECK(strcmp(again, "# last\nalbum = Player Title\n\none.adx = Renamed\ntwo.adx = Two\n") == 0);

    /* A file that exists but cannot be read is not intact: a directory fails to
       open on Windows and fails to read elsewhere. Only an absent one is. */
    Lines lines;
    snprintf(path, sizeof path, "%s/unreadable", jukebox_root);
    make_folder(path);
    lines_open(&lines, path);
    while (lines_next(&lines)) {}
    CHECK(!lines.intact);
    lines_close(&lines);
    snprintf(path, sizeof path, "%s/absent.txt", jukebox_root);
    lines_open(&lines, path);
    while (lines_next(&lines)) {}
    CHECK(lines.intact);
    lines_close(&lines);
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
    memset(targets, 0, sizeof targets);
    snprintf(targets[0].file, sizeof targets[0].file, "one.adx");
    snprintf(targets[0].name, sizeof targets[0].name, "One");
    snprintf(targets[1].file, sizeof targets[1].file, "two.adx");
    snprintf(targets[1].name, sizeof targets[1].name, "Two (Remix)");
    targets[1].entry = 1;
    CHECK(find_target("one.adx") == 0);
    CHECK(find_target("ONE.ADX") == 0);
    CHECK(find_target("two.adx") == 1);
    CHECK(find_target("nope.adx") == -1);
    target_count = 0u;
}

/* Synthetic discs below write their own tables; nothing here is a retail name. */
enum {
    AFS_ENTRY = 0x800, AFS_ENTRY_SIZE = 0x800, AFS_NAMES_AT = 0x2000, ARCHIVE_SIZE = AFS_NAMES_AT + 48 * 8,
    XBE_SIZE = 0x3000, XBE_HEADERS_AT = 0x200, XBE_DATA = 0x400,
    XBE_TABLE_AT = 0x1000, XBE_LABELS_AT = 0x2000, XBE_NAMES_AT = 0x2800,
    NAME_BYTES = 64, RATE = 22050, SECONDS = 22050,
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* The 0x20-byte ADX header of an entry: one channel, RATE, a given length. */
static void put_adx(uint8_t *entry, uint32_t samples)
{
    entry[0] = 0x80u; entry[1] = 0x00u; entry[2] = 0x20u;
    entry[4] = 3u; entry[5] = 0x12u; entry[6] = 4u; entry[7] = 1u;
    put_be32(entry + 8, RATE);
    put_be32(entry + 12, samples);
}

/* A synthetic bgm.afs: an entry per name, an ADX of the given samples long. */
static void make_archive(const char *path, const char *const names[], const uint32_t samples[], unsigned count)
{
    uint8_t afs[AFS_NAMES_AT + 48u * 8u];
    memset(afs, 0, sizeof afs);
    memcpy(afs, "AFS\0", 4u);
    put32(afs + 4, count);
    for (unsigned i = 0u; i < count; ++i) {
        put32(afs + 8u + 8u * i, AFS_ENTRY + AFS_ENTRY_SIZE * i);
        put32(afs + 12u + 8u * i, AFS_ENTRY_SIZE);
        memset(afs + AFS_ENTRY + AFS_ENTRY_SIZE * i, 'a', AFS_ENTRY_SIZE);
        put_adx(afs + AFS_ENTRY + AFS_ENTRY_SIZE * i, samples[i]);
        snprintf((char *)afs + AFS_NAMES_AT + 48u * i, 48u, "%s", names[i]);
    }
    put32(afs + 8u + 8u * count, AFS_NAMES_AT);
    put32(afs + 12u + 8u * count, 48u * count);
    write_file(path, afs, sizeof afs);
}

/* A synthetic executable whose data section holds a music table at table_va:
   DOA2.xbe's entries are label, unused, file, unused; Doa1.xbe's are a file
   pointer each. */
static void make_executable(const char *path, uint32_t table_va, bool labelled,
    const char *const labels[], const char *const files[], unsigned count)
{
    uint8_t xbe[XBE_SIZE];
    memset(xbe, 0, sizeof xbe);
    const uint32_t base = table_va - XBE_TABLE_AT;
    memcpy(xbe, "XBEH", 4u);
    put32(xbe + 0x104, base);
    put32(xbe + 0x11c, 2u);
    put32(xbe + 0x120, base + XBE_HEADERS_AT);
    put32(xbe + XBE_HEADERS_AT + 4u, base);
    put32(xbe + XBE_HEADERS_AT + 8u, XBE_DATA);
    put32(xbe + XBE_HEADERS_AT + 16u, XBE_DATA);
    put32(xbe + XBE_HEADERS_AT + 60u, base + XBE_DATA);
    put32(xbe + XBE_HEADERS_AT + 64u, sizeof xbe - XBE_DATA);
    put32(xbe + XBE_HEADERS_AT + 68u, XBE_DATA);
    put32(xbe + XBE_HEADERS_AT + 72u, sizeof xbe - XBE_DATA);
    for (unsigned i = 0u; i < count; ++i) {
        const uint32_t name = base + XBE_NAMES_AT + NAME_BYTES * i;
        snprintf((char *)xbe + XBE_NAMES_AT + NAME_BYTES * i, NAME_BYTES, "%s", files[i]);
        if (!labelled) {
            put32(xbe + XBE_TABLE_AT + 4u * i, name);
            continue;
        }
        put32(xbe + XBE_TABLE_AT + 16u * i, base + XBE_LABELS_AT + NAME_BYTES * i);
        put32(xbe + XBE_TABLE_AT + 16u * i + 8u, name);
        snprintf((char *)xbe + XBE_LABELS_AT + NAME_BYTES * i, NAME_BYTES, "%s", labels[i]);
    }
    write_file(path, xbe, sizeof xbe);
}

/* The music table gives the album its order and labels; its unused slot (a
   one-sample placeholder) is left out, and a file it names twice is one
   track named for both. */
static void test_music_table(const char *base)
{
    static const char *const names[] = {"one.adx", "two.adx", "unused.adx"};
    static const uint32_t samples[] = {SECONDS * 2u, SECONDS, 1u};
    static const char *const labels[] = {"Remix_Ky", "End_Ky", "Hidden", "End_Other", "NameEntry"};
    static const char *const files[] = {"one.adx", "two.adx", "unused.adx", "two.adx", "absent.adx"};
    char disc[512], path[512], text[4096];
    make_folder(base);
    snprintf(disc, sizeof disc, "%s/table-disc", base);
    make_folder(disc);
    snprintf(path, sizeof path, "%s/DOA2", disc);
    make_folder(path);
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc);
    make_archive(path, names, samples, 3u);
    snprintf(path, sizeof path, "%s/DOA2.xbe", disc);
    make_executable(path, DOA2U_MUSIC, true, labels, files, 5u);
    snprintf(disc_root, sizeof disc_root, "%s", disc);
    snprintf(jukebox_root, sizeof jukebox_root, "%s/jukebox", base);

    Album album = {0};
    load_doa2u(&album);
    CHECK(album.count == 2u);
    if (album.count == 2u) {
        CHECK(strcmp(album.tracks[0].file, "one.adx") == 0);
        CHECK(strcmp(album.tracks[0].name, "Ky (Remix)") == 0);
        CHECK(album.tracks[0].offset == AFS_ENTRY && album.tracks[0].size == AFS_ENTRY_SIZE);
        CHECK(strcmp(album.tracks[1].file, "two.adx") == 0);
        CHECK(strcmp(album.tracks[1].name, "Ky's Ending / Other's Ending") == 0);
        CHECK(album.tracks[1].offset == AFS_ENTRY + AFS_ENTRY_SIZE);
    }
    snprintf(path, sizeof path, "%s/doa2u/names.txt", jukebox_root);
    (void)read_text(path, text, sizeof text);
    CHECK(strstr(text, "one.adx = Ky (Remix)") != NULL);
    CHECK(strstr(text, "two.adx = Ky's Ending / Other's Ending") != NULL);
    free(album.tracks);

    /* Without the executable the album is empty rather than made up. */
    snprintf(path, sizeof path, "%s/DOA2.xbe", disc);
    remove(path);
    Album missing = {0};
    load_doa2u(&missing);
    CHECK(missing.count == 0u);
    free(missing.tracks);
}

/* Doa1.xbe's music table lists 0x2e02f8's files in its order; the names are
   the shared fighter codes, and a file that is not ADX is left out. */
static void test_doa1u_table(const char *base)
{
    static const char *const files[] = {"aa_sng.adx", "bb_lose.adx", "cc_sng.adx"};
    static const char *const missing[] = {"aa_sng.adx"};
    char disc[512], path[512], sound[512], file[32], name[NAME_SIZE];
    make_folder(base);
    snprintf(disc, sizeof disc, "%s/doa1-disc", base);
    make_folder(disc);
    snprintf(sound, sizeof sound, "%s/DOA1", disc);
    make_folder(sound);
    snprintf(sound, sizeof sound, "%s/DOA1/Sound", disc);
    make_folder(sound);
    for (unsigned i = 0u; i < 2u; ++i) {
        uint8_t adx[64] = {0};
        put_adx(adx, SECONDS);
        snprintf(path, sizeof path, "%s/%s", sound, files[i]);
        write_file(path, adx, sizeof adx);
    }
    snprintf(path, sizeof path, "%s/%s", sound, files[2]);
    write_file(path, "no", 2u);
    snprintf(path, sizeof path, "%s/Doa1.xbe", disc);
    make_executable(path, DOA1U_MUSIC, false, NULL, files, 3u);
    snprintf(disc_root, sizeof disc_root, "%s", disc);
    snprintf(jukebox_root, sizeof jukebox_root, "%s/jukebox", base);

    Album album = {0};
    load_doa1u(&album);
    CHECK(album.count == 2u);
    if (album.count == 2u) {
        CHECK(strcmp(album.tracks[0].file, files[0]) == 0);
        CHECK(strcmp(album.tracks[0].name, "Aa Sng") == 0);
        CHECK(strcmp(album.tracks[1].file, files[1]) == 0);
        CHECK(strcmp(album.tracks[1].name, "Bb Lose") == 0);
    }
    free(album.tracks);

    /* The label follows the code the table's names use. */
    snprintf(file, sizeof file, "%s_lose2.adx", DOA1U_CODES[0].code);
    doa1u_label(file, name, sizeof name);
    CHECK(strstr(name, "(Lose 2)") != NULL);

    /* A table with no strings ends; the album is then empty. */
    snprintf(path, sizeof path, "%s/Doa1.xbe", disc);
    make_executable(path, DOA1U_MUSIC, false, NULL, missing, 0u);
    Album empty = {0};
    load_doa1u(&empty);
    CHECK(empty.count == 0u);
    free(empty.tracks);
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
    char disc[512], mods[512], private_dir[512], path[512], text[2048];
    memset(huge_data, 'Z', sizeof huge_data);
    make_folder(base);
    snprintf(disc, sizeof disc, "%s/disc", base);
    make_folder(disc);
    snprintf(path, sizeof path, "%s/DOA2", disc);
    make_folder(path);
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc);
    {
        static const char *const names[] = {"one.adx", "two.adx"};
        static const uint32_t samples[] = {SECONDS, SECONDS};
        make_archive(path, names, samples, 2u);
    }
    snprintf(path, sizeof path, "%s/DOA2.xbe", disc);
    {
        static const char *const labels[] = {"First Theme", "Second Theme"};
        static const char *const files[] = {"one.adx", "two.adx"};
        make_executable(path, DOA2U_MUSIC, true, labels, files, 2u);
    }
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
    CHECK(target_count == 2u);
    if (target_count == 2u) {
        CHECK(strcmp(targets[0].file, "one.adx") == 0);
        CHECK(strcmp(targets[0].name, "First Theme") == 0);
        CHECK(strcmp(targets[1].file, "two.adx") == 0);
        CHECK(strcmp(targets[1].name, "Second Theme") == 0);
    }

    const RecompModOverlay *overlay = recomp_mod_overlay(BGM, ARCHIVE_SIZE);
    uint64_t at = 0u, second_at = 0u;
    CHECK(overlay != NULL && recomp_mod_entry_offset(BGM, ARCHIVE_SIZE, 0u, &at) &&
        recomp_mod_entry_offset(BGM, ARCHIVE_SIZE, 1u, &second_at));
    snprintf(path, sizeof path, "%s/DOA2/bgm.afs", disc);
    base_file = fopen(path, "rb");
    CHECK(base_file != NULL);
    if (overlay == NULL || base_file == NULL) return;
    CHECK(entry_byte(overlay, at) == 0x80u);

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
        /* Going online mid-run, the open archive plays the entry's own track,
           and going offline again brings the assignment back. */
        recomp_jukebox_set_online(true);
        CHECK(!assigning && entry_byte(overlay, at) == 0x80u);
        recomp_jukebox_set_online(false);
        CHECK(assigning && entry_byte(overlay, at) == 'Q');
        /* Online, the page plays nothing over the game. */
        recomp_jukebox_set_online(true);
        recomp_jukebox_confirm(ROW_TRACK);
        CHECK(!player.playing && strstr(page.note, "online") != NULL);
        recomp_jukebox_set_online(false);
        /* A boot installed online stays so: its logoff brings no slots back. */
        recomp_jukebox_install(true);
        recomp_jukebox_set_online(false);
        CHECK(!assigning && entry_byte(overlay, at) == 0x80u);
        recomp_jukebox_install(false);
        CHECK(assigning && entry_byte(overlay, at) == 'Q');
        snprintf(path, sizeof path, "%s/assignments.txt", jukebox_root);
        (void)read_text(path, text, sizeof text);
        CHECK(strstr(text, "one.adx\tDOA3\ttrack.adx") != NULL);

        /* The page reload keeps an assignment that still resolves. */
        recomp_jukebox_page_opened();
        CHECK(targets[0].album == doa3 && targets[0].track == track);
        CHECK(entry_byte(overlay, at) == 'Q');

        /* The saved names match in any case. */
        static const char upper[] = "ONE.ADX\tdoa3\tTRACK.ADX\n";
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

        /* A save keeps the lines it cannot place: an album that is missing for
           now, a music entry this build does not list, and a player's comment. */
        static const char held[] =
            "# The Jukebox's assignments: the game's music<TAB>album<TAB>track file. Set in game.\n"
            "# A comment the player keeps.\n"
            "one.adx\tDOA4\tkept.adx\nghost.adx\tDOA3\ttrack.adx\n";
        write_file(path, held, sizeof held - 1u);
        recomp_jukebox_page_opened();
        CHECK(targets[0].album == -1 && targets[0].track == -1);
        page.album = (unsigned)doa3;
        page.track = (unsigned)track;
        page.target = 1u;
        recomp_jukebox_confirm(ROW_TARGET);
        CHECK(targets[1].album == doa3 && targets[1].track == track);
        CHECK(entry_byte(overlay, second_at) == 'Q');
        (void)read_text(path, text, sizeof text);
        CHECK(strstr(text, "two.adx\tDOA3\ttrack.adx") != NULL);
        CHECK(strstr(text, "one.adx\tDOA4\tkept.adx") != NULL);
        CHECK(strstr(text, "ghost.adx\tDOA3\ttrack.adx") != NULL);
        CHECK(strstr(text, "# A comment the player keeps.") != NULL);
        /* The save's own header is written once, not kept and written again. */
        CHECK(count_text(text, "# The Jukebox's assignments:") == 1u);
        CHECK(count_text(text, "one.adx") == 1u);

        /* A comment longer than the reader's 512-byte buffer: its tail looks
           like an assignment, but it stays one comment line and assigns nothing. */
        static char long_comment[640];
        memset(long_comment, 'x', 511u);
        long_comment[0] = '#';
        snprintf(long_comment + 511, sizeof long_comment - 511u, "two.adx\tDOA3\ttrack.adx\n");
        write_file(path, long_comment, strlen(long_comment));
        recomp_jukebox_page_opened();
        CHECK(targets[1].album == -1 && targets[1].track == -1);
        page.target = 0u;
        recomp_jukebox_confirm(ROW_TARGET);
        (void)read_text(path, text, sizeof text);
        CHECK(strstr(text, long_comment) != NULL);

        /* A line of the player's longer than that buffer, and no assignment,
           is kept whole too; a last line without a newline gets one. */
        static char player_text[720], expected[720];
        memset(player_text, 'y', 600u);
        memcpy(player_text + 600, "\n# last, no newline", 19u);
        write_file(path, player_text, 619u);
        page.target = 0u;
        recomp_jukebox_confirm(ROW_TARGET);
        (void)read_text(path, text, sizeof text);
        memset(expected, 'y', 600u);
        memcpy(expected + 600, "\n# last, no newline\n", 20u);
        CHECK(strstr(text, expected) != NULL);

        /* A target the reloaded file no longer names plays its own music. */
        remove(path);
        recomp_jukebox_page_opened();
        CHECK(targets[0].album == -1 && targets[0].track == -1);
        CHECK(targets[1].album == -1 && targets[1].track == -1);
        CHECK(entry_byte(overlay, at) == 0x80u);
        CHECK(entry_byte(overlay, second_at) == 0x80u);
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
    test_music_table("jukebox-test-table");
    test_doa1u_table("jukebox-test-doa1");
    test_assignments(root);
    return failures != 0u;
}
