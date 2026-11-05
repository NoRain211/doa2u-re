/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_MWPLY_ADAPTER_H
#define DOA2U_RECOMP_MWPLY_ADAPTER_H

#include "runtime.h"

/* Native replacement for the CRI Sofdec (MWSFD) player calls the game makes. */
RecompFunction recomp_mwply_lookup_manual(uint32_t address);

#endif

