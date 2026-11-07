/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_STAGE_SELECT_H
#define DOA2U_RECOMP_STAGE_SELECT_H

#include "runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage Select's row builders and lookups with Tengu's stage added (issue
   #117), or NULL for other addresses. */
RecompFunction recomp_stage_select_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
