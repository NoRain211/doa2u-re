// SPDX-License-Identifier: GPL-3.0-or-later
/* The SDL_GPU presenter's SMAA passes, included inside its anonymous
   namespace after the helpers they reuse. */
#ifndef DOA2U_RECOMP_D3D_SMAA_SDL_H
#define DOA2U_RECOMP_D3D_SMAA_SDL_H

/* SMAA 1x (iryoku/smaa, ultra preset) on the final resolved image, from the
   same HLSL as the Direct3D 11 presenter. Textures 0-4 are color, edges,
   area, search and weights; slots 0 and 1 also carry the linear and point
   samplers the shader names. */
bool createSmaa(RecompD3dPresenter *presenter)
{
#ifdef RECOMP_SMAA
    const uint32_t width = mainWidth(presenter), height = mainHeight(presenter);
    std::string source;
    std::vector<std::string> defines;
    d3d_backend_smaa_source(width, height, source, defines);
    static const char *const passes[3] = {"edges", "weights", "blend"};
    const SDL_GPUTextureFormat formats[3] = {SDL_GPU_TEXTUREFORMAT_R8G8_UNORM,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM};
    bool ready = true;
    for (unsigned pass = 0u; pass < 3u && ready; ++pass) {
        SDL_GPUShader *shaders[2]{};
        for (unsigned stage = 0u; stage < 2u; ++stage) {
            const std::string entry = std::string(stage == 0u ? "vs_" : "ps_") + passes[pass];
            RecompCompiledShader compiled;
            std::string error;
            if (!recomp_shader_compile(source, entry.c_str(), stage == 0u, presenter->shader_target, defines,
                    compiled, error)) {
                std::fprintf(stderr, "recomp d3d presenter: smaa compile %s: %s\n", entry.c_str(), error.c_str());
                ready = false;
                break;
            }
            SDL_GPUShaderCreateInfo info{};
            info.code = compiled.code.data();
            info.code_size = compiled.code.size();
            info.entrypoint = compiled.entry_point.c_str();
            info.format = presenter->shader_format;
            info.stage = stage == 0u ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
            info.num_samplers = stage == 0u ? 0u : 5u;
            shaders[stage] = SDL_CreateGPUShader(presenter->device, &info);
            ready = shaders[stage] != nullptr;
        }
        if (ready) {
            SDL_GPUColorTargetDescription color{};
            color.format = formats[pass];
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = shaders[0];
            info.fragment_shader = shaders[1];
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1u;
            presenter->smaa_pipelines[pass] = SDL_CreateGPUGraphicsPipeline(presenter->device, &info);
            ready = presenter->smaa_pipelines[pass] != nullptr;
        }
        for (SDL_GPUShader *shader : shaders) {
            if (shader != nullptr) SDL_ReleaseGPUShader(presenter->device, shader);
        }
    }
    const SDL_GPUTextureUsageFlags target = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (ready) {
        auto &textures = presenter->smaa_textures;
        textures[0] = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, formats[0], target, width, height);
        textures[1] = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM,
            SDL_GPU_TEXTUREUSAGE_SAMPLER, AREATEX_WIDTH, AREATEX_HEIGHT);
        textures[2] = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            SDL_GPU_TEXTUREUSAGE_SAMPLER, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
        textures[3] = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, formats[1], target, width, height);
        textures[4] = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, formats[2], target, width, height);
        for (SDL_GPUTexture *texture : textures) ready &= texture != nullptr;
        ready = ready &&
            uploadTexture(presenter, textures[1], areaTexBytes, sizeof areaTexBytes, 0u, AREATEX_WIDTH,
                AREATEX_HEIGHT, 1u, AREATEX_PITCH / 2u) &&
            uploadTexture(presenter, textures[2], searchTexBytes, sizeof searchTexBytes, 0u, SEARCHTEX_WIDTH,
                SEARCHTEX_HEIGHT, 1u, SEARCHTEX_PITCH);
    }
    if (!ready) {
        std::fprintf(stderr, "recomp d3d presenter: smaa unavailable: %s\n", SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "recomp d3d presenter: smaa ultra %ux%u\n", width, height);
    return true;
#else
    (void)presenter;
    return false;
#endif
}

/* Runs SMAA's three passes over the resolved back buffer into its output. */
SDL_GPUTexture *runSmaa(RecompD3dPresenter *presenter, SDL_GPUCommandBuffer *commands, SDL_GPUTexture *color)
{
    auto &textures = presenter->smaa_textures;
    SDL_GPUTexture *const targets[3] = {textures[0], textures[3], textures[4]};
    for (unsigned pass = 0u; pass < 3u; ++pass) {
        SDL_GPUColorTargetInfo target{};
        target.texture = targets[pass];
        target.load_op = pass < 2u ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *render = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
        if (render == nullptr) return color;
        // Never sample the target being drawn: unused slots get the white texture.
        const SDL_GPUTextureSamplerBinding bindings[5] = {
            {color, presenter->filter_sampler},
            {pass == 1u ? textures[0] : presenter->white_texture, presenter->point_sampler},
            {textures[1], presenter->filter_sampler},
            {textures[2], presenter->point_sampler},
            {pass == 2u ? textures[3] : presenter->white_texture, presenter->filter_sampler}};
        SDL_BindGPUGraphicsPipeline(render, presenter->smaa_pipelines[pass]);
        SDL_BindGPUFragmentSamplers(render, 0u, bindings, 5u);
        SDL_DrawGPUPrimitives(render, 3u, 1u, 0u, 0u);
        SDL_EndGPURenderPass(render);
    }
    return textures[4];
}

#endif
