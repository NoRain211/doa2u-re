/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "overlay_d3d11.h"
#include "overlay_imgui_panel.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include <chrono>
#include <cstdio>

namespace {

bool ready;
bool failed;
double last_ms;

double now_ms()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool start(ID3D11Device *device, ID3D11DeviceContext *context, UINT height)
{
    if (ready) return true;
    if (failed) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    recomp_overlay_imgui_style(static_cast<float>(height));
    if (!ImGui_ImplDX11_Init(device, context)) {
        std::fprintf(stderr, "[overlay] ImGui D3D11 backend failed to start\n");
        ImGui::DestroyContext();
        failed = true;
        return false;
    }
    ready = true;
    last_ms = now_ms();
    return true;
}

}  // namespace

void recomp_overlay_d3d11_draw(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11RenderTargetView *target, UINT width, UINT height, const RecompOverlayText *text)
{
    if (device == nullptr || context == nullptr || target == nullptr ||
        width == 0u || height == 0u || recomp_overlay_empty(text)) {
        return;
    }
    if (!start(device, context, height)) return;

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    const double now = now_ms();
    io.DeltaTime = now > last_ms ? static_cast<float>((now - last_ms) / 1000.0) : 1.0f / 60.0f;
    last_ms = now;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    recomp_overlay_imgui_build(*text, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr || draw_data->CmdListsCount == 0) return;

    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->OMSetRenderTargets(1u, &target, nullptr);
    context->RSSetViewports(1u, &viewport);
    ImGui_ImplDX11_RenderDrawData(draw_data);
}

void recomp_overlay_d3d11_shutdown(void)
{
    if (!ready) return;
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    ready = false;
}
