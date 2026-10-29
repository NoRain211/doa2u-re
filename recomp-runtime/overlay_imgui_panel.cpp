/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "overlay_imgui_panel.h"

#include "imgui.h"

namespace {

/* Text is sized for the target's height, as the game's own is: ImGui's
   13-pixel font at kReferenceHeight lines, built once at that size. */
constexpr float kReferenceHeight = 480.0f;
constexpr float kFontPixels = 13.0f;
float panel_scale = 1.0f;

}  // namespace

void recomp_overlay_imgui_style(float target_height)
{
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    panel_scale = target_height > kReferenceHeight ? target_height / kReferenceHeight : 1.0f;
    ImFontConfig font{};
    font.SizePixels = kFontPixels * panel_scale;
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.06f, 0.10f, 0.70f);
    style.ScaleAllSizes(panel_scale);
}

/* A small box under the life bars, centred: the title, then one line per
   entry (one per player for the suit switch). */
void recomp_overlay_imgui_build(const RecompOverlayText &text, float width, float height)
{
    if (text.title[0] == '\0' && text.count == 0u) return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.17f), 0, ImVec2(0.5f, 0.0f));
    /* Fixed width, wide enough for the longest suit line, so the box does
       not change size while scrolling. */
    const float fixed = ImGui::CalcTextSize("P1  307 / 307  Carnation (skirt)  ").x +
        ImGui::GetStyle().WindowPadding.x * 2.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(fixed, 0.0f), ImVec2(fixed, height));
    ImGui::Begin("recomp-overlay-panel", nullptr, flags);
    if (text.title[0] != '\0') ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.35f, 1.0f), "%s", text.title);
    for (unsigned i = 0u; i < text.count; ++i) ImGui::TextUnformatted(text.line[i]);
    ImGui::End();
    /* The hint: the controls, along the bottom. */
    if (text.hint[0] == '\0') return;
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, height * 0.97f), 0, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(width * 0.96f, height));
    ImGui::Begin("recomp-overlay-hint", nullptr, flags);
    ImGui::PushTextWrapPos(width * 0.94f);
    ImGui::TextUnformatted(text.hint);
    ImGui::PopTextWrapPos();
    ImGui::End();
}
