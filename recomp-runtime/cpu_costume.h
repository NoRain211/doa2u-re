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
/* Turns random costumes on or off at once (PC Settings); still off online. */
void recomp_cpu_costume_set_enabled(bool enabled);
bool recomp_cpu_costume_enabled(void);
RecompFunction recomp_cpu_costume_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
