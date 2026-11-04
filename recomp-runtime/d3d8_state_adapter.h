/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_D3D8_STATE_ADAPTER_H
#define DOA2U_RECOMP_D3D8_STATE_ADAPTER_H

#include "runtime.h"
#include "d3d_render_state_model.h"

RecompFunction recomp_d3d8_state_lookup_manual(uint32_t address);
const RecompD3dRenderStateModel *recomp_d3d8_state_model(void);
void recomp_d3d8_state_reset(void);
/* Replays the SDK CreateDevice render-state defaults into shadow and model. */
void recomp_d3d8_state_write_defaults(bool auto_depth);
bool recomp_d3d8_state_set_simple(uint32_t method, uint32_t value);

#endif
