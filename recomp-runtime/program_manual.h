/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_PROGRAM_MANUAL_H
#define DOA2U_RECOMP_PROGRAM_MANUAL_H

#include "runtime.h"

#include <stdbool.h>

/* --skip-intro: omit the logo movie's playback wait and end the boot opening movie. */
extern bool recomp_skip_intro;

RecompFunction recomp_lookup_manual(uint32_t guest_address);

#endif
