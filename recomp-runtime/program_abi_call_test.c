/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recomp_types.h"
#include <stdio.h>

static unsigned manual_calls, generated_calls, lookups;
static bool bound;
static bool reset_during_lookup;
uint32_t recomp_program_lookup_generation = 1u;

static void manual_body(void) { ++manual_calls; }
static void generated_body(void) { ++generated_calls; }

RecompFunction recomp_lookup_manual(uint32_t address)
{
    ++lookups;
    RecompFunction function = bound && address == 0x1000u ? manual_body : NULL;
    if (reset_during_lookup) ++recomp_program_lookup_generation;
    return function;
}

int main(void)
{
    bound = true;
    RECOMP_ABI_CALL(0x1000u, generated_body);
    bound = false;
    RECOMP_ABI_CALL(0x1000u, generated_body);
    bound = true;
    RECOMP_ABI_CALL(0x1004u, generated_body);
    if (lookups != 3u || manual_calls != 1u || generated_calls != 2u) {
        fprintf(stderr, "ABI call: manual=%u generated=%u lookups=%u\n",
            manual_calls, generated_calls, lookups);
        return 1;
    }
    /* One site asks once per boot; a reboot's new generation asks again. */
    lookups = manual_calls = generated_calls = 0u;
    for (unsigned boot = 0u; boot < 2u; ++boot) {
        bound = boot == 1u;
        for (unsigned call = 0u; call < 3u; ++call) RECOMP_ABI_CALL(0x1000u, generated_body);
        ++recomp_program_lookup_generation;
    }
    if (lookups != 2u || manual_calls != 3u || generated_calls != 3u) {
        fprintf(stderr, "ABI call site: manual=%u generated=%u lookups=%u\n",
            manual_calls, generated_calls, lookups);
        return 1;
    }
    /* A reset inside the lookup must leave the site on the old generation. */
    lookups = manual_calls = generated_calls = 0u;
    bound = reset_during_lookup = true;
    for (unsigned call = 0u; call < 3u; ++call) {
        RECOMP_ABI_CALL(0x1000u, generated_body);
        bound = reset_during_lookup = false;
    }
    if (lookups != 2u || manual_calls != 1u || generated_calls != 2u) {
        fprintf(stderr, "ABI call reset: manual=%u generated=%u lookups=%u\n",
            manual_calls, generated_calls, lookups);
        return 1;
    }
    return 0;
}
