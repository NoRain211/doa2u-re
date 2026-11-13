/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_D3D_PRESENTER_D3D11_H
#define DOA2U_RECOMP_D3D_PRESENTER_D3D11_H

#include "d3d_draw_shader.h"
#include "d3d_presenter_backend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <vector>

/* The Direct3D 11 presenter's state, shared by d3d_presenter_d3d11.cpp and
   d3d_presenter_quality.cpp. */

template <typename T>
void releaseCom(T *&object)
{
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

struct FrameRateCounter {
    ULONGLONG start_ms = 0;
    uint32_t frames = 0;
    bool started = false;
};

/* Compiled state for one FVF. */
/* ponytail: 16 pipelines cover the measured ten-pipeline pool working set.
   Revisit the bound if another scene exceeds it and causes compilation churn. */
constexpr uint32_t kDrawPipelineSlots = 16u;

struct DrawPipeline {
    uint32_t fvf;
    uint32_t program_count;
    uint32_t program[136][4];
    bool used;
    bool failed;
    bool scaled_depth_bias;
    bool sample_depth_bias;
    bool coverage_only;
    bool projected_stages;
    ID3D11VertexShader *vertex_shader;
    ID3D11GeometryShader *geometry_shader;
    ID3D11PixelShader *pixel_shader;
    ID3D11InputLayout *input_layout;
};

/* Depth-stencil states are few and repeat every frame, so they are cached by
   the guest state that produced them rather than rebuilt per draw. */
constexpr uint32_t kDepthStateSlots = 16u;

struct DepthStateEntry {
    bool used;
    bool test_enable;
    bool write_enable;
    RecompD3dCompareFunc func;
    bool stencil_enable;
    RecompD3dCompareFunc stencil_func;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    RecompD3dStencilOp stencil_fail;
    RecompD3dStencilOp stencil_zfail;
    RecompD3dStencilOp stencil_pass;
    ID3D11DepthStencilState *state;
};

constexpr uint32_t kRasterStateSlots = 16u;
struct RasterStateEntry {
    RecompD3dCullMode cull;
    int32_t bias;
    float slope;
    ID3D11RasterizerState *state;
};

constexpr uint32_t kBlendStateSlots = 16u;

struct BlendStateEntry {
    bool used;
    bool enable;
    RecompD3dBlendFactor src;
    RecompD3dBlendFactor dst;
    RecompD3dBlendOp op;
    uint8_t color_write_mask;
    ID3D11BlendState *state;
};

constexpr uint32_t kSamplerSlots = 64u;

struct SamplerEntry {
    D3D11_TEXTURE_ADDRESS_MODE u, v, w;
    bool point_mip_filter;
    float mip_lod_bias;
    ID3D11SamplerState *state;
};

/* Guest textures are cached by address and shape. Linear movie buffers are
   rewritten in place, so their pixels are refreshed whenever sampled.
   Map and menu screens sample about 300 textures per frame; a FIFO smaller
   than one frame's set evicts each texture before its next use and rebuilds
   all of them every frame. */
constexpr uint32_t kTextureSlots = 4096u;
constexpr uint32_t kPaletteBytes = 256u * 4u;

struct TextureEntry {
    bool used;
    uint32_t data;
    uint32_t format_byte;
    uint32_t width;
    uint32_t height;
    uint32_t volume_depth;
    uint32_t mip_levels;
    uint8_t palette[kPaletteBytes];
    uint64_t fingerprint;
    uint32_t scale;  // texture_upscale.h: the 2D art's redrawn copy
    ID3D11ShaderResourceView *view;
};

/* A guest buffer can be refilled with another texture of the same size and
   format without being freed (item previews reuse theirs), so a hit must also
   match the texels.
   Small spans hash every byte; repeated whole-word samples can cancel changes.
   shortcut: larger spans sample 128 words; hash all bytes if a missed rewrite is observed. */
inline uint64_t textureFingerprint(const void *bytes, uint32_t count)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ count;
    if (count <= 1024u) {
        for (uint32_t i = 0u; i < count; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
        return hash;
    }
    constexpr uint32_t kSamples = 128u;
    for (uint32_t i = 0u; i < kSamples; ++i) {
        uint64_t word;
        std::memcpy(&word, data + static_cast<uint64_t>(count - 8u) * i / (kSamples - 1u), 8u);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

/* Rendered pixels have no CPU copy to re-upload after texture FIFO eviction.
   Retain targets until their guest backing storage is released.
   shortcut: retain at most 64 MiB of guest target storage; add suballocation
   retirement if scene targets exhaust this budget. Host storage scales with AA/resolution. */
constexpr uint64_t kTargetByteLimit = 64u * 1024u * 1024u;

struct RenderTargetEntry {
    RecompD3dTextureDesc desc;
    ID3D11RenderTargetView *render_view;
    ID3D11ShaderResourceView *sample_view;
    bool multisampled;
    bool resolved;  // sample_view holds every write since the last resolve
    uint64_t bytes;
    uint32_t host_width, host_height;
    bool scaled;    // sized by the render scale (it has its own depth)
};

/* Depth belongs to its guest storage, which can serve several color targets. */

struct DepthTargetEntry {
    RecompD3dTextureDesc desc;
    ID3D11DepthStencilView *view;
    bool host_main;  // created at the scaled/MSAA main target size
    uint64_t bytes;
    uint32_t host_width;
};

struct RecompD3dPresenter {
    RecompD3dPresenterConfig config{};
    DWORD owner_thread = 0;
    HINSTANCE instance = nullptr;
    bool owns_window_class = false;
    HWND window = nullptr;
    bool close_requested = false;
    bool fullscreen = false;           // borderless over the monitor; F11 and Alt+Enter toggle it
    WINDOWPLACEMENT windowed_placement{}; // where leaving fullscreen returns; length 0 = unset
    bool widescreen = false;
    uint32_t aspect_w = 16u, aspect_h = 9u; // widescreen shape, from RECOMP_D3D_WIDESCREEN
    float scale = 1.0f;         // RECOMP_D3D_SCALE: main target height multiplier
    uint32_t msaa = 1u;         // RECOMP_D3D_MSAA: main target sample count
    unsigned quality_applied = 0u; // last PC Settings quality generation applied
    unsigned quality_retry_generation = 0u; // failed rebuild waiting to retry
    std::chrono::steady_clock::time_point quality_retry_at{};
    float target_scale_x = 1.0f, target_scale_y = 1.0f; // bound target / guest size
    uint32_t target_samples = 1u; // MSAA samples of the bound target
    // Occlusion queries: the open BEGIN, ended ones in submission order, reusable ones.
    struct VisibilityQuery { ID3D11Query *query; uint32_t index; float samples_per_pixel; };
    ID3D11Query *visibility_active = nullptr;
    std::vector<VisibilityQuery> visibility_pending;
    std::vector<ID3D11Query *> visibility_free;
    bool smaa = false;          // RECOMP_D3D_SMAA: post-process the output
    ID3D11VertexShader *smaa_vs[3]{};   // edges, weights, blend
    ID3D11PixelShader *smaa_ps[3]{};
    ID3D11ShaderResourceView *smaa_views[6]{}; // -, edges, area, search, weights, output
    ID3D11RenderTargetView *smaa_targets[3]{}; // edges, weights, output
    ID3D11SamplerState *smaa_samplers[2]{};   // linear, point
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGISwapChain *swap_chain = nullptr;
    ID3D11RenderTargetView *render_target_view = nullptr;
    ID3D11RenderTargetView *present_target_view = nullptr;
    // VRR only: main-sized output, downscaled into the window-sized swap chain.
    ID3D11RenderTargetView *vrr_target_view = nullptr;
    ID3D11ShaderResourceView *vrr_source = nullptr;
    ID3D11VertexShader *vrr_vs = nullptr;
    ID3D11PixelShader *vrr_ps = nullptr;
    ID3D11SamplerState *vrr_sampler = nullptr;
    ID3D11VertexShader *gamma_vertex_shader = nullptr;
    ID3D11PixelShader *gamma_pixel_shader = nullptr;
    ID3D11Buffer *gamma_buffer = nullptr;
    bool gamma_enabled = false;
    ID3D11Texture2D *back_buffer_copy = nullptr;
    ID3D11ShaderResourceView *back_buffer_sample = nullptr;
    ID3D11Texture2D *previous_frame = nullptr;
    ID3D11ShaderResourceView *previous_frame_sample = nullptr;
    ID3D11Texture2D *depth_texture = nullptr;
    ID3D11DepthStencilView *depth_view = nullptr;
    ID3D11Buffer *draw_vertex_buffer = nullptr;
    ID3D11Buffer *draw_index_buffer = nullptr;
    ID3D11Buffer *draw_constant_buffer = nullptr;
    /* D3D11.1 ring of per-draw constants, bound by offset; null falls back to
       draw_constant_buffer, which renames on every draw. */
    ID3D11DeviceContext1 *context1 = nullptr;
    ID3D11Buffer *constant_ring = nullptr;
    UINT constant_ring_used = 0u;
    /* Ring slot holding the previous draw's constants and a copy of them, so an
       unchanged draw rebinds the slot instead of uploading it again. */
    UINT constant_last_offset = ~0u;
    float constant_last[kRecompD3dDrawConstantFloats];
    RasterStateEntry raster_states[kRasterStateSlots]{};
    uint32_t raster_state_count = 0u, next_raster_state_slot = 0u;
    UINT draw_vertex_capacity = 0u;
    UINT draw_index_capacity = 0u;
    UINT draw_vertex_used = 0u;
    UINT draw_index_used = 0u;
    /* One pipeline per FVF. A failure is recorded against its own FVF so a
       stream this seam cannot build never disables the ones it can. */
    DrawPipeline draw_pipelines[kDrawPipelineSlots]{};
    uint32_t draw_pipeline_count = 0u;
    uint32_t next_draw_pipeline_slot = 0u;
    std::atomic<bool> precompile_stop{false};
    std::thread precompile;
    DepthStateEntry depth_states[kDepthStateSlots]{};
    uint32_t depth_state_count = 0u;
    uint32_t next_depth_state_slot = 0u;
    BlendStateEntry blend_states[kBlendStateSlots]{};
    uint32_t blend_state_count = 0u;
    uint32_t next_blend_state_slot = 0u;
    std::vector<TextureEntry> textures = std::vector<TextureEntry>(kTextureSlots);
    std::unordered_multimap<uint32_t, uint32_t> texture_index; // guest address -> slot
    uint32_t texture_count = 0u;
    uint32_t next_texture_slot = 0u;
    std::vector<RenderTargetEntry> render_targets;
    std::vector<DepthTargetEntry> depth_targets;
    uint64_t target_bytes = 0u;
    ID3D11SamplerState *draw_sampler = nullptr;
    SamplerEntry samplers[kSamplerSlots]{};
    uint32_t sampler_count = 0u;
    uint32_t next_sampler_slot = 0u;
    ID3D11SamplerState *filter_sampler = nullptr;
    ID3D11SamplerState *program_mask_sampler = nullptr;
    bool draw_shared_ready = false;
    bool draw_shared_failed = false;
    uint32_t draw_count = 0u;
    bool first_draw_reported = false;
    const char *driver_name = "unknown";
    HRESULT create_result = E_FAIL;
    uint32_t present_count = 0;
    bool performance_counter = false;
    FrameRateCounter frame_rate;
    // RECOMP_PERF_COUNTER pacing: present-to-present gaps within the second.
    double last_present_ms = 0.0;
    double present_call_max_ms = 0.0;
    double texture_create_ms = 0.0;  // cache-miss texture builds within the second
    unsigned textures_created = 0u;
    std::vector<double> present_gaps;
    // Display refreshes each present stayed on screen (DXGI statistics), 1..8+.
    unsigned refresh_holds[9]{};
    UINT last_stat_present = 0u, last_stat_refresh = 0u;
    /* RECOMP_D3D_PRESENT_LOG: one CSV row of raw DXGI counters per present,
       in QPC ticks, to line up with a PresentMon --qpc_time capture. */
    FILE *present_log = nullptr;
    UINT sync_interval = 1u;
    std::chrono::steady_clock::time_point next_refresh_check{};
    bool vrr = false;           // RECOMP_D3D_VRR=1: the game's 60 Hz timer paces a VRR display
    long long vrr_slot_ns = 0;  // steady_clock time of the last VRR present slot
    bool first_present_reported = false;
    unsigned frame_dump_count = 0u;
    ULONGLONG next_frame_dump_ms = 0u;
    /* RECOMP_D3D_DRAW_LOG names a trigger file: when it appears, the next
       frame's draws are logged, that frame is saved beside it, and it is deleted. */
    const char *draw_log_trigger = nullptr;
    bool draw_log_active = false;
    unsigned draw_log_index = 0u;
};

/* The display aspect: RECOMP_D3D_WIDESCREEN's shape (16:9 for 1), else 4:3.
   The game renders 4:3 or that shape anamorphic into its backbuffer; a
   netplay session takes the host's 4:3 or 16:9, and submitPresent fits it in. */
inline uint32_t presentClientWidth(const RecompD3dPresenter *presenter, uint64_t height)
{
    return static_cast<uint32_t>(presenter->widescreen
        ? (height * presenter->aspect_w + presenter->aspect_h - 1u) / presenter->aspect_h
        : (height * 4u + 1u) / 3u);
}

/* A scaled main target renders at the display aspect, so the anamorphic guest
   width gets full horizontal detail too. Scale 1 keeps the guest size. */
inline uint32_t mainHeight(const RecompD3dPresenter *presenter)
{
    return static_cast<uint32_t>(presenter->config.height * presenter->scale + 0.5f);
}

inline uint32_t mainWidth(const RecompD3dPresenter *presenter)
{
    return presenter->scale == 1.0f
        ? presenter->config.width : presentClientWidth(presenter, mainHeight(presenter));
}

/* Offscreen scenes with their own depth (water reflections) scale too; their samplers use
   normalized UVs. Depthless filter chains (bloom, haze) keep guest size: their taps are one
   guest texel apart, so scaling them leaves gaps between taps and a sharp halo. */
inline uint32_t scaledSize(const RecompD3dPresenter *presenter, uint32_t guest)
{
    return (std::min)(static_cast<uint32_t>(guest * presenter->scale + 0.5f),
        static_cast<uint32_t>(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION));
}

void toggleFullscreen(RecompD3dPresenter *presenter);
void releaseSmaa(RecompD3dPresenter *presenter);
ID3D11ShaderResourceView *lookupBackBufferTexture(RecompD3dPresenter *presenter,
    const RecompD3dTextureDesc &desc, bool previous_frame = false);
/* Applies PC Settings' quality between frames. */
void applyQuality(RecompD3dPresenter *presenter);

#endif
