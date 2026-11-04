/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_compat.h"
#endif

static int check(const char *name, bool ok)
{
    if (!ok) fprintf(stderr, "kernel file: %s\n", name);
    return ok ? 1 : 0;
}

/* A rewind past a file close cannot bring the host handle back; the restore
   has to say so once, and a clean restore must not. */
int recomp_kernel_file_test(void)
{
    char narrow[MAX_PATH + 32];
    wchar_t path[MAX_PATH + 32];
    if (GetTempPathA(MAX_PATH, narrow) == 0u) return check("temp path", false);
    strcat(narrow, "recomp_kernel_file_test.bin");
    FILE *file = fopen(narrow, "wb");
    if (file == NULL || fputs("data", file) < 0 || fclose(file) != 0)
        return check("temp file", false);
    if (mbstowcs(path, narrow, sizeof path / sizeof path[0]) == (size_t)-1)
        return check("wide path", false);

    recomp_kernel_file_reset();
    void *empty = calloc(1u, recomp_kernel_file_state_size());
    void *open = calloc(1u, recomp_kernel_file_state_size());
    int passed = check("state buffers", empty != NULL && open != NULL);
    if (!passed) goto done;
    recomp_kernel_file_state_save(empty);
    const uint32_t kept = recomp_kernel_open_readonly(path, NULL);
    passed &= check("open", kept != 0u);
    recomp_kernel_file_state_save(open);

    /* A handle opened after the save is dropped; nothing is lost. */
    const uint32_t later = recomp_kernel_open_readonly(path, NULL);
    passed &= check("second open", later != 0u);
    passed &= check("open state not restorable", recomp_kernel_file_state_restorable(open));
    recomp_kernel_file_state_restore(open);
    passed &= check("clean restore reports lost", !recomp_kernel_file_state_lost());
    passed &= check("later handle survived restore",
        recomp_kernel_close_file(later, 0u) != 0u);

    passed &= check("close", recomp_kernel_close_file(kept, 0u) == 0u);
    passed &= check("closed state restorable", !recomp_kernel_file_state_restorable(open));
    passed &= check("empty state not restorable", recomp_kernel_file_state_restorable(empty));
    recomp_kernel_file_state_restore(open);
    passed &= check("closed handle not reported", recomp_kernel_file_state_lost());
    passed &= check("lost reported twice", !recomp_kernel_file_state_lost());

    /* Each restore starts clean: an unread flag does not leak into the next. */
    recomp_kernel_file_state_restore(open);
    recomp_kernel_file_state_restore(empty);
    passed &= check("flag survived a clean restore", !recomp_kernel_file_state_lost());

done:
    free(empty);
    free(open);
    recomp_kernel_file_reset();
    remove(narrow);
    return passed;
}
