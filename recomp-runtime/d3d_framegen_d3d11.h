/* Frame generation for the Direct3D 11 presenter (RECOMP_D3D_FRAMEGEN=1).
   Included by d3d_presenter_d3d11.cpp inside its anonymous namespace, after
   the draw, resolve and present helpers it reuses, the way
   d3d_boot_programs_sdl_gpu.h is included by the SDL_GPU presenter. The
   passes are d3d_framegen_shaders.h's and the slot schedule is
   d3d_framegen_schedule.h's; this file owns the Direct3D 11 resources, the
   HUD mask replay and the window presents. */

// Reads a RECOMP_ env flag the way the SDL_GPU presenter does.
bool fgEnvFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
}

int64_t fgSteadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* A gap this long between real frames starts the history again (a load). It is
   1.5 guest frames on the SDL_GPU presenter; here the gap also holds this
   presenter's own work between two presents (the flow, the mask and the
   in-betweens' passes), which the SDL_GPU one queues and submits in one go, so
   it allows 2.5 guest frames and still catches a load, hundreds of ms long.
   Tests set it through d3d_backend_framegen_set_late_ns. */
std::atomic<int64_t> fgStallNs{40000000};

bool fgReadback(RecompD3dPresenter *presenter, ID3D11Texture2D *texture, UINT width, UINT height,
    UINT x, UINT y, void *bytes, UINT pitch);

void fgReleaseTexture(FramegenTexture &entry)
{
    releaseCom(entry.view);
    releaseCom(entry.target);
    releaseCom(entry.texture);
}

bool fgCreateTexture(RecompD3dPresenter *presenter, FramegenTexture &entry, UINT width, UINT height,
    DXGI_FORMAT format, bool render_target = true, bool sample = true)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1u;
    desc.Format = format;
    desc.SampleDesc.Count = 1u;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = (render_target ? D3D11_BIND_RENDER_TARGET : 0u) |
        (sample ? D3D11_BIND_SHADER_RESOURCE : 0u);
    HRESULT result = presenter->device->CreateTexture2D(&desc, nullptr, &entry.texture);
    if (SUCCEEDED(result) && render_target)
        result = presenter->device->CreateRenderTargetView(entry.texture, nullptr, &entry.target);
    if (SUCCEEDED(result) && sample)
        result = presenter->device->CreateShaderResourceView(entry.texture, nullptr, &entry.view);
    if (FAILED(result)) {
        fgReleaseTexture(entry);
        return false;
    }
    return true;
}

/* Whether frame generation runs now: host time paces the guest, the window
   shows, and the picture is not presented the moment it is drawn. */
bool framegenActive(const RecompD3dPresenter *presenter)
{
    return presenter->fg_k != 0u && !immediate_present && !presenter->vrr &&
        !recomp_xapi_frame_clock_enabled() &&
        !(recomp_xapi_fast_forward != nullptr && recomp_xapi_fast_forward()) &&
        IsWindowVisible(presenter->window) && !IsIconic(presenter->window);
}

/* RECOMP_D3D_FRAMEGEN=1: window presents per guest frame, k = the display's
   refresh / 60, or 0. The chain is created with three buffers and a latency
   of two (createDeviceWithDriver): a slot presented one refresh after the last
   still finds a free buffer while one is shown and one queued, as SDL_GPU's
   three frames in flight do, and one that finds none is skipped rather than
   waited for. shortcut: k is read once at startup; a window moved to another
   display keeps it. */
unsigned framegenPresents(RecompD3dPresenter *presenter)
{
    if (!fgEnvFlag("RECOMP_D3D_FRAMEGEN")) return 0u;
    float refresh = 0.0f;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof monitor;
    DEVMODEW mode{};
    mode.dmSize = sizeof mode;
    if (GetMonitorInfoW(MonitorFromWindow(presenter->window, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        refresh = static_cast<float>(mode.dmDisplayFrequency);
    }
    const unsigned k = static_cast<unsigned>(refresh / 60.0f + 0.5f);
    const char *reason = unpacedRun() ? "unpaced run"
        : refresh <= 0.0f ? "unknown refresh"
        : presenter->vrr ? "VRR is on"
        : k < 2u ? "the refresh is below 120 Hz"
        : std::fabs(refresh - 60.0f * k) > 0.005f * refresh ? "the refresh is not a whole multiple of 60 Hz"
        : nullptr;
    if (reason != nullptr) {
        std::fprintf(stderr, "recomp d3d presenter: framegen unsupported: %s (%.2f Hz)\n", reason, refresh);
        return 0u;
    }
    std::fprintf(stderr, "recomp d3d presenter: framegen k=%u at %.2f Hz\n", k, refresh);
    return k;
}

/* Main target pixels per finest pyramid pixel: the finest level is at most 720
   high, so 2160 searches 1280x720. */
uint32_t fgFlowScale(const RecompD3dPresenter *presenter)
{
    return std::clamp((mainHeight(presenter) + 719u) / 720u, 1u, 4u);
}

void fgFlowLevelSize(const RecompD3dPresenter *presenter, unsigned level, uint32_t size[2])
{
    size[0] = mainWidth(presenter) / fgFlowScale(presenter);
    size[1] = mainHeight(presenter) / fgFlowScale(presenter);
    for (unsigned i = 0u; i < level; ++i) {
        size[0] = (std::max)(size[0] / 2u, 1u);
        size[1] = (std::max)(size[1] / 2u, 1u);
    }
}

// The search field of a level: one texel per 8x8 block, partial blocks included.
void fgFlowFieldSize(const RecompD3dPresenter *presenter, unsigned level, uint32_t size[2])
{
    fgFlowLevelSize(presenter, level, size);
    size[0] = (size[0] + 7u) / 8u;
    size[1] = (size[1] + 7u) / 8u;
}

// The HUD mask of frame i (0 = N-1, 1 = N). Both exist while the flow does.
FramegenTexture &framegenCoverOf(RecompD3dPresenter *presenter, unsigned i)
{
    return presenter->fg_cover[i];
}

void fgReleaseCoverDraws(RecompD3dPresenter *presenter)
{
    for (FramegenCoverDraw &draw : presenter->fg_cover_draws) {
        releaseCom(draw.pixel_shader);
        releaseCom(draw.vertex_shader);
        releaseCom(draw.input_layout);
        for (auto &view : draw.views) releaseCom(view);
        for (auto &sampler : draw.samplers) releaseCom(sampler);
        releaseCom(draw.raster);
    }
    presenter->fg_cover_draws.clear();
}

void fgReleaseFlow(RecompD3dPresenter *presenter)
{
    for (unsigned i = 0u; i < 2u; ++i) {
        for (unsigned level = 0u; level < recomp_framegen::kFlowLevels; ++level) {
            fgReleaseTexture(presenter->fg_luma[i][level]);
            fgReleaseTexture(presenter->fg_search[i][level]);
        }
        fgReleaseTexture(presenter->fg_flow[i]);
        fgReleaseTexture(presenter->fg_cover[i]);
        fgReleaseTexture(presenter->fg_cut[i]);
    }
    fgReleaseTexture(presenter->fg_motion);
    fgReleaseTexture(presenter->fg_cut_blocks);
    for (auto &shader : presenter->fg_flow_pixel) releaseCom(shader);
}

// The history, the pipelines and the HUD mask; false when they cannot exist.
void framegenRelease(RecompD3dPresenter *presenter)
{
    fgReleaseCoverDraws(presenter);
    fgReleaseFlow(presenter);
    fgReleaseTexture(presenter->fg_picture);
    fgReleaseTexture(presenter->fg_smaa);
    fgReleaseTexture(presenter->fg_prev);
    fgReleaseTexture(presenter->fg_cur);
    fgReleaseTexture(presenter->fg_out);
    releaseCom(presenter->fg_vertex_shader);
    releaseCom(presenter->fg_blend_pixel_shader);
    releaseCom(presenter->fg_flow_buffer);
    releaseCom(presenter->fg_point_sampler);
    releaseCom(presenter->fg_linear_sampler);
    releaseCom(presenter->fg_cover_blend);
    releaseCom(presenter->fg_cover_depth);
    presenter->fg_history = false;
    presenter->fg_failed = false;
    presenter->fg_flow_failed = false;
}

bool fgCompilePixelShader(RecompD3dPresenter *presenter, const std::string &source, const char *entry,
    const std::vector<D3D_SHADER_MACRO> &defines, ID3D11PixelShader **shader)
{
    std::vector<D3D_SHADER_MACRO> macros = defines;
    macros.push_back({nullptr, nullptr});
    ID3DBlob *blob = nullptr, *errors = nullptr;
    const HRESULT result = D3DCompile(source.data(), source.size(), nullptr, macros.data(), nullptr,
        entry, "ps_4_0", 0u, 0u, &blob, &errors);
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp d3d presenter: framegen shader %s failed hr=0x%08lX %s\n",
            entry, static_cast<unsigned long>(result),
            errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "");
    }
    releaseCom(errors);
    if (SUCCEEDED(result)) {
        const HRESULT created = presenter->device->CreatePixelShader(
            blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader);
        if (FAILED(created)) releaseCom(*shader);
    }
    releaseCom(blob);
    return SUCCEEDED(result) && *shader != nullptr;
}

/* One fullscreen pass into target, reading inputs in slot order. Every input
   slot gets the same sampler, as SDL_GPU binds one per input there; uniforms is
   written to the flow constant buffer when given. */
void fgPass(RecompD3dPresenter *presenter, ID3D11PixelShader *shader, FramegenTexture &target,
    UINT width, UINT height, ID3D11ShaderResourceView *const *views, UINT view_count,
    ID3D11SamplerState *sampler, const recomp_framegen::FlowUniforms *uniforms)
{
    auto *context = presenter->context;
    if (uniforms != nullptr) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(presenter->fg_flow_buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped))) {
            std::memcpy(mapped.pData, uniforms, sizeof *uniforms);
            context->Unmap(presenter->fg_flow_buffer, 0u);
        }
    }
    ID3D11ShaderResourceView *none[8]{};
    context->PSSetShaderResources(0u, 8u, none);
    context->OMSetRenderTargets(1u, &target.target, nullptr);
    const D3D11_VIEWPORT viewport = fullViewport(width, height);
    context->RSSetViewports(1u, &viewport);
    context->RSSetScissorRects(0u, nullptr);
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->fg_vertex_shader, nullptr, 0u);
    context->GSSetShader(nullptr, nullptr, 0u);
    context->PSSetShader(shader, nullptr, 0u);
    ID3D11Buffer *constant = presenter->fg_flow_buffer;
    context->PSSetConstantBuffers(0u, 1u, &constant);
    context->PSSetShaderResources(0u, view_count, views);
    ID3D11SamplerState *samplers[8]{};
    for (UINT slot = 0u; slot < view_count && slot < 8u; ++slot) samplers[slot] = sampler;
    context->PSSetSamplers(0u, view_count, samplers);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(presenter->fg_cover_depth, 0u);
    context->Draw(3u, 0u);
}

/* The forward (N-1 to N) and backward (N to N-1) fields in fg_flow, from
   fg_prev and fg_cur. N-1's pyramid is kept from the frame before unless
   both_frames. Levels are searched from coarse to fine, each around the
   coarser level's vector, so with radii 4 and 2 and the half-pixel fit carried
   up, a motion of up to 53.5 finest-level pixels per axis is found. */
void framegenFlow(RecompD3dPresenter *presenter, bool both_frames)
{
    auto &pipelines = presenter->fg_flow_pixel;
    if (presenter->fg_flow[0].texture == nullptr || pipelines[recomp_framegen::kFlowLuma] == nullptr)
        return;
    const int32_t scale = static_cast<int32_t>(fgFlowScale(presenter));
    FramegenTexture *frames[2] = {&presenter->fg_prev, &presenter->fg_cur};
    ID3D11SamplerState *point = presenter->fg_point_sampler;
    for (unsigned frame = both_frames ? 0u : 1u; frame < 2u; ++frame) {
        recomp_framegen::FlowUniforms uniforms{};
        uniforms.info[0] = scale;
        uniforms.weights[0] = 0.299f;
        uniforms.weights[1] = 0.587f;
        uniforms.weights[2] = 0.114f;
        ID3D11ShaderResourceView *const picture = frames[frame]->view;
        fgPass(presenter, pipelines[recomp_framegen::kFlowLuma], presenter->fg_luma[frame][0],
            mainWidth(presenter) / static_cast<UINT>(scale), mainHeight(presenter) / static_cast<UINT>(scale),
            &picture, 1u, point, &uniforms);
        for (unsigned level = 1u; level < recomp_framegen::kFlowLevels; ++level) {
            uint32_t size[2];
            fgFlowLevelSize(presenter, level, size);
            recomp_framegen::FlowUniforms down{};
            down.info[0] = 2;
            down.weights[0] = 1.0f;
            ID3D11ShaderResourceView *const below = presenter->fg_luma[frame][level - 1u].view;
            fgPass(presenter, pipelines[recomp_framegen::kFlowLuma], presenter->fg_luma[frame][level],
                size[0], size[1], &below, 1u, point, &down);
        }
    }
    for (unsigned level = recomp_framegen::kFlowLevels; level-- > 0u;) {
        uint32_t size[2], coarse[2]{};
        fgFlowLevelSize(presenter, level, size);
        if (level + 1u < recomp_framegen::kFlowLevels) fgFlowFieldSize(presenter, level + 1u, coarse);
        const recomp_framegen::FlowUniforms uniforms{{static_cast<int32_t>(size[0]), static_cast<int32_t>(size[1]),
            static_cast<int32_t>(coarse[0]), static_cast<int32_t>(coarse[1])}, {}};
        uint32_t field[2];
        fgFlowFieldSize(presenter, level, field);
        for (unsigned direction = 0u; direction < 2u; ++direction) {
            ID3D11ShaderResourceView *inputs[3] = {presenter->fg_luma[direction][level].view,
                presenter->fg_luma[1u - direction][level].view, nullptr};
            if (level + 1u == recomp_framegen::kFlowLevels) {
                fgPass(presenter, pipelines[recomp_framegen::kFlowCoarse],
                    presenter->fg_search[direction][level], field[0], field[1],
                    inputs, 2u, point, &uniforms);
            } else {
                inputs[2] = presenter->fg_search[direction][level + 1u].view;
                fgPass(presenter, pipelines[recomp_framegen::kFlowSearch],
                    presenter->fg_search[direction][level], field[0], field[1],
                    inputs, 3u, point, &uniforms);
            }
        }
    }
    uint32_t field[2];
    fgFlowFieldSize(presenter, 0u, field);
    const recomp_framegen::FlowUniforms uniforms{{static_cast<int32_t>(field[0]), static_cast<int32_t>(field[1]), 0, 0},
        {static_cast<float>(scale), 8.0f * scale, 1.0f / mainWidth(presenter), 1.0f / mainHeight(presenter)}};
    ID3D11SamplerState *point_sampler = presenter->fg_point_sampler;
    for (unsigned direction = 0u; direction < 2u; ++direction) {
        ID3D11ShaderResourceView *inputs[3] = {presenter->fg_search[direction][0].view,
            presenter->fg_search[1u - direction][0].view, framegenCoverOf(presenter, direction).view};
        fgPass(presenter, pipelines[recomp_framegen::kFlowConfidence], presenter->fg_flow[direction],
            field[0], field[1], inputs, 3u, point_sampler, &uniforms);
    }
    // Cut detection, then the cut flag: the pair before it (earlier) keeps its own.
    uint32_t coarsest[2], blocks[2];
    fgFlowLevelSize(presenter, recomp_framegen::kFlowLevels - 1u, coarsest);
    fgFlowFieldSize(presenter, recomp_framegen::kFlowLevels - 1u, blocks);
    {
        ID3D11ShaderResourceView *inputs[4] = {
            presenter->fg_luma[0][recomp_framegen::kFlowLevels - 1u].view,
            presenter->fg_luma[1][recomp_framegen::kFlowLevels - 1u].view,
            framegenCoverOf(presenter, 0u).view, framegenCoverOf(presenter, 1u).view};
        const recomp_framegen::FlowUniforms cut_uniforms{
            {static_cast<int32_t>(coarsest[0]), static_cast<int32_t>(coarsest[1]), 0, 0},
            {0.0f, 64.0f * fgFlowScale(presenter), 1.0f / mainWidth(presenter), 1.0f / mainHeight(presenter)}};
        fgPass(presenter, pipelines[recomp_framegen::kFlowCutBlocks], presenter->fg_cut_blocks,
            blocks[0], blocks[1], inputs, 4u, point_sampler, &cut_uniforms);
    }
    std::swap(presenter->fg_cut[0], presenter->fg_cut[1]);
    {
        ID3D11ShaderResourceView *inputs[2] = {presenter->fg_cut_blocks.view, presenter->fg_cut[1].view};
        const recomp_framegen::FlowUniforms cut_uniforms{
            {static_cast<int32_t>(blocks[0]), static_cast<int32_t>(blocks[1]), 0, 0}, {}};
        fgPass(presenter, pipelines[recomp_framegen::kFlowCut], presenter->fg_cut[0],
            1u, 1u, inputs, 2u, point_sampler, &cut_uniforms);
    }
}

/* The in-between picture t of the way from real frame N-1 to N, in fg_out:
   N-1 and N warped along the flow; without the flow, a crossfade, or without
   the crossfade's shader, the nearer real frame. */
void framegenGenerate(RecompD3dPresenter *p, float t)
{
    auto &pipelines = p->fg_flow_pixel;
    if (pipelines[recomp_framegen::kFlowView] != nullptr) {
        uint32_t field[2];
        fgFlowFieldSize(p, 0u, field);
        const float scale = static_cast<float>(fgFlowScale(p));
        ID3D11ShaderResourceView *inputs[3] = {p->fg_flow[0].view, framegenCoverOf(p, 1u).view, p->fg_cut[0].view};
        ID3D11SamplerState *point = p->fg_point_sampler;
        const recomp_framegen::FlowUniforms uniforms{{static_cast<int32_t>(field[0]), static_cast<int32_t>(field[1]), 0, 0},
            {8.0f * scale, 16.0f * scale, 1.0f / mainWidth(p), 1.0f / mainHeight(p)}};
        fgPass(p, pipelines[recomp_framegen::kFlowView], p->fg_out,
            mainWidth(p), mainHeight(p), inputs, 3u, point, &uniforms);
        return;
    }
    if (pipelines[recomp_framegen::kFlowWarp] != nullptr && p->fg_motion.texture != nullptr) {
        uint32_t field[2], size[2];
        fgFlowFieldSize(p, 0u, field);
        fgFlowLevelSize(p, 0u, size);
        const float scale = static_cast<float>(fgFlowScale(p));
        ID3D11SamplerState *linear = p->fg_linear_sampler;
        {
            ID3D11ShaderResourceView *inputs[4] = {p->fg_luma[0][0].view, p->fg_luma[1][0].view,
                p->fg_flow[0].view, p->fg_flow[1].view};
            const recomp_framegen::FlowUniforms uniforms{{static_cast<int32_t>(field[0]), static_cast<int32_t>(field[1]), 0, 0},
                {t, scale, 1.0f / size[0], 1.0f / size[1]}};
            fgPass(p, pipelines[recomp_framegen::kFlowSelect], p->fg_motion,
                size[0], size[1], inputs, 4u, linear, &uniforms);
        }
        ID3D11ShaderResourceView *inputs[6] = {p->fg_prev.view, p->fg_cur.view, p->fg_motion.view,
            framegenCoverOf(p, 0u).view, framegenCoverOf(p, 1u).view, p->fg_cut[0].view};
        const recomp_framegen::FlowUniforms uniforms{{static_cast<int32_t>(size[0]), static_cast<int32_t>(size[1]), 0, 0},
            {t, scale, 1.0f / mainWidth(p), 1.0f / mainHeight(p)}};
        fgPass(p, pipelines[recomp_framegen::kFlowWarp], p->fg_out,
            mainWidth(p), mainHeight(p), inputs, 6u, linear, &uniforms);
        return;
    }
    if (p->fg_blend_pixel_shader == nullptr) {
        p->context->CopyResource(p->fg_out.texture,
            (t < 0.5f ? p->fg_prev : p->fg_cur).texture);
        return;
    }
    // The crossfade's one float arrives at the top of the flow buffer.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(p->context->Map(p->fg_flow_buffer, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped))) {
        const float weight[4] = {t, 0.0f, 0.0f, 0.0f};
        std::memcpy(mapped.pData, weight, sizeof weight);
        p->context->Unmap(p->fg_flow_buffer, 0u);
    }
    auto *context = p->context;
    ID3D11ShaderResourceView *none[8]{};
    context->PSSetShaderResources(0u, 8u, none);
    context->OMSetRenderTargets(1u, &p->fg_out.target, nullptr);
    const D3D11_VIEWPORT viewport = fullViewport(mainWidth(p), mainHeight(p));
    context->RSSetViewports(1u, &viewport);
    context->RSSetScissorRects(0u, nullptr);
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(p->fg_vertex_shader, nullptr, 0u);
    context->GSSetShader(nullptr, nullptr, 0u);
    context->PSSetShader(p->fg_blend_pixel_shader, nullptr, 0u);
    ID3D11Buffer *constant = p->fg_flow_buffer;
    context->PSSetConstantBuffers(0u, 1u, &constant);
    ID3D11ShaderResourceView *inputs[2] = {p->fg_prev.view, p->fg_cur.view};
    context->PSSetShaderResources(0u, 2u, inputs);
    ID3D11SamplerState *point = p->fg_point_sampler;
    context->PSSetSamplers(0u, 1u, &point);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(p->fg_cover_depth, 0u);
    context->Draw(3u, 0u);
}

/* The flow's textures and shaders. On a failure the flow stays off and frame
   generation keeps its crossfade; logged once. */
bool fgCreateFlow(RecompD3dPresenter *presenter)
{
    auto &pipelines = presenter->fg_flow_pixel;
    bool created = true;
    for (unsigned level = 0u; level < recomp_framegen::kFlowLevels && created; ++level) {
        uint32_t size[2], field[2];
        fgFlowLevelSize(presenter, level, size);
        fgFlowFieldSize(presenter, level, field);
        for (unsigned i = 0u; i < 2u && created; ++i) {
            created = fgCreateTexture(presenter, presenter->fg_luma[i][level], size[0], size[1],
                    DXGI_FORMAT_R8_UNORM) &&
                fgCreateTexture(presenter, presenter->fg_search[i][level], field[0], field[1],
                    DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (level == 0u) {
                created = created && fgCreateTexture(presenter, presenter->fg_flow[i], field[0], field[1],
                    DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
        }
        if (level + 1u == recomp_framegen::kFlowLevels) {
            created = created && fgCreateTexture(presenter, presenter->fg_cut_blocks, field[0], field[1],
                DXGI_FORMAT_R16G16B16A16_FLOAT);
            for (auto &cut : presenter->fg_cut) {
                created = created && fgCreateTexture(presenter, cut, 1u, 1u, DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
        }
        if (level == 0u) {
            created = created && fgCreateTexture(presenter, presenter->fg_motion, size[0], size[1],
                DXGI_FORMAT_R16G16B16A16_FLOAT);
        }
    }
    if (created) {
        const std::string search = recomp_framegen::flowShader(false, {"source", "target", "coarse"},
            recomp_framegen::kFlowSearchShader);
        const std::string coarse_radius = std::to_string(recomp_framegen::kFlowCoarseRadius);
        const std::string search_radius = std::to_string(recomp_framegen::kFlowRadius);
        const std::vector<D3D_SHADER_MACRO> coarse_defines = {
            {"RADIUS", coarse_radius.c_str()}, {"PREDICT", "0"}};
        const std::vector<D3D_SHADER_MACRO> search_defines = {
            {"RADIUS", search_radius.c_str()}, {"PREDICT", "1"}};
        const std::string luma = recomp_framegen::flowShader(false, {"picture"}, recomp_framegen::kFlowLumaShader);
        const std::string confidence = recomp_framegen::flowShader(false, {"field", "other", "cover"},
            recomp_framegen::kFlowConfidenceShader);
        const std::string cut_blocks = recomp_framegen::flowShader(false,
            {"previous", "current", "previous_cover", "current_cover"}, recomp_framegen::kFlowCutBlocksShader);
        const std::string cut = recomp_framegen::flowShader(false, {"blocks", "earlier"},
            recomp_framegen::kFlowCutShader);
        const std::string select = recomp_framegen::flowShader(false,
            {"previous", "current", "forward", "backward"}, recomp_framegen::kFlowSelectShader);
        const std::string warp = recomp_framegen::flowShader(false,
            {"previous", "current", "motion", "previous_cover", "current_cover", "cut"},
            recomp_framegen::kFlowWarpShader);
        created = fgCompilePixelShader(presenter, luma, "ps_main", {},
                &pipelines[recomp_framegen::kFlowLuma]) &&
            fgCompilePixelShader(presenter, search, "ps_main", coarse_defines,
                &pipelines[recomp_framegen::kFlowCoarse]) &&
            fgCompilePixelShader(presenter, search, "ps_main", search_defines,
                &pipelines[recomp_framegen::kFlowSearch]) &&
            fgCompilePixelShader(presenter, confidence, "ps_main", {},
                &pipelines[recomp_framegen::kFlowConfidence]) &&
            fgCompilePixelShader(presenter, cut_blocks, "ps_main", {},
                &pipelines[recomp_framegen::kFlowCutBlocks]) &&
            fgCompilePixelShader(presenter, cut, "ps_main", {}, &pipelines[recomp_framegen::kFlowCut]) &&
            fgCompilePixelShader(presenter, select, "ps_main", {}, &pipelines[recomp_framegen::kFlowSelect]) &&
            fgCompilePixelShader(presenter, warp, "ps_main", {}, &pipelines[recomp_framegen::kFlowWarp]);
        if (created && presenter->fg_flow_view) {
            const std::string view = recomp_framegen::flowShader(false, {"field", "cover", "cut"},
                recomp_framegen::kFlowViewShader);
            created = fgCompilePixelShader(presenter, view, "ps_main", {}, &pipelines[recomp_framegen::kFlowView]);
        }
        if (created) {
            // The HUD mask: white where no 2D layer draw hides the scene.
            const float scene[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            for (unsigned i = 0u; i < 2u && created; ++i) {
                created = fgCreateTexture(presenter, presenter->fg_cover[i],
                    mainWidth(presenter), mainHeight(presenter), DXGI_FORMAT_R8_UNORM);
                if (created) presenter->context->ClearRenderTargetView(presenter->fg_cover[i].target, scene);
            }
        }
        if (created) {
            // A cut before the first pair would hold the previous frame: start steady.
            const float steady[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (auto &cut_target : presenter->fg_cut)
                presenter->context->ClearRenderTargetView(cut_target.target, steady);
        }
    }
    if (!created) {
        std::fprintf(stderr, "recomp d3d presenter: framegen flow unavailable, keeping the crossfade\n");
        presenter->fg_flow_failed = true;
        fgReleaseFlow(presenter);
        return false;
    }
    return true;
}

/* The history textures, the crossfade and the device state, made on first use.
   False without them, which turns frame generation off for this run. */
bool framegenCreate(RecompD3dPresenter *presenter)
{
    if (presenter->fg_picture.texture != nullptr) return true;
    if (presenter->fg_failed) return false;
    if (presenter->fg_cover_draws.capacity() == 0u) presenter->fg_cover_draws.reserve(64u);
    const UINT width = mainWidth(presenter), height = mainHeight(presenter);
    bool created = ensureSharedDrawState(presenter);
    for (FramegenTexture *target : {&presenter->fg_picture, &presenter->fg_smaa, &presenter->fg_prev,
            &presenter->fg_cur, &presenter->fg_out}) {
        created = created && fgCreateTexture(presenter, *target, width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    if (created) {
        static const char vertex[] =
            "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
            " float2 p = float2((id << 1) & 2, id & 2);\n"
            " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n";
        ID3DBlob *blob = nullptr, *errors = nullptr;
        HRESULT result = D3DCompile(vertex, sizeof vertex - 1u, nullptr, nullptr, nullptr,
            "vs_main", "vs_4_0", 0u, 0u, &blob, &errors);
        if (FAILED(result)) {
            std::fprintf(stderr, "recomp d3d presenter: framegen vertex shader failed hr=0x%08lX %s\n",
                static_cast<unsigned long>(result),
                errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        }
        releaseCom(errors);
        if (SUCCEEDED(result)) {
            result = presenter->device->CreateVertexShader(
                blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &presenter->fg_vertex_shader);
        }
        releaseCom(blob);
        created = created && SUCCEEDED(result) &&
            fgCompilePixelShader(presenter, recomp_framegen::blendShader(false), "ps_main", {},
                &presenter->fg_blend_pixel_shader);
    }
    if (created) {
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(recomp_framegen::FlowUniforms);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        created = SUCCEEDED(presenter->device->CreateBuffer(&buffer, nullptr, &presenter->fg_flow_buffer));
    }
    if (created) {
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        created = SUCCEEDED(presenter->device->CreateSamplerState(&sampler, &presenter->fg_point_sampler));
        sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        created = created &&
            SUCCEEDED(presenter->device->CreateSamplerState(&sampler, &presenter->fg_linear_sampler));
    }
    if (created) {
        /* Frame generation's HUD mask: the scene weight times one less what the
           2D layer hides, accumulated into one channel, as the SDL_GPU
           presenter's cover pipeline blends. */
        D3D11_BLEND_DESC blend{};
        D3D11_RENDER_TARGET_BLEND_DESC &target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_ZERO;
        target.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        created = SUCCEEDED(presenter->device->CreateBlendState(&blend, &presenter->fg_cover_blend));
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.StencilEnable = FALSE;
        created = created &&
            SUCCEEDED(presenter->device->CreateDepthStencilState(&depth, &presenter->fg_cover_depth));
    }
    if (created && !presenter->fg_flow_failed) fgCreateFlow(presenter);
    if (!created) {
        std::fprintf(stderr, "recomp d3d presenter: framegen off, no history textures\n");
        framegenRelease(presenter);
        presenter->fg_failed = true;
        presenter->fg_k = 0u;
        return false;
    }
    return true;
}

/* One recorded 2D layer draw, issued into the mask exactly as it went to the
   frame: same vertices, constants, views and viewport, the cover output and
   the mask blend instead of the frame's. */
void fgReplayCover(RecompD3dPresenter *presenter, const FramegenCoverDraw &draw)
{
    UINT vertex_offset = 0u, index_offset = 0u;
    if (!uploadRing(presenter, &presenter->draw_vertex_buffer, presenter->draw_vertex_capacity,
            presenter->draw_vertex_used, draw.vertices.data(),
            static_cast<UINT>(draw.vertices.size()), D3D11_BIND_VERTEX_BUFFER, vertex_offset) ||
        !uploadRing(presenter, &presenter->draw_index_buffer, presenter->draw_index_capacity,
            presenter->draw_index_used, draw.indices.data(),
            static_cast<UINT>(draw.indices.size()), D3D11_BIND_INDEX_BUFFER, index_offset) ||
        !bindDrawConstants(presenter, draw.constants)) {
        return;
    }
    auto *context = presenter->context;
    ID3D11ShaderResourceView *none[8]{};
    context->PSSetShaderResources(0u, 8u, none);
    context->OMSetRenderTargets(1u, &presenter->fg_cover[1].target, nullptr);
    context->OMSetDepthStencilState(presenter->fg_cover_depth, 0u);
    context->OMSetBlendState(presenter->fg_cover_blend, nullptr, 0xffffffffu);
    ID3D11Buffer *vertex_buffer = presenter->draw_vertex_buffer;
    context->IASetVertexBuffers(0u, 1u, &vertex_buffer, &draw.vertex_stride, &vertex_offset);
    context->IASetIndexBuffer(presenter->draw_index_buffer, DXGI_FORMAT_R16_UINT, index_offset);
    context->IASetPrimitiveTopology(draw.topology);
    context->IASetInputLayout(draw.input_layout);
    context->VSSetShader(draw.vertex_shader, nullptr, 0u);
    context->GSSetShader(nullptr, nullptr, 0u);
    context->PSSetShader(draw.pixel_shader, nullptr, 0u);
    context->PSSetShaderResources(0u, 4u, draw.views);
    context->PSSetSamplers(0u, 3u, draw.samplers);
    context->RSSetState(draw.raster);
    context->RSSetViewports(1u, &draw.viewport);
    context->RSSetScissorRects(1u, &draw.scissor);
    context->DrawIndexed(draw.index_count, 0u, 0);
}

/* The HUD mask of real frame N in fg_cover[1], N-1's kept in fg_cover[0]:
   the scene weight, 1 where no 2D layer draw (framegenQueueCover) hides the
   scene. Runs before the history advances, so the flow sees both masks. */
void framegenCover(RecompD3dPresenter *presenter)
{
    if (presenter->fg_cover[0].texture == nullptr) return;
    presenter->context->ClearState();
    std::swap(presenter->fg_cover[0], presenter->fg_cover[1]);
    const float scene[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    presenter->context->ClearRenderTargetView(presenter->fg_cover[1].target, scene);
    for (const FramegenCoverDraw &draw : presenter->fg_cover_draws) fgReplayCover(presenter, draw);
    fgReleaseCoverDraws(presenter);
    presenter->context->ClearState();
}

/* Drops this frame's queued mask draws without drawing them, when the frame is
   presented directly (frame generation off, hidden window, immediate present). */
void framegenDiscardCover(RecompD3dPresenter *presenter)
{
    if (!presenter->fg_cover_draws.empty()) fgReleaseCoverDraws(presenter);
}

/* Queues a draw of the 2D layer (recomp_widescreen_2d_layer: the HUD and
   menus, not fades, backdrops or full-height screen effects) for the mask. Its
   output is how much of the scene the fragment hides: all of it unblended, its
   alpha over the scene, its brightness when added. */
void framegenQueueCover(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw,
    const RecompD3dVertexLayout &layout, const void *vertices, UINT vertex_bytes,
    const void *indices, UINT index_bytes, UINT index_count, const float *constants)
{
    if (presenter->fg_cover[0].texture == nullptr || !layout.pretransformed ||
        draw.target.offscreen || draw.program_count != 0u || draw.blend.color_write_mask == 0u) return;
    float bounds[4];
    if (!recomp_widescreen_2d_layer(draw.vertex_bytes, draw.vertex_stride, draw.vertex_count,
            static_cast<const uint16_t *>(draw.index_bytes), draw.index_count,
            static_cast<float>(presenter->config.width), static_cast<float>(presenter->config.height),
            draw.has_texture && draw.texture.render_target, bounds)) return;
    const RecompD3dBlendState &blend = draw.blend;
    const uint8_t output = !blend.blend_enable ? kFgOutputOne
        : blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA ? kFgOutputAlpha
        : blend.dst_factor != RECOMP_D3D_BLEND_ONE ? kFgOutputOne
        : blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA ? kFgOutputLightAlpha
        : kFgOutputLight;
    const DrawPipeline *cover = lookupDrawPipeline(presenter, draw.fvf, &draw, false, output);
    if (cover == nullptr) return;
    try {
        FramegenCoverDraw &recorded = presenter->fg_cover_draws.emplace_back();
        recorded.pixel_shader = cover->pixel_shader;
        recorded.vertex_shader = cover->vertex_shader;
        recorded.input_layout = cover->input_layout;
        if (recorded.pixel_shader != nullptr) recorded.pixel_shader->AddRef();
        if (recorded.vertex_shader != nullptr) recorded.vertex_shader->AddRef();
        if (recorded.input_layout != nullptr) recorded.input_layout->AddRef();
        recorded.vertices.assign(static_cast<const uint8_t *>(vertices),
            static_cast<const uint8_t *>(vertices) + vertex_bytes);
        recorded.indices.assign(static_cast<const uint8_t *>(indices),
            static_cast<const uint8_t *>(indices) + index_bytes);
        recorded.vertex_stride = draw.vertex_stride;
        recorded.index_count = index_count;
        std::memcpy(recorded.constants, constants, sizeof recorded.constants);
        recorded.topology = draw.primitive_type == 2u ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
            : draw.primitive_type == 6u ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
            : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        presenter->context->PSGetShaderResources(0u, 4u, recorded.views);
        presenter->context->PSGetSamplers(0u, 3u, recorded.samplers);
        for (auto &view : recorded.views) if (view != nullptr) view->AddRef();
        for (auto &sampler : recorded.samplers) if (sampler != nullptr) sampler->AddRef();
        presenter->context->RSGetState(&recorded.raster);
        UINT count = 1u;
        presenter->context->RSGetViewports(&count, &recorded.viewport);
        if (count != 1u) recorded.viewport = fullViewport(mainWidth(presenter), mainHeight(presenter));
        count = 1u;
        presenter->context->RSGetScissorRects(&count, &recorded.scissor);
        if (count != 1u) recorded.scissor = {0, 0, 32767, 32767};
    } catch (const std::bad_alloc &) {
        // The mask keeps what was recorded; the frame itself is unaffected.
    }
}

/* Makes shown the newest real frame: fg_cur holds it, fg_prev the one before,
   and the flow is estimated between them. */
void framegenAdvance(RecompD3dPresenter *presenter, ID3D11Texture2D *shown)
{
    presenter->fg_restarted = !presenter->fg_history;
    std::swap(presenter->fg_prev, presenter->fg_cur);
    std::swap(presenter->fg_luma[0], presenter->fg_luma[1]);
    presenter->context->CopyResource(presenter->fg_cur.texture, shown);
    if (!presenter->fg_history) {
        presenter->context->CopyResource(presenter->fg_prev.texture, shown);
        if (presenter->fg_cover[0].texture != nullptr)
            presenter->context->CopyResource(presenter->fg_cover[0].texture, presenter->fg_cover[1].texture);
    }
    framegenFlow(presenter, !presenter->fg_history);
    presenter->fg_history = true;
}

/* The frame's picture before gamma in fg_picture: the main target resolved,
   then SMAA when it runs, the same picture the SDL_GPU presenter keeps. */
bool framegenResolvePicture(RecompD3dPresenter *presenter)
{
    if (presenter->render_target_view == nullptr || presenter->fg_picture.texture == nullptr) return false;
    auto *context = presenter->context;
    ID3D11Resource *source = nullptr;
    presenter->render_target_view->GetResource(&source);
    if (source == nullptr) return false;
    context->ClearState();
    if (presenter->msaa > 1u) {
        context->ResolveSubresource(presenter->fg_picture.texture, 0u, source, 0u, DXGI_FORMAT_B8G8R8A8_UNORM);
    } else {
        context->CopyResource(presenter->fg_picture.texture, source);
    }
    releaseCom(source);
    if (presenter->smaa && presenter->smaa_vs[0] == nullptr && !createSmaa(presenter)) {
        presenter->smaa = false;
    }
    if (presenter->smaa) {
        runSmaa(presenter, presenter->fg_picture.view, presenter->fg_smaa.target);
        context->ClearState();
        context->CopyResource(presenter->fg_picture.texture, presenter->fg_smaa.texture);
    }
    context->ClearState();
    return true;
}

/* Keeps real frame N's picture in the history and starts its slots. False
   while frame generation is off. N starts the history again, so no generated
   frame mixes it with N-1, after frame generation was off, a quality change
   (which releases the history), a change of aspect, or a stall (a load, see
   fgStallNs). */
bool framegenRecord(RecompD3dPresenter *presenter, float aspect)
{
    if (presenter->fg_history && presenter->fg_next_slot < presenter->fg_k) {
        presenter->fg_late += presenter->fg_k - 1u - presenter->fg_next_slot;
        ++presenter->fg_unshown;
    }
    if (fgSteadyNs() - presenter->fg_present_ns > fgStallNs || aspect != presenter->fg_aspect)
        presenter->fg_history = false;
    if (!framegenActive(presenter)) {
        presenter->fg_history = false;
        return false;
    }
    if (!framegenCreate(presenter)) return false;
    framegenCover(presenter);
    if (!framegenResolvePicture(presenter)) return false;
    framegenAdvance(presenter, presenter->fg_picture.texture);
    presenter->fg_aspect = aspect;
    presenter->fg_present_ns = fgSteadyNs();
    presenter->fg_next_slot = 0u;
    return true;
}

// The window's aspect for the picture's size, as submitPresent fits it.
D3D11_VIEWPORT fgFitViewport(const RecompD3dPresenter *presenter, float aspect, UINT width, UINT height,
    bool &drawn)
{
    const float window_aspect = presenter->widescreen
        ? static_cast<float>(presenter->aspect_w) / presenter->aspect_h : 4.0f / 3.0f;
    const float shown = aspect > 0.0f ? aspect : window_aspect;
    drawn = width != mainWidth(presenter) || height != mainHeight(presenter) ||
        std::fabs(shown - window_aspect) > 0.001f;
    return drawn ? fitViewport(shown, width, height) : fullViewport(width, height);
}

// The picture through gamma into target at the main target's size.
void fgGamma(RecompD3dPresenter *presenter, ID3D11ShaderResourceView *source, ID3D11RenderTargetView *target)
{
    auto *context = presenter->context;
    context->ClearState();
    ID3D11ShaderResourceView *none[8]{};
    context->PSSetShaderResources(0u, 8u, none);
    context->OMSetRenderTargets(1u, &target, nullptr);
    const D3D11_VIEWPORT viewport = fullViewport(mainWidth(presenter), mainHeight(presenter));
    context->RSSetViewports(1u, &viewport);
    context->RSSetScissorRects(0u, nullptr);
    context->RSSetState(nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter->gamma_vertex_shader, nullptr, 0u);
    context->GSSetShader(nullptr, nullptr, 0u);
    context->PSSetShader(presenter->gamma_pixel_shader, nullptr, 0u);
    ID3D11Buffer *constant = presenter->gamma_buffer;
    context->PSSetConstantBuffers(0u, 1u, &constant);
    context->PSSetShaderResources(0u, 1u, &source);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(presenter->fg_cover_depth, 0u);
    context->Draw(3u, 0u);
    context->ClearState();
}

/* Shows picture (the main target's size, before gamma) in the window through
   gamma, the aspect fit and the overlay, then presents it at one refresh. A
   real frame also takes the frame dump, the draw log and a screenshot, as
   submitPresent does when it presents directly. 1 when shown, 0 when the
   chain had no room for it this time, and -1 when Present failed. */
int framegenShowPicture(RecompD3dPresenter *presenter, const FramegenTexture &picture, float aspect, bool real)
{
    auto *context = presenter->context;
    DXGI_SWAP_CHAIN_DESC chain{};
    presenter->swap_chain->GetDesc(&chain);
    bool drawn = false;
    const D3D11_VIEWPORT viewport =
        fgFitViewport(presenter, aspect, chain.BufferDesc.Width, chain.BufferDesc.Height, drawn);
    /* The picture goes through gamma into the main-sized output, as the direct
       path does, then into the window. The output is also what a dump or a
       screenshot reads: reading the swap chain's buffer would wait on its
       flips, while this one only waits for this frame's own work. */
    context->ClearState();
    ID3D11Resource *output = nullptr;
    presenter->vrr_source->GetResource(&output);
    /* The output keeps a mip chain for the window blit, and CopyResource
       refuses that against a single-level picture: copy level 0 only. */
    if (presenter->gamma_enabled) {
        fgGamma(presenter, picture.view, presenter->vrr_target_view);
    } else if (output != nullptr) {
        context->CopySubresourceRegion(output, 0u, 0u, 0u, 0u, picture.texture, 0u, nullptr);
    }
    ID3D11Texture2D *frame = nullptr;
    if (output != nullptr) output->QueryInterface(IID_PPV_ARGS(&frame));
    releaseCom(output);
    if (drawn) {
        /* The implicit-LOD window sample only leaves mip 0 when the fitted
           picture is smaller than the render target. */
        if (viewport.Width < mainWidth(presenter) || viewport.Height < mainHeight(presenter))
            context->GenerateMips(presenter->vrr_source);
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearState();
        context->ClearRenderTargetView(presenter->present_target_view, black);
        blitPicture(presenter, presenter->vrr_source, presenter->present_target_view, viewport);
    } else {
        ID3D11Resource *back_buffer = nullptr;
        presenter->present_target_view->GetResource(&back_buffer);
        ID3D11Resource *shown = nullptr;
        presenter->vrr_source->GetResource(&shown);
        context->ClearState();
        if (back_buffer != nullptr && shown != nullptr)
            context->CopySubresourceRegion(back_buffer, 0u, 0u, 0u, 0u, shown, 0u, nullptr);
        releaseCom(shown);
        releaseCom(back_buffer);
    }
    if (real) capturePresentedFrame(presenter, frame, presenter->present_count);
    if (real && recomp_screenshot_take_request()) saveScreenshot(presenter, frame);
    releaseCom(frame);
    {
        DXGI_SWAP_CHAIN_DESC window{};
        presenter->swap_chain->GetDesc(&window);
        RecompOverlayText overlay;
        recomp_overlay_take(&overlay);
        recomp_overlay_d3d11_draw(presenter->device, presenter->context, presenter->present_target_view,
            window.BufferDesc.Width, window.BufferDesc.Height, &overlay);
    }
    /* Each slot is one refresh: frame generation replaces syncInterval's
       holds. Waiting for that refresh would hold the caller (the worker, or a
       test driving the presenter) for it and push the next real frame's
       arrival out, so a full chain skips the slot instead. */
    const HRESULT result = presenter->swap_chain->Present(1u, DXGI_PRESENT_DO_NOT_WAIT);
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) return 0;
    if (FAILED(result)) {
        std::fprintf(stderr, "recomp d3d presenter: framegen Present failed hr=0x%08lX\n",
            static_cast<unsigned long>(result));
        return -1;
    }
    return 1;
}

/* RECOMP_PERF_COUNTER: one line a second about frame generation's window presents. */
void framegenCount(RecompD3dPresenter *presenter, bool shown, bool real, int64_t start, int64_t end)
{
    presenter->fg_window_max_ms = (std::max)(presenter->fg_window_max_ms, (end - start) / 1e6);
    if (shown) {
        ++(real ? presenter->fg_real : presenter->fg_generated);
        if (presenter->fg_last_window_ns != 0)
            presenter->fg_intervals.push_back((end - presenter->fg_last_window_ns) / 1e6);
        presenter->fg_last_window_ns = end;
    } else {
        ++presenter->fg_not_ready;
    }
    if (presenter->fg_second_ns == 0) presenter->fg_second_ns = end;
    if (end - presenter->fg_second_ns < 1000000000) return;
    std::vector<double> &intervals = presenter->fg_intervals;
    std::sort(intervals.begin(), intervals.end());
    const auto at = [&](double q) {
        return intervals.empty() ? 0.0 : intervals[(std::min)(intervals.size() - 1u, static_cast<size_t>(q * intervals.size()))];
    };
    // Intervals within a quarter refresh of one refresh, of two, and the rest.
    const double refresh_ms = 1000.0 / (60.0 * presenter->fg_k);
    unsigned one = 0u, two = 0u;
    for (const double ms : intervals) {
        one += std::fabs(ms - refresh_ms) <= refresh_ms / 4.0;
        two += std::fabs(ms - 2.0 * refresh_ms) <= refresh_ms / 4.0;
    }
    std::fprintf(stderr, "recomp framegen: k=%u real=%u generated=%u late=%u not_ready=%u real_unshown=%u "
        "interval_ms p1=%.2f p50=%.2f p99=%.2f max=%.2f refreshes 1=%u 2=%u other=%zu window_max_ms=%.2f\n",
        presenter->fg_k, presenter->fg_real, presenter->fg_generated, presenter->fg_late, presenter->fg_not_ready,
        presenter->fg_unshown, at(0.01), at(0.5), at(0.99), at(1.0), one, two, intervals.size() - one - two,
        presenter->fg_window_max_ms);
    intervals.clear();
    presenter->fg_second_ns = end;
    presenter->fg_real = presenter->fg_generated = presenter->fg_late = presenter->fg_not_ready = presenter->fg_unshown = 0u;
    presenter->fg_window_max_ms = 0.0;
}

/* Shows slot's picture: an in-between from the flow, or the real frame
   itself at the last slot, which is the only one that is dumped. A slot whose
   swap chain image is not ready is skipped, as the SDL_GPU presenter skips
   one; a lost device turns frame generation off for real frames. */
bool framegenPresent(RecompD3dPresenter *presenter, unsigned slot)
{
    const bool real = slot + 1u == presenter->fg_k;
    /* The chain offers a buffer one refresh after a present, and a wait takes
       that offer; the real frame's buffer is waited for rather than skipped. */
    if (presenter->frame_latency != nullptr &&
        WaitForSingleObject(presenter->frame_latency, 0u) != WAIT_OBJECT_0) return false;
    const int64_t start = fgSteadyNs();
    if (!real) framegenGenerate(presenter, static_cast<float>(slot + 1u) / presenter->fg_k);
    const int shown = framegenShowPicture(presenter, real ? presenter->fg_cur : presenter->fg_out,
        presenter->fg_aspect, real);
    if (presenter->performance_counter) framegenCount(presenter, shown > 0, real, start, fgSteadyNs());
    if (shown < 0) {
        // A lost device must not keep the worker presenting: fall back to real frames.
        presenter->fg_history = false;
        presenter->fg_k = 0u;
        return false;
    }
    return shown > 0;
}

/* Presents real frame N's due slot and returns the 100 ns ticks until its
   next one, or UINT64_MAX once N is shown. An in-between whose slot passed is
   skipped; N is shown at its own slot. */
uint64_t framegenIdle(RecompD3dPresenter *presenter)
{
    if (!presenter->fg_history || presenter->fg_cur.texture == nullptr ||
        presenter->fg_next_slot >= presenter->fg_k) return UINT64_MAX;
    if (!framegenActive(presenter)) {
        presenter->fg_history = false;
        return UINT64_MAX;
    }
    const RecompFramegenDue due = recomp_framegen_due(presenter->fg_k, presenter->fg_present_ns,
        fgSteadyNs(), presenter->fg_next_slot);
    presenter->fg_late += due.missed;
    presenter->fg_next_slot += due.missed;
    if (due.slot >= 0) {
        const unsigned slot = static_cast<unsigned>(due.slot);
        if (!framegenPresent(presenter, slot) && slot + 1u == presenter->fg_k) return 5000u;
        presenter->fg_next_slot = slot + 1u;
    }
    if (presenter->fg_k == 0u || presenter->fg_next_slot >= presenter->fg_k) return UINT64_MAX;
    const int64_t until = recomp_framegen_until(presenter->fg_k, presenter->fg_present_ns,
        fgSteadyNs(), presenter->fg_next_slot);
    return static_cast<uint64_t>(until + 99) / 100u;
}

/* ---- Test hooks ---------------------------------------------------------- */

// Waits for the GPU to finish what the context holds, for the hooks' timings.
void fgWaitGpu(RecompD3dPresenter *presenter)
{
    const D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0u};
    ID3D11Query *query = nullptr;
    if (FAILED(presenter->device->CreateQuery(&desc, &query))) return;
    presenter->context->End(query);
    presenter->context->Flush();
    BOOL done = FALSE;
    for (unsigned i = 0u; i < 2000u; ++i) {
        if (presenter->context->GetData(query, &done, sizeof done, 0u) == S_OK) break;
        Sleep(0u);
    }
    releaseCom(query);
}

// Copies a width x height rectangle of a texture into bytes, packed.
bool fgReadback(RecompD3dPresenter *presenter, ID3D11Texture2D *texture, UINT width, UINT height,
    UINT x, UINT y, void *bytes, UINT pitch)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1u;
    desc.SampleDesc.Count = 1u;
    desc.SampleDesc.Quality = 0u;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(presenter->device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    const D3D11_BOX box = {x, y, 0u, x + width, y + height, 1u};
    presenter->context->CopySubresourceRegion(staging, 0u, 0u, 0u, 0u, texture, 0u, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool read = false;
    if (SUCCEEDED(presenter->context->Map(staging, 0u, D3D11_MAP_READ, 0u, &mapped))) {
        for (UINT row = 0u; row < height; ++row) {
            std::memcpy(static_cast<uint8_t *>(bytes) + row * pitch,
                static_cast<const uint8_t *>(mapped.pData) + row * mapped.RowPitch, pitch);
        }
        presenter->context->Unmap(staging, 0u);
        read = true;
    }
    releaseCom(staging);
    return read;
}

unsigned framegenProbe(RecompD3dPresenter *presenter, unsigned slot, uint32_t pixels[3])
{
    if (!presenter->fg_history || slot + 1u >= presenter->fg_k) return 0u;
    framegenGenerate(presenter, static_cast<float>(slot + 1u) / presenter->fg_k);
    fgWaitGpu(presenter);
    ID3D11Texture2D *textures[3] = {presenter->fg_prev.texture, presenter->fg_cur.texture, presenter->fg_out.texture};
    const UINT x = mainWidth(presenter) / 2u, y = mainHeight(presenter) / 2u;
    for (unsigned i = 0u; i < 3u; ++i) {
        if (!fgReadback(presenter, textures[i], 1u, 1u, x, y, &pixels[i], 4u)) return 0u;
    }
    return presenter->fg_k;
}

double framegenFlowBench(RecompD3dPresenter *presenter, unsigned repeat, unsigned size[2], float *fields,
    size_t capacity)
{
    if (!presenter->fg_history || presenter->fg_flow[0].texture == nullptr) return -1.0;
    uint32_t field[2];
    fgFlowFieldSize(presenter, 0u, field);
    size[0] = field[0];
    size[1] = field[1];
    const size_t texels = static_cast<size_t>(field[0]) * field[1] * 4u;  // RGBA16F
    for (unsigned i = 0u; i < repeat; ++i) framegenFlow(presenter, false);
    const int64_t start = fgSteadyNs();
    fgWaitGpu(presenter);
    const double ms = (fgSteadyNs() - start) / 1e6;
    if (fields != nullptr && capacity >= 2u * texels) {
        std::vector<uint16_t> halves(2u * texels);
        for (unsigned i = 0u; i < 2u; ++i) {
            if (!fgReadback(presenter, presenter->fg_flow[i].texture, field[0], field[1], 0u, 0u,
                    halves.data() + i * texels, field[0] * 8u)) return -1.0;
        }
        for (size_t i = 0u; i < 2u * texels; ++i) {
            const unsigned half = halves[i], exponent = half >> 10 & 31u, mantissa = half & 1023u;
            const float value = exponent == 0u ? std::ldexp(static_cast<float>(mantissa), -24)
                : exponent == 31u ? INFINITY : std::ldexp(static_cast<float>(mantissa + 1024u), static_cast<int>(exponent) - 25);
            fields[i] = half & 0x8000u ? -value : value;
        }
    }
    return ms;
}

double framegenGenerateBench(RecompD3dPresenter *presenter, float t, unsigned repeat, unsigned size[2],
    uint32_t *pixels, size_t capacity)
{
    if (!presenter->fg_history) return -1.0;
    size[0] = mainWidth(presenter);
    size[1] = mainHeight(presenter);
    for (unsigned i = 0u; i < repeat; ++i) framegenGenerate(presenter, t);
    const int64_t start = fgSteadyNs();
    fgWaitGpu(presenter);
    const double ms = (fgSteadyNs() - start) / 1e6;
    if (pixels != nullptr && capacity >= static_cast<size_t>(size[0]) * size[1]) {
        if (!fgReadback(presenter, presenter->fg_out.texture, size[0], size[1], 0u, 0u,
                pixels, size[0] * 4u)) return -1.0;
    }
    return ms;
}

double framegenCoverBench(RecompD3dPresenter *presenter, unsigned repeat, unsigned *draws)
{
    if (presenter->fg_cover[0].texture == nullptr) return -1.0;
    *draws = static_cast<unsigned>(presenter->fg_cover_draws.size());
    const float scene[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (unsigned i = 0u; i < repeat; ++i) {
        presenter->context->ClearRenderTargetView(presenter->fg_cover[1].target, scene);
        for (const FramegenCoverDraw &draw : presenter->fg_cover_draws) fgReplayCover(presenter, draw);
    }
    const int64_t start = fgSteadyNs();
    fgWaitGpu(presenter);
    return (fgSteadyNs() - start) / 1e6;
}

// Makes picture (0xAARRGGBB before gamma) real frame N and hud its mask.
bool framegenPush(RecompD3dPresenter *presenter, const uint32_t *picture, const uint8_t *hud, bool restart)
{
    if (!framegenCreate(presenter)) return false;
    const UINT width = mainWidth(presenter);
    auto *context = presenter->context;
    context->UpdateSubresource(presenter->fg_out.texture, 0u, nullptr, picture, width * 4u, 0u);
    if (presenter->fg_cover[0].texture != nullptr && hud != nullptr) {
        context->UpdateSubresource(presenter->fg_cover[1].texture, 0u, nullptr, hud, width, 0u);
    }
    if (restart) presenter->fg_history = false;
    framegenAdvance(presenter, presenter->fg_out.texture);
    presenter->fg_present_ns = fgSteadyNs();
    presenter->fg_next_slot = 0u;
    return true;
}

void framegenCrossfade(RecompD3dPresenter *presenter)
{
    fgReleaseFlow(presenter);
    presenter->fg_flow_failed = true;
}
