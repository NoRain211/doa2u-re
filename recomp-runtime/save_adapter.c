/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "save_adapter.h"
#include "kernel_abi.h"
#include "save_transaction.h"
#include "stop_report.h"

#include <inttypes.h>
#include <stdio.h>

extern RecompFunction recomp_lookup(uint32_t guest_address);

/* 0x219c0 writes one save game: it creates the save (SaveMeta.xbx and
   SaveImage.xbx) and writes its payload, ups.dat for the profile or br.dat
   for a Battle Record replay. cdecl, 11 arguments, returns 0 on success or a
   negative code. The journal commits those files together; a failed write or
   an interrupted process restores the previous save. The callers' own
   XDeleteSaveGame and fallback writes stay outside, as on the console. */
static void save_game_write(void)
{
    const uint32_t owner = recomp_kernel_current_thread_id();
    /* The generated body: calling it cannot reenter this hook. */
    const RecompFunction original = recomp_lookup(0x000219c0u);

    if (original == NULL || !recomp_save_begin(owner)) {
        recomp_stop(2, "save:begin:0x000219c0");
        return;
    }
    original();
    if (!recomp_kernel_save_handles_closed(owner)) {
        recomp_save_note_failure(owner);
    }
    const uint32_t status = recomp_runtime.registers.eax;
    const bool success = status == 0u;
    if (recomp_save_end_recovers(owner, success)) {
        recomp_kernel_release_profile_handles();
    }
    if (!recomp_save_end(owner, success)) {
        /* A rollback leaves the old save and the game reports its own error;
           a commit or rollback that itself failed cannot continue safely. */
        if (!recomp_save_ready()) {
            recomp_stop(2, "save:end:0x000219c0");
            return;
        }
        fprintf(stderr, "recomp save: 0x000219c0 rolled back status=%" PRId32 "\n",
            (int32_t)status);
    }
}

RecompFunction recomp_save_lookup_manual(uint32_t guest_address)
{
    return guest_address == 0x000219c0u ? save_game_write : NULL;
}
