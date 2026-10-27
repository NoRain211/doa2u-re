/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_SAVE_ADAPTER_H
#define DOA2U_RECOMP_SAVE_ADAPTER_H

#include "runtime.h"

/* Brackets the game's save writer with the save journal; see save_adapter.c. */
RecompFunction recomp_save_lookup_manual(uint32_t guest_address);

#endif
