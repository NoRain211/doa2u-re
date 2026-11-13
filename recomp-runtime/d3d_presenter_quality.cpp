/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d_presenter_d3d11.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

/* PC Settings' quality, set from the game thread and applied by the
   presenter after a present (applyQuality). */
struct QualityRequest {
    float scale = 1.0f;
    uint32_t msaa = 1u;
    bool smaa = false;
    unsigned generation = 0u;
};
static std::mutex quality_lock;
static QualityRequest quality_request;

/* The largest sample count up to requested that both the color and the
   depth format support, as createGraphics chooses it. */
UINT supportedSamples(RecompD3dPresenter *presenter, UINT requested)
{
    UINT samples = 1u;
    for (UINT count = 2u; count <= 32u && count <= requested; count *= 2u) {
        UINT quality = 0u, depth_quality = 0u;
        presenter->device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, count, &quality);
        presenter->device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, count, &depth_quality);
        if (quality != 0u && depth_quality != 0u) samples = count;
    }
    return samples;
}

/* A color target of width x height with samples, its render view and a
   single-sample view to sample (the same texture when samples is 1). */
bool createColorTarget(RecompD3dPresenter *presenter, UINT width, UINT height, UINT samples,
    ID3D11RenderTargetView *&render, ID3D11ShaderResourceView *&sample)
{
    render = nullptr;
    sample = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | (samples == 1u ? static_cast<UINT>(D3D11_BIND_SHADER_RESOURCE) : 0u);
    ID3D11Texture2D *texture = nullptr;
    HRESULT result = presenter->device->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(result)) result = presenter->device->CreateRenderTargetView(texture, nullptr, &render);
    if (SUCCEEDED(result) && samples > 1u) {
        releaseCom(texture);
        desc.SampleDesc.Count = 1u;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        result = presenter->device->CreateTexture2D(&desc, nullptr, &texture);
    }
    if (SUCCEEDED(result)) result = presenter->device->CreateShaderResourceView(texture, nullptr, &sample);
    releaseCom(texture);
    if (FAILED(result)) {
        releaseCom(render);
        releaseCom(sample);
        return false;
    }
    return true;
}

/* Draws source over all of target (width x height), filtered, with the VRR
   output's full-screen shader. */
void blitPicture(RecompD3dPresenter *presenter, ID3D11ShaderResourceView *source,
    ID3D11RenderTargetView *target, UINT width, UINT height)
{
    auto *context = presenter->context;
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
        0.0f, 1.0f};
    context->ClearState();
    context->RSSetViewports(1u, &viewport);
    context->OMSetRenderTargets(1u, &target, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->vrr_vs, nullptr, 0u);
    context->PSSetShader(presenter->vrr_ps, nullptr, 0u);
    context->PSSetSamplers(0u, 1u, &presenter->vrr_sampler);
    context->PSSetShaderResources(0u, 1u, &source);
    context->Draw(3u, 0u);
    context->ClearState();
}

/* Applies PC Settings' quality between frames, as the Vulkan renderer does.
   SMAA only switches its pass. A new scale or MSAA rebuilds the main target,
   its depth and output, and every offscreen target sized by them, carrying
   each picture over (rendered pixels have no CPU copy); the back buffer
   copies and SMAA's buffers are made again at the new size when next used,
   and scaled depth targets when next bound. */
void applyQuality(RecompD3dPresenter *presenter)
{
    // An open occlusion count draws into a target of the old size.
    if (presenter->visibility_active != nullptr) return;
    QualityRequest want;
    {
        std::lock_guard<std::mutex> lock(quality_lock);
        if (quality_request.generation == presenter->quality_applied) return;
        want = quality_request;
    }
    const auto now = std::chrono::steady_clock::now();
    if (want.generation == presenter->quality_retry_generation && now < presenter->quality_retry_at) return;
    const bool old_smaa = presenter->smaa;
    presenter->smaa = want.smaa;
    const float scale = std::isfinite(want.scale) ? std::clamp(want.scale, 1.0f, 8.0f) : 1.0f;
    const UINT msaa = supportedSamples(presenter, std::clamp(want.msaa, 1u, 32u));
    if (scale == presenter->scale && msaa == presenter->msaa) {
        presenter->quality_applied = want.generation;
        return;
    }

    auto *context = presenter->context;
    context->ClearState();
    // The current picture, single-sampled, in the old output texture.
    {
        ID3D11Resource *picture = nullptr, *output = nullptr;
        presenter->render_target_view->GetResource(&picture);
        presenter->vrr_source->GetResource(&output);
        if (presenter->msaa > 1u) context->ResolveSubresource(output, 0u, picture, 0u, DXGI_FORMAT_B8G8R8A8_UNORM);
        else context->CopyResource(output, picture);
        releaseCom(output);
        releaseCom(picture);
    }
    const float old_scale = presenter->scale;
    const UINT old_msaa = presenter->msaa;
    presenter->scale = scale;
    presenter->msaa = msaa;
    const UINT width = mainWidth(presenter), height = mainHeight(presenter);

    // The main target, its output and depth.
    ID3D11RenderTargetView *main_render = nullptr, *output_render = nullptr;
    ID3D11ShaderResourceView *main_sample = nullptr, *output_sample = nullptr;
    ID3D11Texture2D *depth_texture = nullptr;
    ID3D11DepthStencilView *depth_view = nullptr;
    bool created = createColorTarget(presenter, width, height, msaa, main_render, main_sample) &&
        createColorTarget(presenter, width, height, 1u, output_render, output_sample);
    if (created) {
        D3D11_TEXTURE2D_DESC depth_desc{};
        depth_desc.Width = width;
        depth_desc.Height = height;
        depth_desc.MipLevels = depth_desc.ArraySize = 1u;
        depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count = msaa;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        created = SUCCEEDED(presenter->device->CreateTexture2D(&depth_desc, nullptr, &depth_texture)) &&
            SUCCEEDED(presenter->device->CreateDepthStencilView(depth_texture, nullptr, &depth_view));
    }
    releaseCom(main_sample); // the main target is sampled through its copies
    if (!created) {
        std::fprintf(stderr, "recomp d3d presenter: quality %ux%u msaa=%u unavailable\n", width, height, msaa);
        releaseCom(main_render);
        releaseCom(output_render);
        releaseCom(output_sample);
        releaseCom(depth_view);
        releaseCom(depth_texture);
        presenter->scale = old_scale;
        presenter->msaa = old_msaa;
        presenter->smaa = old_smaa;
        presenter->quality_retry_generation = want.generation;
        presenter->quality_retry_at = now + std::chrono::seconds(1);
    } else {
        blitPicture(presenter, presenter->vrr_source, main_render, width, height);
        context->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0u);
        releaseCom(presenter->render_target_view);
        releaseCom(presenter->vrr_target_view);
        releaseCom(presenter->vrr_source);
        releaseCom(presenter->depth_view);
        releaseCom(presenter->depth_texture);
        presenter->render_target_view = main_render;
        presenter->vrr_target_view = output_render;
        presenter->vrr_source = output_sample;
        presenter->depth_texture = depth_texture;
        presenter->depth_view = depth_view;

        // Main-sized and scaled offscreen targets, as bindTarget sizes them.
        for (RenderTargetEntry &entry : presenter->render_targets) {
            const bool host_main = entry.desc.width == presenter->config.width &&
                entry.desc.height == presenter->config.height;
            if (!host_main && !entry.scaled) continue;
            if (entry.multisampled && !entry.resolved) {
                ID3D11Resource *rendered = nullptr, *resolved = nullptr;
                entry.render_view->GetResource(&rendered);
                entry.sample_view->GetResource(&resolved);
                context->ResolveSubresource(resolved, 0u, rendered, 0u, DXGI_FORMAT_B8G8R8A8_UNORM);
                releaseCom(rendered);
                releaseCom(resolved);
            }
            const UINT host_width = host_main ? width : scaledSize(presenter, entry.desc.width);
            const UINT host_height = host_main ? height : scaledSize(presenter, entry.desc.height);
            const UINT samples = host_main ? msaa : 1u;
            ID3D11RenderTargetView *render = nullptr;
            ID3D11ShaderResourceView *sample = nullptr;
            if (!createColorTarget(presenter, host_width, host_height, samples, render, sample))
                continue; // keeps its old size
            blitPicture(presenter, entry.sample_view, render, host_width, host_height);
            releaseCom(entry.render_view);
            releaseCom(entry.sample_view);
            entry.render_view = render;
            entry.sample_view = sample;
            entry.multisampled = samples > 1u;
            entry.resolved = false;
            entry.host_width = host_width;
            entry.host_height = host_height;
        }
        // Depths sized by the scale or samples are made again when bound.
        for (auto it = presenter->depth_targets.begin(); it != presenter->depth_targets.end();) {
            if (it->host_main || it->host_width != it->desc.width) {
                releaseCom(it->view);
                presenter->target_bytes -= it->bytes;
                it = presenter->depth_targets.erase(it);
            } else {
                ++it;
            }
        }
        releaseCom(presenter->back_buffer_sample);
        releaseCom(presenter->back_buffer_copy);
        const bool had_previous = presenter->previous_frame != nullptr;
        releaseCom(presenter->previous_frame_sample);
        releaseCom(presenter->previous_frame);
        if (had_previous) {
            RecompD3dTextureDesc desc{};
            desc.format_byte = 0x12u;
            desc.linear = true;
            desc.width = presenter->config.width;
            desc.height = presenter->config.height;
            lookupBackBufferTexture(presenter, desc, true); // remade from this picture
        }
        releaseSmaa(presenter);
        // A 480-line picture is windowed and a scaled one fills the screen, as at start.
        if ((old_scale > 1.0f) != (scale > 1.0f) && presenter->fullscreen != (scale > 1.0f))
            toggleFullscreen(presenter);
        presenter->quality_applied = want.generation;
    }
    context->ClearState();
    context->OMSetRenderTargets(1u, &presenter->render_target_view, presenter->depth_view);
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(mainWidth(presenter)),
        static_cast<float>(mainHeight(presenter)), 0.0f, 1.0f};
    context->RSSetViewports(1u, &viewport);
    presenter->target_scale_x = viewport.Width / presenter->config.width;
    presenter->target_scale_y = viewport.Height / presenter->config.height;
    presenter->target_samples = presenter->msaa;
    std::fprintf(stderr, "recomp d3d presenter: quality target=%ux%u msaa=%u smaa=%d\n",
        mainWidth(presenter), mainHeight(presenter), presenter->msaa, presenter->smaa ? 1 : 0);
}

void d3d_backend_set_quality(float scale, uint32_t msaa, bool smaa)
{
    std::lock_guard<std::mutex> lock(quality_lock);
    quality_request.scale = scale;
    quality_request.msaa = msaa;
    quality_request.smaa = smaa;
    ++quality_request.generation;
    std::fprintf(stderr, "recomp d3d presenter: quality requested scale=%.3f msaa=%u smaa=%d\n",
        scale, msaa, smaa ? 1 : 0);
}

bool d3d_backend_quality_live()
{
    return true;
}
