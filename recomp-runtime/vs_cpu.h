/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_VS_CPU_H
#define DOA2U_RECOMP_VS_CPU_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Versus against the CPU with one controller (issue #116; vs_cpu.c). */
RecompFunction recomp_vs_cpu_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
