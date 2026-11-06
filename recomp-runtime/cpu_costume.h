/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_CPU_COSTUME_H
#define DOA2U_RECOMP_CPU_COSTUME_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RECOMP_RANDOM_CPU_COSTUMES=1 dresses CPU opponents in random unlocked
   costumes (issue #116). Off online, where both games must agree. */
void recomp_cpu_costume_install(bool online);
RecompFunction recomp_cpu_costume_lookup_manual(uint32_t guest_address);
/* The costume group for one opponent of a run: the same inputs give the same
   group, so every lookup in a run agrees. */
uint8_t recomp_cpu_costume_group(uint32_t seed, const uint8_t *run, uint32_t run_size,
    uint8_t character, uint8_t index, uint8_t groups);

#ifdef __cplusplus
}
#endif

#endif
