/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic test for live_identity.c: the secret file keeps the same account
   across launches, and a name left in the storage folder is never written
   through. A run killed between its create and its rename leaves
   live-identity.bin.tmp behind, and a program on the machine can plant a link
   there; neither may keep Live unavailable or hand the secret to the target. */

#include "live_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

/* A file's size, or -1 when no file is reached by that name. */
static long file_size(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    const long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    fclose(file);
    return size;
}

static bool write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    const bool written = fputs(text, file) >= 0;
    return fclose(file) == 0 && written;
}

/* Reads up to capacity - 1 bytes, zero padding the rest; -1 when no file is
   reached by that name. */
static long read_file(const char *path, char *text, size_t capacity)
{
    memset(text, 0, capacity);
    FILE *file = fopen(path, "rb");
    if (file == NULL) return -1;
    const size_t got = fread(text, 1, capacity - 1u, file);
    fclose(file);
    return (long)got;
}

static bool make_folder(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static void remove_folder(const char *path)
{
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

/* A link at the temporary name, as another program could leave one. False when
   this host may not make one: Windows needs the privilege or developer mode. */
static bool plant_link(const char *path, const char *target)
{
    remove(path);
#ifdef _WIN32
    return CreateSymbolicLinkA(path, target, 0x2u) != 0;
#else
    return symlink(target, path) == 0;
#endif
}

/* A storage folder of this test's own, so a parallel run does not share it. */
static bool make_storage(char *path, size_t capacity)
{
#ifdef _WIN32
    char directory[MAX_PATH];
    if (GetTempPathA(MAX_PATH, directory) == 0u) return false;
    const int needed = snprintf(path, capacity, "%srecomp-live-identity-%lu",
        directory, (unsigned long)GetCurrentProcessId());
#else
    const char *directory = getenv("TMPDIR");
    if (directory == NULL || directory[0] == '\0') directory = "/tmp";
    const int needed = snprintf(path, capacity, "%s/recomp-live-identity-%ld",
        directory, (long)getpid());
#endif
    if (needed < 0 || (size_t)needed >= capacity) return false;
    return make_folder(path);
}

int main(void)
{
    enum { PATH_BYTES = 1200 };
    char storage[PATH_BYTES], identity_path[PATH_BYTES], temporary[PATH_BYTES], victim[PATH_BYTES];
    if (!make_storage(storage, sizeof storage)) {
        fprintf(stderr, "FAIL live identity storage folder\n");
        return 1;
    }
    snprintf(identity_path, sizeof identity_path, "%s/%s", storage, "live-identity.bin");
    snprintf(temporary, sizeof temporary, "%s.tmp", identity_path);
    snprintf(victim, sizeof victim, "%s/%s", storage, "victim");

    /* A leftover temporary from a killed run: the next launch still makes the
       account, and the stale name is gone afterwards. */
    check("stale temporary", write_file(temporary, "cut short"));
    RecompLiveIdentity identity;
    check("first use after a stale temporary", recomp_live_identity_load(storage, &identity));
    check("the secret is written", file_size(identity_path) == RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    check("the stale temporary is gone", file_size(temporary) < 0);
    check("the account has a type-9 XUID", (identity.xuid >> 48) == RECOMP_LIVE_XUID_TYPE);
    char stored[RECOMP_LIVE_IDENTITY_SECRET_BYTES + 1u];
    check("the file holds the secret", read_file(identity_path, stored, sizeof stored) ==
        RECOMP_LIVE_IDENTITY_SECRET_BYTES &&
        memcmp(stored, identity.secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES) == 0);

    /* The same file gives the same account on the next launch. */
    RecompLiveIdentity again;
    check("second use", recomp_live_identity_load(storage, &again));
    check("the same account", again.xuid == identity.xuid &&
        memcmp(again.proof, identity.proof, sizeof again.proof) == 0);

    /* A planted link is dropped as a name: the file it points at keeps its
       bytes, and the identity is made beside it. */
    check("victim", write_file(victim, "outside data"));
    remove(identity_path);
    if (plant_link(temporary, victim)) {
        RecompLiveIdentity through;
        check("first use beside a planted link", recomp_live_identity_load(storage, &through));
        char text[64];
        check("the link's target is untouched", read_file(victim, text, sizeof text) >= 0 &&
            strcmp(text, "outside data") == 0);
        check("the planted link is gone", file_size(temporary) < 0);
        check("the identity is made beside the link",
            file_size(identity_path) == RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    } else {
        fprintf(stderr, "skip planted link: this host cannot make one\n");
    }

    remove(identity_path);
    remove(temporary);
    remove(victim);
    remove_folder(storage);
    if (failures != 0) return 1;
    puts("PASS live identity keeps its account and clears a stale or planted temporary");
    return 0;
}
