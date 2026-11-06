/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_OVERLAY_D3D11_H
#define DOA2U_RECOMP_OVERLAY_D3D11_H

#include <d3d11.h>

#include "overlay_text.h"

/* Draws the runtime's overlay text (overlay_text.h) with Dear ImGui onto the
   window's back buffer, after the game's frame. Created on first use;
   nothing is drawn while the overlay is empty. */
void recomp_overlay_d3d11_draw(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *target, UINT width, UINT height, const RecompOverlayText *text);
void recomp_overlay_d3d11_shutdown(void);

#endif
