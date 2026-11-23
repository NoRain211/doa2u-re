// SPDX-License-Identifier: GPL-3.0-or-later
/* The SDL_GPU presenter's ambient occlusion passes, included inside its
   anonymous namespace after the helpers they reuse (d3d_ao.h has the HLSL). */
#ifndef DOA2U_RECOMP_D3D_AO_SDL_H
#define DOA2U_RECOMP_D3D_AO_SDL_H

/* Ambient occlusion (d3d_ao.h) matches the D3D11 presenter's three passes. Its
   depth input must be a single-sample texture, which SDL_GPU cannot make from
   a multisampled depth target, so AO runs only while MSAA is off here. */
SDL_GPUGraphicsPipeline *createAoPipeline(RecompD3dPresenter *presenter, const std::string &source,
    uint32_t samplers, SDL_GPUTextureFormat format, bool blend)
{
    SDL_GPUShader *vertex = createShader(presenter, source, true, {}, 0u, 0u);
    SDL_GPUShader *fragment = vertex != nullptr ? createShader(presenter, source, false, {}, samplers, 1u) : nullptr;
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    if (fragment != nullptr) {
        SDL_GPUColorTargetDescription color{};
        color.format = format;
        if (blend) {
            color.blend_state.enable_blend = true;
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR;  // dst * occlusion
            color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1u;
        pipeline = SDL_CreateGPUGraphicsPipeline(presenter->device, &info);
    }
    if (vertex != nullptr) SDL_ReleaseGPUShader(presenter->device, vertex);
    if (fragment != nullptr) SDL_ReleaseGPUShader(presenter->device, fragment);
    return pipeline;
}

bool ensureAo(RecompD3dPresenter *presenter)
{
    const uint32_t width = (mainWidth(presenter) + 1u) / 2u;
    const uint32_t height = (mainHeight(presenter) + 1u) / 2u;
    if (presenter->ao_occlusion_pipeline != nullptr && presenter->ao_width == width &&
        presenter->ao_height == height) return true;
    releaseAo(presenter);
    const std::string header =
        "[[vk::binding(0, 2)]] Texture2D<float4> ao_depth : register(t0);\n"
        "[[vk::binding(0, 2)]] SamplerState ao_sampler : register(s0);\n"
        "[[vk::binding(0, RECOMP_UNIFORM_SET)]] cbuffer Ao : register(b0) { " RECOMP_AO_FIELDS " };\n"
        "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
        " float2 p = float2((id << 1) & 2, id & 2);\n"
        " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n";
    const std::string occlusion = header + RECOMP_AO_COMMON_HLSL RECOMP_AO_OCCLUSION_HLSL
        "float4 ps_main(float4 p : SV_Position) : SV_Target { return recompAoOcclusion(p); }\n";
    const std::string blur =
        "[[vk::binding(0, 2)]] Texture2D<float4> ao_depth : register(t0);\n"
        "[[vk::binding(0, 2)]] SamplerState ao_sampler : register(s0);\n"
        "[[vk::binding(1, 2)]] Texture2D<float4> ao_value : register(t1);\n"
        "[[vk::binding(1, 2)]] SamplerState ao_sampler1 : register(s1);\n"
        "[[vk::binding(0, RECOMP_UNIFORM_SET)]] cbuffer Ao : register(b0) { " RECOMP_AO_FIELDS " };\n"
        "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
        " float2 p = float2((id << 1) & 2, id & 2);\n"
        " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n"
        "float recompAoAt(int2 p) { return ao_value.Load(int3(p, 0)).x; }\n"
        RECOMP_AO_COMMON_HLSL RECOMP_AO_BLUR_HLSL
        "float4 ps_main(float4 p : SV_Position) : SV_Target { return recompAoBlur(p); }\n";
    const std::string apply =
        "[[vk::binding(0, 2)]] Texture2D<float4> ao_value : register(t0);\n"
        "[[vk::binding(0, 2)]] SamplerState ao_sampler : register(s0);\n"
        "[[vk::binding(0, RECOMP_UNIFORM_SET)]] cbuffer Ao : register(b0) { " RECOMP_AO_FIELDS " };\n"
        "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
        " float2 p = float2((id << 1) & 2, id & 2);\n"
        " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n"
        RECOMP_AO_APPLY_HLSL
        "float4 ps_main(float4 p : SV_Position) : SV_Target { return recompAoApply(p.xy / ao_screen.xy); }\n";
    presenter->ao_occlusion_pipeline = createAoPipeline(presenter, occlusion, 1u,
        SDL_GPU_TEXTUREFORMAT_R8_UNORM, false);
    presenter->ao_blur_pipeline = createAoPipeline(presenter, blur, 2u,
        SDL_GPU_TEXTUREFORMAT_R8_UNORM, false);
    presenter->ao_apply_pipeline = createAoPipeline(presenter, apply, 1u,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, true);
    if (presenter->ao_occlusion_pipeline != nullptr && presenter->ao_blur_pipeline != nullptr &&
        presenter->ao_apply_pipeline != nullptr) {
        const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        for (SDL_GPUTexture *&texture : presenter->ao_texture) {
            texture = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                usage, width, height);
        }
    }
    if (presenter->ao_occlusion_pipeline == nullptr || presenter->ao_blur_pipeline == nullptr ||
        presenter->ao_apply_pipeline == nullptr || presenter->ao_texture[0] == nullptr ||
        presenter->ao_texture[1] == nullptr) {
        std::fprintf(stderr, "recomp d3d presenter: ambient occlusion unavailable: %s\n", SDL_GetError());
        releaseAo(presenter);
        return false;
    }
    presenter->ao_width = width;
    presenter->ao_height = height;
    std::fprintf(stderr, "recomp d3d presenter: ambient occlusion %ux%u\n", width, height);
    return true;
}

/* Applies one frame's occlusion to the main target's RGB. */
bool runAo(RecompD3dPresenter *presenter)
{
    presenter->ao_done = true;
    if (presenter->msaa_count > 1u) {
        if (!presenter->ao_reported) {
            presenter->ao_reported = true;
            std::fprintf(stderr, "recomp d3d presenter: ambient occlusion needs a single-sample depth;"
                " MSAA %u leaves it off\n", presenter->msaa_count);
        }
        return false;
    }
    if (!recomp_ao_projection(presenter->ao_projection)) {
        if (!presenter->ao_reported) {
            presenter->ao_reported = true;
            std::fprintf(stderr, "recomp d3d presenter: ambient occlusion needs the scene's projection;"
                " this frame's draws carried none\n");
        }
        return false;
    }
    if (!ensureAo(presenter)) {
        presenter->ao = false;
        return false;
    }
    flushSegment(presenter);
    SDL_GPUCommandBuffer *commands = commandBuffer(presenter);
    if (commands == nullptr) return false;
    const RecompAoConstants constants = recomp_ao_constants(presenter->ao_projection,
        mainWidth(presenter), mainHeight(presenter), presenter->ao_width, presenter->ao_height);
    const SDL_GPUTextureSamplerBinding depth = {presenter->main_depth, presenter->point_sampler};
    // Occlusion into ao_texture[0].
    SDL_GPUColorTargetInfo target{};
    target.texture = presenter->ao_texture[0];
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr)) {
        SDL_BindGPUGraphicsPipeline(pass, presenter->ao_occlusion_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0u, &depth, 1u);
        SDL_PushGPUFragmentUniformData(commands, 0u, &constants, sizeof constants);
        SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
        SDL_EndGPURenderPass(pass);
    }
    // Depth-aware blur into ao_texture[1].
    const SDL_GPUTextureSamplerBinding blur_inputs[2] = {
        depth, {presenter->ao_texture[0], presenter->point_sampler}};
    target.texture = presenter->ao_texture[1];
    if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr)) {
        SDL_BindGPUGraphicsPipeline(pass, presenter->ao_blur_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0u, blur_inputs, 2u);
        SDL_PushGPUFragmentUniformData(commands, 0u, &constants, sizeof constants);
        SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
        SDL_EndGPURenderPass(pass);
    }
    // Multiply the scene; the HUD and effects come after this pass.
    const SDL_GPUTextureSamplerBinding occlusion = {presenter->ao_texture[1], presenter->filter_sampler};
    target.texture = presenter->main_color;
    target.load_op = SDL_GPU_LOADOP_LOAD;
    if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr)) {
        SDL_BindGPUGraphicsPipeline(pass, presenter->ao_apply_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0u, &occlusion, 1u);
        SDL_PushGPUFragmentUniformData(commands, 0u, &constants, sizeof constants);
        SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
        SDL_EndGPURenderPass(pass);
    }
    return true;
}

/* A draw that writes the scene's depth into the main target is the scene the
   occlusion covers. Everything else that follows one, before the frame ends,
   is a layer the game drew on top: run the pass before it so the HUD, menus,
   fonts and full-screen effects keep their pixels. */
bool aoSceneDraw(const RecompD3dPresenterDrawCommand &draw, const RecompD3dVertexLayout &layout)
{
    return !draw.target.offscreen && !draw.target.no_depth && !layout.pretransformed &&
        (draw.has_transform || draw.program_count != 0u) &&
        draw.depth.depth_test_enable && draw.depth.depth_write_enable;
}

void aoDrawBoundary(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw,
    const RecompD3dVertexLayout &layout)
{
    if (!presenter->ao) return;
    if (aoSceneDraw(draw, layout)) {
        presenter->ao_scene = true;
        std::memcpy(presenter->ao_projection, draw.projection, sizeof presenter->ao_projection);
    } else if (presenter->ao_scene && !presenter->ao_done && !draw.target.offscreen) {
        runAo(presenter);
    }
}

#endif
