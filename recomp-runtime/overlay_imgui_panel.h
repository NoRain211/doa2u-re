/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_OVERLAY_IMGUI_PANEL_H
#define DOA2U_RECOMP_OVERLAY_IMGUI_PANEL_H

#include "overlay_text.h"

/* The overlay's look and layout in Dear ImGui: call the style once after
   creating the context, with the height of the target it will draw to, and
   the build between ImGui::NewFrame and ImGui::Render. */
void recomp_overlay_imgui_style(float target_height);
void recomp_overlay_imgui_build(const RecompOverlayText &text, float width, float height);

#endif
