/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_abi.h"
#include "program_manual.h"
#include "stop_report.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

extern RecompFunction recomp_lookup(uint32_t guest_address);

void recomp_program_missing(uint32_t guest_address)
{
    fprintf(
        stderr,
        "recomp program: missing generated body at 0x%08" PRIx32 "\n",
        guest_address);
    recomp_stop(2, "missing-body:0x%08" PRIx32, guest_address);
}

/* Every indirect guest call resolves here, so cache hits; the chain was about
   5% of the game thread in fights. Manual hooks depend on boot settings (the
   room turns the intro skip on), so a reboot clears the cache. */
static struct { uint32_t address; RecompFunction function; } cache[4096];

void recomp_program_lookup_reset(void)
{
    memset(cache, 0, sizeof cache);
}

RecompFunction recomp_program_lookup(uint32_t guest_address)
{
    const uint32_t slot = (guest_address ^ guest_address >> 12) & 4095u;
    if (cache[slot].function != NULL && cache[slot].address == guest_address)
        return cache[slot].function;

    RecompFunction function = recomp_lookup_manual(guest_address);

    if (function == NULL) {
        function = recomp_lookup(guest_address);
    }
    if (function == NULL) {
        function = recomp_lookup_kernel(guest_address);
    }
    if (function != NULL) {
        cache[slot].address = guest_address;
        cache[slot].function = function;
    }
    return function;
}
