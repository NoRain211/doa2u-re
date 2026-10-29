/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_D3D8_DRAW_ADAPTER_H
#define DOA2U_RECOMP_D3D8_DRAW_ADAPTER_H

#include "runtime.h"

RecompFunction recomp_d3d8_draw_lookup_manual(uint32_t address);
/* Draws handed to the presenter so far, including the 2D overlay draws the
   free camera's clean view leaves out (scene_freeze_adapter.c). */
uint32_t recomp_d3d8_draw_submitted(void);

#endif
