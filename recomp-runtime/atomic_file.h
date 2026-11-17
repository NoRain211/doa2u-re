/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_ATOMIC_FILE_H
#define DOA2U_ATOMIC_FILE_H

#include <stdbool.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

/* Keep the old file until a complete, flushed replacement is ready. */
static FILE *recomp_atomic_open(const char *path, char *temporary, size_t size)
{
    static unsigned serial;
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    for (unsigned attempt = 0u; attempt < 64u; ++attempt) {
        const int n = snprintf(temporary, size, "%s.tmp.%lu.%u", path, pid, ++serial);
        if (n < 0 || (size_t)n >= size) return NULL;
        FILE *file = fopen(temporary, "wbx");
        if (file != NULL) return file;
    }
    return NULL;
}

static bool recomp_atomic_commit(FILE *file, const char *temporary, const char *path, bool written)
{
    bool ok = written && !ferror(file) && fflush(file) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(file)) == 0;
#else
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (fclose(file) != 0) ok = false;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok) ok = rename(temporary, path) == 0;
#endif
    if (!ok) remove(temporary);
    return ok;
}

#endif
