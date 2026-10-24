// SPDX-License-Identifier: GPL-3.0-or-later
/* The presenter backend for macOS (Metal) and Linux (Vulkan), through
   SDL_GPU. It follows the Direct3D 11 backend (d3d_presenter_d3d11.cpp) call
   for call: the same draw shader and constants (d3d_draw_shader.cpp), target
   sizes, declines and resource lifetimes. Where SDL_GPU lacks a Direct3D 11
   feature this file says so: widened lines are expanded on the CPU instead of
   in a geometry shader, coverage probes rasterize on the CPU, and occlusion
   counts come from an additive pass. VRR and RECOMP_D3D_PRESENT_LOG are
   Direct3D 11 only. */
#include "d3d_presenter_backend.h"
#include "d3d_draw_model.h"
#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"
#include "d3d_texture_model.h"
#include "d3d_vblank.h"
#include "input_keyboard_win32.h"

#include <SDL3/SDL.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef RECOMP_SMAA
#include "AreaTex.h"
#include "SearchTex.h"
static const unsigned char kSmaaSource[] = {
#include "smaa_hlsl.inc"
};
#endif

namespace {

constexpr uint32_t kTextureSlots = 4096u;
constexpr uint32_t kPaletteBytes = 256u * 4u;
/* As in the D3D11 backend: guest target storage is budgeted, not host texels. */
constexpr uint64_t kTargetByteLimit = 64u * 1024u * 1024u;
constexpr uint32_t kMaxTextureSize = 16384u;
constexpr uint32_t kMaxVolumeSize = 2048u;
constexpr float kMipLodBiasMin = -16.0f, kMipLodBiasMax = 15.99f;
/* The draw constants without vc[192], which goes to its own uniform slot. */
constexpr uint32_t kProgramFloats = 192u * 4u;
constexpr uint32_t kTransformFloats = kRecompD3dDrawConstantFloats - kProgramFloats;
constexpr uint32_t kInitialVertexCapacity = 32u * 1024u * 1024u;
constexpr uint32_t kInitialIndexCapacity = 16u * 1024u * 1024u;

bool envFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
}

// RECOMP_UNPACED=1 skips host waits, so the game runs faster than 60 Hz.
bool unpacedRun()
{
    const char *value = std::getenv("RECOMP_UNPACED");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool fileExists(const char *path)
{
    struct stat info;
    return path != nullptr && stat(path, &info) == 0;
}

uint64_t hashBytes(const void *bytes, size_t count, uint64_t hash = 0xcbf29ce484222325ull)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0u; i < count; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

/* As in the D3D11 backend: small spans hash every byte, larger ones sample
   128 words. shortcut: hash all bytes if a missed rewrite is observed. */
uint64_t textureFingerprint(const void *bytes, uint32_t count)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ count;
    if (count <= 1024u) return hashBytes(data, count, hash);
    constexpr uint32_t kSamples = 128u;
    for (uint32_t i = 0u; i < kSamples; ++i) {
        uint64_t word;
        std::memcpy(&word, data + static_cast<uint64_t>(count - 8u) * i / (kSamples - 1u), 8u);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

struct TextureEntry {
    bool used = false;
    uint32_t data = 0u;
    uint32_t format_byte = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t volume_depth = 0u;
    uint32_t mip_levels = 0u;
    uint8_t palette[kPaletteBytes]{};
    uint64_t fingerprint = 0u;
    SDL_GPUTexture *texture = nullptr;
};

/* Rendered pixels have no CPU copy, so targets live until their guest
   storage is released. A multisampled target draws into render and samples
   resolve, which is refreshed only after new writes. */
struct RenderTargetEntry {
    RecompD3dTextureDesc desc{};
    SDL_GPUTexture *render = nullptr;
    SDL_GPUTexture *resolve = nullptr;
    bool multisampled = false;
    bool resolved = true;
    uint64_t bytes = 0u;
    uint32_t host_width = 0u, host_height = 0u;
    SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
};

struct DepthTargetEntry {
    RecompD3dTextureDesc desc{};
    SDL_GPUTexture *texture = nullptr;
    bool host_main = false;
    uint64_t bytes = 0u;
    uint32_t host_width = 0u;
};

struct SamplerEntry {
    SDL_GPUSamplerAddressMode u, v, w;
    bool point_mip_filter;
    float mip_lod_bias;
    SDL_GPUSampler *sampler;
};

struct ShaderPair {
    SDL_GPUShader *vertex = nullptr;
    SDL_GPUShader *fragment = nullptr;
    RecompD3dVertexLayout layout{};
};

/* Everything that selects one SDL graphics pipeline. Zero-filled before use,
   so it hashes and compares as bytes. */
struct PipelineKey {
    SDL_GPUShader *vertex;
    SDL_GPUShader *fragment;
    uint32_t fvf;
    uint32_t stride;
    uint32_t primitive;
    uint32_t cull;
    uint32_t color_format;
    uint32_t depth_format;
    uint32_t samples;
    int32_t depth_bias;
    float slope_bias;
    uint8_t coverage_only;
    uint8_t count_pass;
    uint8_t depth_test, depth_write, depth_func;
    uint8_t stencil_enable, stencil_read, stencil_write, stencil_func;
    uint8_t stencil_fail, stencil_zfail, stencil_pass;
    uint8_t blend_enable, write_mask, src_factor, dst_factor, blend_op;
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &key) const
    {
        return static_cast<size_t>(hashBytes(&key, sizeof key));
    }
};

struct PipelineKeyEqual {
    bool operator()(const PipelineKey &a, const PipelineKey &b) const
    {
        return std::memcmp(&a, &b, sizeof a) == 0;
    }
};

struct ShaderKey {
    uint32_t fvf;
    uint32_t program_count;
    uint64_t program_hash;
    uint8_t coverage_only, scaled_depth_bias, sample_depth_bias, count_pass;
    uint32_t samples;
};

struct ShaderKeyHash {
    size_t operator()(const ShaderKey &key) const { return static_cast<size_t>(hashBytes(&key, sizeof key)); }
};

struct ShaderKeyEqual {
    bool operator()(const ShaderKey &a, const ShaderKey &b) const { return std::memcmp(&a, &b, sizeof a) == 0; }
};

struct QueuedDraw {
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    uint32_t vertex_offset = 0u;
    uint32_t index_offset = 0u;
    uint32_t index_count = 0u;
    SDL_GPUTexture *textures[4]{};
    SDL_GPUSampler *samplers[4]{};
    uint32_t uniform_offset = 0u;
    uint32_t program_offset = UINT32_MAX;
    SDL_FColor blend_constants{};
    float sampler_bias[4]{};  // Metal applies sampler LOD bias in the shader
    uint8_t stencil_ref = 0u;
    SDL_GPUViewport viewport{};
    SDL_Rect scissor{};
};

/* Draws for one bound target, recorded as one render pass when the target
   changes or something must observe what they drew. */
struct Segment {
    SDL_GPUTexture *color = nullptr;
    SDL_GPUTexture *depth = nullptr;
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;
    std::vector<float> uniform_data;
    std::vector<QueuedDraw> draws;
};

/* The bound target of a clear or draw, as the D3D11 backend's bindTarget. */
struct BoundTarget {
    SDL_GPUTexture *color = nullptr;
    SDL_GPUTexture *depth = nullptr;
    RenderTargetEntry *entry = nullptr;  // null for the main target
    uint32_t guest_width = 0u, guest_height = 0u;
    uint32_t host_width = 0u, host_height = 0u;
    SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
    float scale_x = 1.0f, scale_y = 1.0f;
};

/* An occlusion count: draws between BEGIN and END also add one per passing
   sample into count, which END resolves, reduces with mipmaps and reads back. */
struct VisibilityQuery {
    uint32_t index = 0u;
    SDL_GPUTexture *count = nullptr;       // counting target (the draws' sample count)
    SDL_GPUTexture *resolved = nullptr;    // count resolved, with MSAA
    SDL_GPUTexture *reduce = nullptr;      // R32F with a mip chain
    uint32_t width = 0u, height = 0u;      // count's host size
    SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
    float host_pixels_per_guest_pixel = 1.0f;
    SDL_GPUTransferBuffer *download = nullptr;
    uint32_t level = 0u, level_width = 0u, level_height = 0u;
    SDL_GPUFence *fence = nullptr;
};

} // namespace

struct RecompD3dPresenter {
    RecompD3dPresenterConfig config{};
    std::thread::id owner_thread;
    SDL_Window *window = nullptr;
    SDL_GPUDevice *device = nullptr;
    RecompShaderTarget shader_target = RecompShaderTarget::Spirv;
    SDL_GPUShaderFormat shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    bool close_requested = false;
    bool widescreen = false;
    float scale = 1.0f;
    SDL_GPUSampleCount msaa = SDL_GPU_SAMPLECOUNT_1;
    uint32_t msaa_count = 1u;

    SDL_GPUTexture *main_color = nullptr;    // the guest back buffer (multisampled with MSAA)
    SDL_GPUTexture *main_resolve = nullptr;  // main_color itself without MSAA
    SDL_GPUTexture *main_depth = nullptr;
    bool main_resolved = true;
    SDL_GPUTexture *back_buffer_copy = nullptr;   // GetBackBuffer(0) snapshots
    SDL_GPUTexture *previous_frame = nullptr;     // GetBackBuffer(-1)
    bool copy_mips = false;
    SDL_GPUTexture *white_texture = nullptr;
    SDL_GPUTexture *white_volume = nullptr;

    SDL_GPUSampler *draw_sampler = nullptr;       // linear, wrap
    SDL_GPUSampler *filter_sampler = nullptr;     // linear, point mips, clamp
    SDL_GPUSampler *program_mask_sampler = nullptr;
    SDL_GPUSampler *point_sampler = nullptr;
    std::vector<SamplerEntry> samplers;

    SDL_GPUBuffer *vertex_buffer = nullptr;
    SDL_GPUBuffer *index_buffer = nullptr;
    SDL_GPUTransferBuffer *vertex_transfer = nullptr;
    SDL_GPUTransferBuffer *index_transfer = nullptr;
    uint32_t vertex_capacity = 0u, index_capacity = 0u;
    uint32_t vertex_used = 0u, index_used = 0u;

    Segment segment;
    SDL_GPUCommandBuffer *command_buffer = nullptr;

    std::mutex shader_lock;
    std::unordered_map<ShaderKey, ShaderPair, ShaderKeyHash, ShaderKeyEqual> shaders;
    std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline *, PipelineKeyHash, PipelineKeyEqual> pipelines;
    std::thread precompile;
    std::atomic<bool> precompile_stop{false};

    std::vector<TextureEntry> textures = std::vector<TextureEntry>(kTextureSlots);
    std::unordered_multimap<uint32_t, uint32_t> texture_index;
    uint32_t texture_count = 0u;
    uint32_t next_texture_slot = 0u;
    // Textures replaced while queued draws still bind them; released after the next flush.
    std::vector<SDL_GPUTexture *> retired_textures;

    std::vector<RenderTargetEntry> render_targets;
    std::vector<DepthTargetEntry> depth_targets;
    uint64_t target_bytes = 0u;

    // SetGammaRamp applies when a frame is shown; guest reads stay uncorrected.
    bool gamma_enabled = false;
    float gamma_ramp[256][4]{};
    bool smaa = false;  // RECOMP_D3D_SMAA: post-process the output
    SDL_GPUGraphicsPipeline *smaa_pipelines[3]{};  // edges, weights, blend
    SDL_GPUTexture *smaa_textures[5]{};  // edges, area, search, weights, output
    SDL_GPUGraphicsPipeline *gamma_pipeline = nullptr;
    SDL_GPUTexture *gamma_target = nullptr;

    // The target of the last clear or draw: a visibility test's scale.
    BoundTarget last_bound;
    bool visibility_open = false;
    VisibilityQuery visibility_active;
    std::vector<VisibilityQuery> visibility_pending;
    std::vector<VisibilityQuery> visibility_free;

    uint32_t present_count = 0u;
    uint32_t draw_count = 0u;
    bool performance_counter = false;
    std::chrono::steady_clock::time_point second_start{}, last_present{};
    uint32_t second_presents = 0u;
    double second_max_ms = 0.0;
    unsigned frame_dump_count = 0u;
    uint64_t next_frame_dump_ms = 0u;
    const char *draw_log_trigger = nullptr;
    bool draw_log_active = false;
    unsigned draw_log_index = 0u;
};

namespace {

RecompD3dPresenter *active_presenter;
bool immediate_present;

uint32_t presentClientWidth(const RecompD3dPresenter *presenter, uint64_t height)
{
    return static_cast<uint32_t>(presenter->widescreen
        ? (height * 16u + 8u) / 9u
        : (height * 4u + 1u) / 3u);
}

uint32_t mainHeight(const RecompD3dPresenter *presenter)
{
    return static_cast<uint32_t>(presenter->config.height * presenter->scale + 0.5f);
}

uint32_t mainWidth(const RecompD3dPresenter *presenter)
{
    return presenter->scale == 1.0f
        ? presenter->config.width : presentClientWidth(presenter, mainHeight(presenter));
}

/* Offscreen scenes with their own depth (water reflections) scale too;
   depthless filter chains keep guest size, as in the D3D11 backend. */
uint32_t scaledSize(const RecompD3dPresenter *presenter, uint32_t guest)
{
    return std::min(static_cast<uint32_t>(guest * presenter->scale + 0.5f), kMaxTextureSize);
}

uint32_t sampleCount(SDL_GPUSampleCount samples)
{
    switch (samples) {
    case SDL_GPU_SAMPLECOUNT_2: return 2u;
    case SDL_GPU_SAMPLECOUNT_4: return 4u;
    case SDL_GPU_SAMPLECOUNT_8: return 8u;
    default: return 1u;
    }
}

// The guest paces itself to 60 Hz. On Linux (gamescope on the Steam Deck),
// MAILBOX presents without a second wait for the swapchain.
bool preferMailbox(bool immediate)
{
#ifdef __linux__
    (void)immediate;
    return true;
#else
    return immediate;
#endif
}

SDL_GPUCommandBuffer *commandBuffer(RecompD3dPresenter *presenter)
{
    if (presenter->command_buffer == nullptr) {
        presenter->command_buffer = SDL_AcquireGPUCommandBuffer(presenter->device);
    }
    return presenter->command_buffer;
}

void submitCommands(RecompD3dPresenter *presenter)
{
    if (presenter->command_buffer != nullptr) {
        SDL_SubmitGPUCommandBuffer(presenter->command_buffer);
        presenter->command_buffer = nullptr;
    }
}

SDL_GPUTexture *createTexture(RecompD3dPresenter *presenter, SDL_GPUTextureType type,
    SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, uint32_t width, uint32_t height,
    uint32_t depth = 1u, uint32_t levels = 1u, SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1)
{
    SDL_GPUTextureCreateInfo info{};
    info.type = type;
    info.format = format;
    info.usage = usage;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = depth;
    info.num_levels = levels;
    info.sample_count = samples;
    return SDL_CreateGPUTexture(presenter->device, &info);
}

/* Resolves a multisampled color target into its single-sample twin with an
   empty pass that loads and resolves. */
void resolveTarget(RecompD3dPresenter *presenter, SDL_GPUTexture *source, SDL_GPUTexture *destination)
{
    SDL_GPUColorTargetInfo color{};
    color.texture = source;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
    color.resolve_texture = destination;
    if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commandBuffer(presenter), &color, 1, nullptr)) {
        SDL_EndGPURenderPass(pass);
    }
}

void flushSegment(RecompD3dPresenter *presenter);

void resolveMain(RecompD3dPresenter *presenter)
{
    if (presenter->main_resolved) return;
    if (presenter->main_resolve != presenter->main_color) {
        resolveTarget(presenter, presenter->main_color, presenter->main_resolve);
    }
    presenter->main_resolved = true;
}

void unindexTexture(RecompD3dPresenter *presenter, uint32_t slot)
{
    const auto range = presenter->texture_index.equal_range(presenter->textures[slot].data);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == slot) {
            presenter->texture_index.erase(it);
            return;
        }
    }
}

void retireTexture(RecompD3dPresenter *presenter, SDL_GPUTexture *&texture)
{
    if (texture != nullptr) presenter->retired_textures.push_back(texture);
    texture = nullptr;
}

void releaseRetiredTextures(RecompD3dPresenter *presenter)
{
    for (SDL_GPUTexture *texture : presenter->retired_textures) {
        SDL_ReleaseGPUTexture(presenter->device, texture);
    }
    presenter->retired_textures.clear();
}

RenderTargetEntry *findRenderTarget(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &desc)
{
    if (desc.volume_depth != 0u) return nullptr;
    for (RenderTargetEntry &entry : presenter->render_targets) {
        if (entry.desc.data == desc.data && entry.desc.format_byte == desc.format_byte &&
            entry.desc.width == desc.width && entry.desc.height == desc.height) {
            return &entry;
        }
    }
    return nullptr;
}

RecompD3dPresenterError lookupDepthTarget(RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target, uint32_t color_width, uint32_t color_height,
    uint32_t host_width, uint32_t host_height, SDL_GPUTexture *&texture)
{
    texture = nullptr;
    if (target.no_depth) return RECOMP_D3D_PRESENTER_OK;
    const RecompD3dTextureDesc &desc = target.depth;
    const uint32_t width = target.custom_depth ? desc.width : presenter->config.width;
    const uint32_t height = target.custom_depth ? desc.height : presenter->config.height;
    if (width != color_width || height != color_height) {
        std::fprintf(stderr,
            "recomp d3d presenter: target size mismatch "
            "color=0x%08X fmt=0x%02X size=%ux%u "
            "depth=0x%08X fmt=0x%02X size=%ux%u custom_depth=%d\n",
            target.color.data, target.color.format_byte, color_width, color_height,
            desc.data, desc.format_byte, width, height, target.custom_depth ? 1 : 0);
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (!target.custom_depth) {
        texture = presenter->main_depth;
        return RECOMP_D3D_PRESENTER_OK;
    }
    if (!desc.depth || desc.data == 0u || width == 0u || height == 0u ||
        width > kMaxTextureSize || height > kMaxTextureSize ||
        (desc.format_byte != 0x2au && desc.format_byte != 0x2eu)) {
        std::fprintf(stderr,
            "recomp d3d presenter: unsupported depth target data=0x%08X "
            "fmt=0x%02X size=%ux%u depth=%d\n",
            desc.data, desc.format_byte, width, height, desc.depth ? 1 : 0);
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    /* A custom depth bound with the main target must match its host size and samples. */
    const bool host_main = color_width == presenter->config.width &&
        color_height == presenter->config.height &&
        (presenter->scale != 1.0f || presenter->msaa_count > 1u);
    const SDL_GPUSampleCount samples = host_main ? presenter->msaa : SDL_GPU_SAMPLECOUNT_1;
    for (const DepthTargetEntry &entry : presenter->depth_targets) {
        if (entry.desc.data == desc.data && entry.desc.format_byte == desc.format_byte &&
            entry.desc.width == width && entry.desc.height == height &&
            entry.host_main == host_main && entry.host_width == host_width) {
            texture = entry.texture;
            return RECOMP_D3D_PRESENTER_OK;
        }
    }
    // Like the main color target, a main-sized depth is outside the offscreen budget.
    const uint64_t bytes = host_main ? 0u : static_cast<uint64_t>(width) * height * 4u * sampleCount(samples);
    if (bytes > kTargetByteLimit - presenter->target_bytes) {
        std::fprintf(stderr, "recomp d3d presenter: target memory budget exhausted\n");
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    texture = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, presenter->depth_format,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, host_width, host_height, 1u, 1u, samples);
    if (texture == nullptr) {
        std::fprintf(stderr,
            "recomp d3d presenter: depth target create failed data=0x%08X fmt=0x%02X size=%ux%u: %s\n",
            desc.data, desc.format_byte, width, height, SDL_GetError());
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    presenter->depth_targets.push_back({desc, texture, host_main, bytes, host_width});
    presenter->target_bytes += bytes;
    std::fprintf(stderr, "recomp d3d presenter: depth target data=0x%08X fmt=0x%02X size=%ux%u\n",
        desc.data, desc.format_byte, host_width, host_height);
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError bindTarget(RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target, BoundTarget &bound)
{
    bound = {};
    bound.color = presenter->main_color;
    uint32_t width = presenter->config.width, height = presenter->config.height;
    uint32_t host_width = mainWidth(presenter), host_height = mainHeight(presenter);
    // Full-frame effects must keep the main depth's dimensions and samples.
    const bool host_main = !target.offscreen ||
        (target.color.width == width && target.color.height == height);
    const SDL_GPUSampleCount samples = host_main ? presenter->msaa : SDL_GPU_SAMPLECOUNT_1;

    if (target.offscreen) {
        const RecompD3dTextureDesc &desc = target.color;
        if (desc.depth || desc.data == 0u || desc.width == 0u || desc.height == 0u ||
            desc.width > kMaxTextureSize || desc.height > kMaxTextureSize ||
            (desc.format_byte != RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 && desc.format_byte != 0x12u)) {
            std::fprintf(stderr,
                "recomp d3d presenter: unsupported target data=0x%08X fmt=0x%02X size=%ux%u no_depth=%d\n",
                desc.data, desc.format_byte, desc.width, desc.height, target.no_depth ? 1 : 0);
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
        RenderTargetEntry *entry = findRenderTarget(presenter, desc);
        if (entry == nullptr) {
            const bool scaled = !host_main && target.custom_depth;
            host_width = scaled ? scaledSize(presenter, desc.width) : host_main ? host_width : desc.width;
            host_height = scaled ? scaledSize(presenter, desc.height) : host_main ? host_height : desc.height;
            // Budget guest storage, as with main-sized depth; quality must not reduce capacity.
            const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
            if (bytes > kTargetByteLimit - presenter->target_bytes) {
                std::fprintf(stderr, "recomp d3d presenter: target memory budget exhausted\n");
                return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
            }
            RenderTargetEntry created{};
            created.desc = desc;
            created.multisampled = samples != SDL_GPU_SAMPLECOUNT_1;
            created.samples = samples;
            created.bytes = bytes;
            created.host_width = host_width;
            created.host_height = host_height;
            created.render = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (created.multisampled ? 0u : SDL_GPU_TEXTUREUSAGE_SAMPLER),
                host_width, host_height, 1u, 1u, samples);
            created.resolve = !created.multisampled ? created.render : createTexture(presenter,
                SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, host_width, host_height);
            if (created.render == nullptr || created.resolve == nullptr) {
                std::fprintf(stderr,
                    "recomp d3d presenter: render target create failed data=0x%08X fmt=0x%02X size=%ux%u: %s\n",
                    desc.data, desc.format_byte, desc.width, desc.height, SDL_GetError());
                if (created.resolve != nullptr && created.resolve != created.render)
                    SDL_ReleaseGPUTexture(presenter->device, created.resolve);
                if (created.render != nullptr) SDL_ReleaseGPUTexture(presenter->device, created.render);
                return RECOMP_D3D_PRESENTER_HOST_FAILURE;
            }
            presenter->render_targets.push_back(created);
            presenter->target_bytes += bytes;
            entry = &presenter->render_targets.back();
            std::fprintf(stderr, "recomp d3d presenter: render target data=0x%08X fmt=0x%02X size=%ux%u\n",
                desc.data, desc.format_byte, desc.width, desc.height);
        }
        bound.color = entry->render;
        bound.entry = entry;
        width = desc.width;
        height = desc.height;
        host_width = entry->host_width;
        host_height = entry->host_height;
    }
    const RecompD3dPresenterError depth = lookupDepthTarget(presenter, target, width, height,
        host_width, host_height, bound.depth);
    if (depth != RECOMP_D3D_PRESENTER_OK) return depth;
    bound.guest_width = width;
    bound.guest_height = height;
    bound.host_width = host_width;
    bound.host_height = host_height;
    bound.samples = samples;
    bound.scale_x = static_cast<float>(host_width) / width;
    bound.scale_y = static_cast<float>(host_height) / height;
    return RECOMP_D3D_PRESENTER_OK;
}

/* Marks a bound target written, so its next sampling resolves it first. */
void markWritten(RecompD3dPresenter *presenter, const BoundTarget &bound)
{
    if (bound.entry != nullptr) bound.entry->resolved = !bound.entry->multisampled;
    else presenter->main_resolved = presenter->main_resolve == presenter->main_color;
}

SDL_GPUSamplerAddressMode hostAddressMode(uint32_t mode)
{
    /* Xbox D3DTADDRESS WRAP, MIRROR, CLAMP, BORDER, CLAMPTOEDGE. SDL_GPU has no
       border color; the draw adapter stops on BORDER before it gets here. */
    switch (mode) {
    case 2u: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 3u:
    case 4u:
    case 5u: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
}

SDL_GPUSampler *createSampler(RecompD3dPresenter *presenter, SDL_GPUSamplerAddressMode u,
    SDL_GPUSamplerAddressMode v, SDL_GPUSamplerAddressMode w, bool point_mip_filter, float bias,
    SDL_GPUFilter filter = SDL_GPU_FILTER_LINEAR)
{
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = filter;
    info.mag_filter = filter;
    info.mipmap_mode = point_mip_filter ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    info.address_mode_u = u;
    info.address_mode_v = v;
    info.address_mode_w = w;
    info.mip_lod_bias = bias;
    info.max_lod = 1000.0f;  // the zero default would sample only the base mip level
    return SDL_CreateGPUSampler(presenter->device, &info);
}

/* Zero-initialized draw commands keep the linear/wrap sampler. */
SDL_GPUSampler *lookupDrawSampler(RecompD3dPresenter *presenter, uint32_t address_u, uint32_t address_v,
    bool point_mip_filter, float bias, uint32_t address_w = 1u)
{
    const SDL_GPUSamplerAddressMode u = hostAddressMode(address_u), v = hostAddressMode(address_v),
        w = hostAddressMode(address_w);
    if (u == SDL_GPU_SAMPLERADDRESSMODE_REPEAT && v == SDL_GPU_SAMPLERADDRESSMODE_REPEAT &&
        w == SDL_GPU_SAMPLERADDRESSMODE_REPEAT && !point_mip_filter && bias == 0.0f) {
        return presenter->draw_sampler;
    }
    for (const SamplerEntry &entry : presenter->samplers) {
        if (entry.u == u && entry.v == v && entry.w == w &&
            entry.point_mip_filter == point_mip_filter && entry.mip_lod_bias == bias) {
            return entry.sampler;
        }
    }
    SDL_GPUSampler *sampler = createSampler(presenter, u, v, w, point_mip_filter, bias);
    if (sampler != nullptr) presenter->samplers.push_back({u, v, w, point_mip_filter, bias, sampler});
    return sampler;
}

/* Xbox block-compressed formats map straight onto the host equivalents. A8
   becomes white BGRA, as NV2A supplies white RGB with it. */
SDL_GPUTextureFormat hostTextureFormat(uint32_t format_byte)
{
    switch (format_byte) {
    case RECOMP_D3D_TEXTURE_FORMAT_DXT1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT3: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_DXT5: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8:
    case RECOMP_D3D_TEXTURE_FORMAT_P8:
    case RECOMP_D3D_TEXTURE_FORMAT_A8: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case 0x28u:
        /* V8U8: U in the low byte, V in the high byte, read signed.
           shortcut: SNORM scale is unverified against NV2A, as on D3D11. */
        return SDL_GPU_TEXTUREFORMAT_R8G8_SNORM;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

bool isSwizzledTextureFormat(uint32_t format_byte)
{
    return format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 ||
        format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 || format_byte == 0x28u;
}

/* Uploads one region of bytes, rows packed at pixels_per_row texels. */
bool uploadTexture(RecompD3dPresenter *presenter, SDL_GPUTexture *texture, const void *bytes,
    uint32_t size, uint32_t level, uint32_t width, uint32_t height, uint32_t depth = 1u,
    uint32_t pixels_per_row = 0u)
{
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(presenter->device, &info);
    if (transfer == nullptr) return false;
    void *mapped = SDL_MapGPUTransferBuffer(presenter->device, transfer, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(presenter->device, transfer);
        return false;
    }
    std::memcpy(mapped, bytes, size);
    SDL_UnmapGPUTransferBuffer(presenter->device, transfer);
    SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commandBuffer(presenter));
    if (pass != nullptr) {
        SDL_GPUTextureTransferInfo source{transfer, 0u, pixels_per_row, depth > 1u ? height : 0u};
        SDL_GPUTextureRegion region{texture, level, 0u, 0u, 0u, 0u, width, height, depth};
        SDL_UploadToGPUTexture(pass, &source, &region, false);
        SDL_EndGPUCopyPass(pass);
    }
    SDL_ReleaseGPUTransferBuffer(presenter->device, transfer);
    return pass != nullptr;
}

/* Copies the guest back buffer (resolving it) into a snapshot texture. */
void copyGuestBuffer(RecompD3dPresenter *presenter, SDL_GPUTexture *target, bool mips = true)
{
    resolveMain(presenter);
    if (SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commandBuffer(presenter))) {
        SDL_GPUTextureLocation source{presenter->main_resolve, 0u, 0u, 0u, 0u, 0u};
        SDL_GPUTextureLocation destination{target, 0u, 0u, 0u, 0u, 0u};
        SDL_CopyGPUTextureToTexture(pass, &source, &destination,
            mainWidth(presenter), mainHeight(presenter), 1u, false);
        SDL_EndGPUCopyPass(pass);
    }
    if (mips && presenter->copy_mips) SDL_GenerateMipmapsForGPUTexture(commandBuffer(presenter), target);
}

/* A main-sized offscreen target has the main target's host size, so the
   resolved frame copies whole into the view that draws sample. */
RecompD3dPresenterError submitCopyMainColor(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &destination)
{
    if (destination.width != presenter->config.width || destination.height != presenter->config.height)
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    RecompD3dPresenterTarget target{};
    target.offscreen = true;
    target.no_depth = true;
    target.color = destination;
    BoundTarget bound;
    const RecompD3dPresenterError error = bindTarget(presenter, target, bound);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    // The snapshot must hold every queued draw to the back buffer.
    flushSegment(presenter);
    copyGuestBuffer(presenter, bound.entry->resolve, false);
    /* shortcut: with MSAA only the resolved view holds the copy, so a later draw
       into this target would resolve over it; copy the samples if a game does. */
    bound.entry->resolved = true;
    return RECOMP_D3D_PRESENTER_OK;
}

SDL_GPUTexture *lookupBackBufferTexture(RecompD3dPresenter *presenter,
    const RecompD3dTextureDesc &desc, bool previous_frame)
{
    /* 3D scenes use a horizontally supersampled guest back buffer (1440x480
       for 720x480 output). The host keeps one resolved buffer, and linear
       UVs are normalised by the guest size, so whole multiples map onto it. */
    if (desc.format_byte != 0x12u || !desc.linear || desc.depth ||
        desc.width == 0u || desc.height == 0u ||
        desc.width % presenter->config.width != 0u ||
        desc.height % presenter->config.height != 0u) return nullptr;
    SDL_GPUTexture *&copy = previous_frame ? presenter->previous_frame : presenter->back_buffer_copy;
    const bool created = copy == nullptr;
    if (created) {
        /* An upscaled copy keeps mips so a guest downsample (the 256x256
           depth-of-field source) averages its footprint like the Xbox did. */
        uint32_t levels = 1u;
        if (presenter->copy_mips) {
            while ((std::max(mainWidth(presenter), mainHeight(presenter)) >> levels) != 0u) ++levels;
        }
        copy = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1u ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0u),
            mainWidth(presenter), mainHeight(presenter), 1u, levels);
        if (copy == nullptr) return nullptr;
    }
    /* Present refreshes the previous frame; until the first one, use this frame. */
    if (previous_frame && !created) return copy;
    // The snapshot must hold every queued draw to the back buffer.
    flushSegment(presenter);
    copyGuestBuffer(presenter, copy);
    return copy;
}

/* Draw-time tally of the formats a draw actually sampled, keyed by format
   byte, plus how many draws wanted a texture and got none. */
struct DrawTextureTally {
    uint32_t textured[256];
    uint32_t rejected[256];
    uint32_t untextured;
};

DrawTextureTally draw_texture_tally;

void countDrawTexture(const RecompD3dPresenterDrawCommand &draw, bool sampled)
{
    if (!draw.has_texture) {
        ++draw_texture_tally.untextured;
    } else if (sampled) {
        ++draw_texture_tally.textured[draw.texture.format_byte & 0xffu];
    } else {
        ++draw_texture_tally.rejected[draw.texture.format_byte & 0xffu];
    }
}

SDL_GPUTexture *lookupTexture(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const RecompD3dTextureDesc &desc = draw.texture;
    const bool volume = desc.volume_depth != 0u;
    if (volume && (desc.linear || desc.depth || desc.format_byte != 6u ||
        desc.bits_per_pixel != 32u || desc.mip_levels > 1u ||
        desc.width == 0u || desc.height == 0u ||
        desc.width > kMaxVolumeSize || desc.height > kMaxVolumeSize || desc.volume_depth > kMaxVolumeSize ||
        static_cast<uint64_t>(desc.width) * desc.height * desc.volume_depth * 4u > draw.texture_byte_count))
        return nullptr;
    // Only the bump pass reads 0x28, with COLORSIGN; its unsigned uses stay unsampled.
    if (desc.format_byte == 0x28u && !draw.bump_env) return nullptr;
    const bool palettized = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8;
    const bool linear_bgra = desc.format_byte == 0x12u;
    const bool compressed = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ||
        desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 || desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5;
    const uint32_t levels = compressed && desc.mip_levels ? desc.mip_levels : 1u;
    if (compressed) {
        const uint32_t span = recomp_d3d_texture_compressed_mip_span(&desc);
        if (span == 0u || span > draw.texture_byte_count ||
            desc.width > kMaxTextureSize || desc.height > kMaxTextureSize) return nullptr;
    }

    if (draw.texture_is_backbuffer) {
        SDL_GPUTexture *texture = lookupBackBufferTexture(presenter, desc, draw.texture_is_front_buffer);
        static bool reported;
        if (texture == nullptr && !reported) {
            reported = true;
            std::fprintf(stderr, "recomp d3d: frame-buffer texture %ux%u format=0x%x has no host view;"
                " the draw samples nothing\n", desc.width, desc.height, desc.format_byte);
        }
        return texture;
    }
    if (palettized && (draw.palette_bytes == nullptr || draw.palette_byte_count != kPaletteBytes)) {
        return nullptr;
    }
    if (RenderTargetEntry *entry = findRenderTarget(presenter, desc)) {
        // Full-frame effects sample one target many times per frame; resolve after writes only.
        if (entry->multisampled && !entry->resolved) {
            if (presenter->segment.color == entry->render) flushSegment(presenter);
            resolveTarget(presenter, entry->render, entry->resolve);
            entry->resolved = true;
        }
        return entry->resolve;
    }
    if (linear_bgra && (!desc.linear || desc.depth ||
        desc.bits_per_pixel != 32u || desc.width == 0u || desc.height == 0u ||
        desc.width > kMaxTextureSize || desc.height > kMaxTextureSize ||
        desc.pitch < desc.width * 4u || draw.texture_bytes == nullptr ||
        static_cast<uint64_t>(desc.height - 1u) * desc.pitch + desc.width * 4u > draw.texture_byte_count)) {
        return nullptr;
    }
    const auto cached = presenter->texture_index.equal_range(desc.data);
    const bool fingerprinted = !linear_bgra && draw.texture_bytes != nullptr;
    const uint64_t fingerprint = fingerprinted
        ? textureFingerprint(draw.texture_bytes, draw.texture_byte_count) : 0u;
    for (auto it = cached.first; it != cached.second; ++it) {
        TextureEntry &entry = presenter->textures[it->second];
        if (entry.format_byte == desc.format_byte && entry.width == desc.width &&
            entry.height == desc.height && entry.volume_depth == desc.volume_depth &&
            entry.mip_levels == levels &&
            (!palettized || std::memcmp(entry.palette, draw.palette_bytes, kPaletteBytes) == 0)) {
            if (fingerprinted && entry.fingerprint != fingerprint) {
                static unsigned reported;
                if (reported < 32u) {
                    ++reported;
                    std::fprintf(stderr, "recomp d3d presenter: texels changed data=0x%08X "
                        "fmt=0x%02X size=%ux%u\n", desc.data, desc.format_byte, desc.width, desc.height);
                }
                unindexTexture(presenter, it->second);
                retireTexture(presenter, entry.texture);
                entry.used = false;
                break;
            }
            if (linear_bgra) {
                // Queued draws sampled the old pixels; record them before this upload.
                flushSegment(presenter);
                std::vector<uint8_t> packed(static_cast<size_t>(desc.width) * desc.height * 4u);
                for (uint32_t y = 0u; y < desc.height; ++y) {
                    std::memcpy(packed.data() + static_cast<size_t>(y) * desc.width * 4u,
                        static_cast<const uint8_t *>(draw.texture_bytes) + static_cast<size_t>(y) * desc.pitch,
                        desc.width * 4u);
                }
                uploadTexture(presenter, entry.texture, packed.data(),
                    static_cast<uint32_t>(packed.size()), 0u, desc.width, desc.height);
            }
            return entry.texture;
        }
    }
    const SDL_GPUTextureFormat format = volume ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
        : linear_bgra ? SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM : hostTextureFormat(desc.format_byte);
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID || draw.texture_bytes == nullptr ||
        draw.texture_byte_count == 0u || (desc.linear && !linear_bgra)) {
        return nullptr;
    }

    std::vector<uint8_t> unpacked;
    if (volume) {
        unpacked.resize(static_cast<size_t>(desc.width) * desc.height * desc.volume_depth * 4u);
        if (!recomp_d3d_texture_unswizzle_volume(static_cast<const uint8_t *>(draw.texture_bytes),
                unpacked.data(), desc.width, desc.height, desc.volume_depth, 4u)) return nullptr;
        /* RGBA8 is supported for native 3D textures; guest words are BGRA. */
        for (size_t i = 0u; i < unpacked.size(); i += 4u) std::swap(unpacked[i], unpacked[i + 2u]);
    } else if (linear_bgra) {
        unpacked.resize(static_cast<size_t>(desc.width) * desc.height * 4u);
        for (uint32_t y = 0u; y < desc.height; ++y) {
            std::memcpy(unpacked.data() + static_cast<size_t>(y) * desc.width * 4u,
                static_cast<const uint8_t *>(draw.texture_bytes) + static_cast<size_t>(y) * desc.pitch,
                desc.width * 4u);
        }
    } else if (palettized) {
        const size_t texels = static_cast<size_t>(desc.width) * desc.height;
        if (desc.width == 0u || desc.height == 0u || desc.width > kMaxTextureSize ||
            desc.height > kMaxTextureSize || texels > draw.texture_byte_count) return nullptr;
        std::vector<uint8_t> indices(texels);
        if (!recomp_d3d_texture_unswizzle(static_cast<const uint8_t *>(draw.texture_bytes),
                indices.data(), desc.width, desc.height, 1u)) return nullptr;
        unpacked.resize(texels * 4u);
        const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
        for (size_t i = 0u; i < texels; ++i) {
            /* Little-endian ARGB palette words are already BGRA bytes. */
            std::memcpy(unpacked.data() + i * 4u, palette + indices[i] * 4u, 4u);
        }
    } else if (isSwizzledTextureFormat(desc.format_byte)) {
        const uint32_t texel_bytes = desc.bits_per_pixel / 8u;
        if (texel_bytes == 0u || desc.width == 0u || desc.height == 0u ||
            desc.width > kMaxTextureSize || desc.height > kMaxTextureSize) return nullptr;
        unpacked.resize(static_cast<size_t>(desc.width) * desc.height * texel_bytes);
        if (!recomp_d3d_texture_unswizzle(static_cast<const uint8_t *>(draw.texture_bytes),
                unpacked.data(), desc.width, desc.height, texel_bytes)) return nullptr;
        if (desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8) {
            std::vector<uint8_t> white(unpacked.size() * 4u, 0xffu);
            for (size_t i = 0u; i < unpacked.size(); ++i) white[i * 4u + 3u] = unpacked[i];
            unpacked.swap(white);
        }
    }

    SDL_GPUTexture *texture = createTexture(presenter,
        volume ? SDL_GPU_TEXTURETYPE_3D : SDL_GPU_TEXTURETYPE_2D, format, SDL_GPU_TEXTUREUSAGE_SAMPLER,
        desc.width, desc.height, volume ? desc.volume_depth : 1u, levels);
    if (texture == nullptr) return nullptr;
    bool uploaded = true;
    if (compressed) {
        const uint32_t block_bytes = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? 8u : 16u;
        uint32_t width = desc.width, height = desc.height, offset = 0u;
        for (uint32_t level = 0u; level < levels && uploaded; ++level) {
            const uint32_t size = ((width + 3u) / 4u) * block_bytes * ((height + 3u) / 4u);
            uploaded = uploadTexture(presenter, texture,
                static_cast<const uint8_t *>(draw.texture_bytes) + offset, size, level, width, height);
            offset += size;
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
    } else {
        uploaded = uploadTexture(presenter, texture, unpacked.data(), static_cast<uint32_t>(unpacked.size()),
            0u, desc.width, desc.height, volume ? desc.volume_depth : 1u);
    }
    if (!uploaded) {
        SDL_ReleaseGPUTexture(presenter->device, texture);
        return nullptr;
    }

    // ponytail: FIFO eviction; track recent use if upload churn is costly.
    const uint32_t slot = presenter->next_texture_slot;
    TextureEntry &entry = presenter->textures[slot];
    if (entry.used) unindexTexture(presenter, slot);
    retireTexture(presenter, entry.texture);
    presenter->next_texture_slot = (presenter->next_texture_slot + 1u) % kTextureSlots;
    if (presenter->texture_count < kTextureSlots) ++presenter->texture_count;
    entry.used = true;
    entry.data = desc.data;
    entry.format_byte = desc.format_byte;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.volume_depth = desc.volume_depth;
    entry.mip_levels = levels;
    if (palettized) std::memcpy(entry.palette, draw.palette_bytes, kPaletteBytes);
    entry.fingerprint = fingerprint;
    entry.texture = texture;
    presenter->texture_index.emplace(desc.data, slot);
    return texture;
}

/* ---- Shaders and pipelines ------------------------------------------------ */

SDL_GPUShader *createShader(RecompD3dPresenter *presenter, const std::string &hlsl, bool vertex,
    const std::vector<std::string> &defines, uint32_t samplers, uint32_t uniform_buffers)
{
    RecompCompiledShader compiled;
    std::string error;
    if (!recomp_shader_compile(hlsl, vertex ? "vs_main" : "ps_main", vertex, presenter->shader_target,
            defines, compiled, error)) {
        std::fprintf(stderr, "recomp d3d presenter: draw shader %s failed: %s\n",
            vertex ? "vs_main" : "ps_main", error.c_str());
        return nullptr;
    }
    SDL_GPUShaderCreateInfo info{};
    info.code = compiled.code.data();
    info.code_size = compiled.code.size();
    info.entrypoint = compiled.entry_point.c_str();
    info.format = presenter->shader_format;
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniform_buffers;
    SDL_GPUShader *shader = SDL_CreateGPUShader(presenter->device, &info);
    if (shader == nullptr) {
        std::fprintf(stderr, "recomp d3d presenter: SDL rejected the %s shader: %s\n",
            vertex ? "vertex" : "fragment", SDL_GetError());
    }
    return shader;
}

/* The draw shader for one key. A count-pass variant outputs one per fragment
   that survives its discards, for occlusion counting. Failures are cached
   too, so a shader that cannot build is not retried every draw. */
ShaderPair lookupShaders(RecompD3dPresenter *presenter, const RecompD3dDrawShaderKey &draw_key,
    bool count_pass)
{
    ShaderKey key{};
    key.fvf = draw_key.fvf;
    key.program_count = draw_key.program_count;
    key.program_hash = draw_key.program_count ? hashBytes(draw_key.program, draw_key.program_count * 16u) : 0u;
    key.coverage_only = draw_key.coverage_only;
    key.scaled_depth_bias = draw_key.scaled_depth_bias;
    key.sample_depth_bias = draw_key.sample_depth_bias;
    key.count_pass = count_pass;
    key.samples = draw_key.sample_depth_bias ? presenter->msaa_count : 0u;
    {
        std::lock_guard<std::mutex> lock(presenter->shader_lock);
        const auto found = presenter->shaders.find(key);
        if (found != presenter->shaders.end()) return found->second;
    }
    const auto start = std::chrono::steady_clock::now();
    ShaderPair pair;
    std::string hlsl;
    if (recomp_d3d_draw_shader_source(draw_key, pair.layout, hlsl)) {
        hlsl = recomp_shader_draw_bindings(hlsl);
        if (presenter->shader_target == RecompShaderTarget::Msl) hlsl = recomp_shader_emulate_sampler_bias(hlsl);
        if (count_pass) {
            static const std::string result = "    return shaded;\n";
            const size_t at = hlsl.rfind(result);
            if (at != std::string::npos) hlsl.replace(at, result.size(), "    return float4(1, 1, 1, 1);\n");
        }
        std::vector<std::string> defines;
        if (draw_key.sample_depth_bias) defines.push_back("RECOMP_SAMPLE_COUNT=" + std::to_string(presenter->msaa_count));
        pair.vertex = createShader(presenter, hlsl, true, defines, 0u, 2u);
        pair.fragment = pair.vertex ? createShader(presenter, hlsl, false, defines, 4u,
            presenter->shader_target == RecompShaderTarget::Msl ? 2u : 1u) : nullptr;
        if (pair.fragment == nullptr && pair.vertex != nullptr) {
            SDL_ReleaseGPUShader(presenter->device, pair.vertex);
            pair.vertex = nullptr;
        }
    } else if (draw_key.program_count) {
        std::fprintf(stderr, "recomp d3d presenter: vertex program untranslated fvf=%03x count=%u\n",
            draw_key.fvf, draw_key.program_count);
    }
    std::fprintf(stderr, "recomp d3d presenter: draw shader fvf=0x%03X program=%u%s%s in %lld ms\n",
        draw_key.fvf, draw_key.program_count, count_pass ? " count" : "",
        pair.vertex != nullptr ? "" : " failed",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
    std::lock_guard<std::mutex> lock(presenter->shader_lock);
    const auto inserted = presenter->shaders.emplace(key, pair);
    if (!inserted.second) {
        // The boot precompile finished the same key first; keep its pair.
        if (pair.vertex) SDL_ReleaseGPUShader(presenter->device, pair.vertex);
        if (pair.fragment) SDL_ReleaseGPUShader(presenter->device, pair.fragment);
    }
    return inserted.first->second;
}

// ponytail: fixed list; vertex-program shaders and unlisted FVFs still compile on first use.
void precompileDrawShaders(RecompD3dPresenter *presenter)
{
    const auto start = std::chrono::steady_clock::now();
    unsigned compiled = 0u;
    for (const uint32_t fvf : kRecompD3dBootDrawFvfs) {
        if (presenter->precompile_stop.load()) return;
        const RecompD3dDrawShaderKey key{fvf, 0u, nullptr, false, false, false};
        compiled += lookupShaders(presenter, key, false).vertex != nullptr;
    }
    std::fprintf(stderr, "recomp d3d presenter: precompiled %u draw shaders in %lld ms\n", compiled,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
}

SDL_GPUBlendFactor hostBlendFactor(RecompD3dBlendFactor factor, bool alpha_channel)
{
    switch (factor) {
    case RECOMP_D3D_BLEND_ZERO: return SDL_GPU_BLENDFACTOR_ZERO;
    case RECOMP_D3D_BLEND_CONSTANT_COLOR:
    case RECOMP_D3D_BLEND_CONSTANT_ALPHA: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case RECOMP_D3D_BLEND_INV_CONSTANT_COLOR:
    case RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    /* The alpha blend equation may only name alpha operands. */
    case RECOMP_D3D_BLEND_SRC_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_SRC_ALPHA : SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case RECOMP_D3D_BLEND_INV_SRC_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case RECOMP_D3D_BLEND_INV_SRC_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case RECOMP_D3D_BLEND_DST_ALPHA: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case RECOMP_D3D_BLEND_INV_DST_ALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case RECOMP_D3D_BLEND_DST_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_DST_ALPHA : SDL_GPU_BLENDFACTOR_DST_COLOR;
    case RECOMP_D3D_BLEND_INV_DST_COLOR:
        return alpha_channel ? SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA : SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA_SATURATE: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case RECOMP_D3D_BLEND_ONE:
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

SDL_GPUBlendOp hostBlendOp(RecompD3dBlendOp op)
{
    switch (op) {
    case RECOMP_D3D_BLEND_OP_MIN: return SDL_GPU_BLENDOP_MIN;
    case RECOMP_D3D_BLEND_OP_MAX: return SDL_GPU_BLENDOP_MAX;
    case RECOMP_D3D_BLEND_OP_SUBTRACT: return SDL_GPU_BLENDOP_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_ADD:
    default: return SDL_GPU_BLENDOP_ADD;
    }
}

SDL_GPUCompareOp hostCompareOp(RecompD3dCompareFunc func)
{
    switch (func) {
    case RECOMP_D3D_COMPARE_NEVER: return SDL_GPU_COMPAREOP_NEVER;
    case RECOMP_D3D_COMPARE_LESS: return SDL_GPU_COMPAREOP_LESS;
    case RECOMP_D3D_COMPARE_EQUAL: return SDL_GPU_COMPAREOP_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER: return SDL_GPU_COMPAREOP_GREATER;
    case RECOMP_D3D_COMPARE_NOT_EQUAL: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER_EQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case RECOMP_D3D_COMPARE_ALWAYS: return SDL_GPU_COMPAREOP_ALWAYS;
    case RECOMP_D3D_COMPARE_LESS_EQUAL:
    default: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    }
}

SDL_GPUStencilOp hostStencilOp(RecompD3dStencilOp op)
{
    switch (op) {
    case RECOMP_D3D_STENCIL_ZERO: return SDL_GPU_STENCILOP_ZERO;
    case RECOMP_D3D_STENCIL_REPLACE: return SDL_GPU_STENCILOP_REPLACE;
    case RECOMP_D3D_STENCIL_INCRSAT: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case RECOMP_D3D_STENCIL_DECRSAT: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case RECOMP_D3D_STENCIL_INVERT: return SDL_GPU_STENCILOP_INVERT;
    case RECOMP_D3D_STENCIL_INCRWRAP: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case RECOMP_D3D_STENCIL_DECRWRAP: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    case RECOMP_D3D_STENCIL_KEEP:
    default: return SDL_GPU_STENCILOP_KEEP;
    }
}

/* Vertex inputs in the shader's VSIn order, which recomp_shader_draw_bindings
   numbers from zero. */
uint32_t vertexAttributes(const RecompD3dVertexLayout &layout, bool coverage_only,
    SDL_GPUVertexAttribute (&attributes)[8])
{
    uint32_t count = 0u;
    const auto add = [&](SDL_GPUVertexElementFormat format, uint32_t offset) {
        attributes[count] = {count, 0u, format, offset};
        ++count;
    };
    add(layout.pretransformed ? SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4 : SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,
        layout.position_offset);
    if (layout.blend_weight_count != 0u) {
        const SDL_GPUVertexElementFormat formats[] = {SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,
            SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3};
        add(formats[layout.blend_weight_count - 1u], 12u);
    }
    if (coverage_only) return count;
    if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT) add(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, layout.normal_offset);
    if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) add(SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, layout.diffuse_offset);
    const uint32_t texture_coords = layout.texcoord_count == 4u ? 4u
        : layout.texcoord_count == 2u ? 2u : (layout.texcoord_count != 0u ? 1u : 0u);
    for (uint32_t i = 0u; i < texture_coords; ++i) {
        add(SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, layout.texcoord_offset + i * 8u);
    }
    return count;
}

struct PipelineState {
    const RecompD3dPresenterDrawCommand *draw;
    ShaderPair shaders;
    uint32_t fvf;
    uint32_t stride;
    SDL_GPUPrimitiveType primitive;
    RecompD3dCullMode cull;
    SDL_GPUTextureFormat color_format;
    SDL_GPUTextureFormat depth_format;  // INVALID without depth
    SDL_GPUSampleCount samples;
    float slope_bias;
    bool coverage_only;
    bool count_pass;
};

SDL_GPUGraphicsPipeline *lookupPipeline(RecompD3dPresenter *presenter, const PipelineState &state)
{
    const RecompD3dPresenterDrawCommand &draw = *state.draw;
    PipelineKey key;
    std::memset(&key, 0, sizeof key);
    key.vertex = state.shaders.vertex;
    key.fragment = state.shaders.fragment;
    key.fvf = state.fvf;
    key.stride = state.stride;
    key.primitive = state.primitive;
    key.cull = state.cull;
    key.color_format = state.color_format;
    key.depth_format = state.depth_format;
    key.samples = state.samples;
    key.depth_bias = draw.depth_bias;
    key.slope_bias = state.slope_bias;
    key.coverage_only = state.coverage_only;
    key.count_pass = state.count_pass;
    key.depth_test = draw.depth.depth_test_enable;
    key.depth_write = draw.depth.depth_write_enable && !state.count_pass;
    key.depth_func = static_cast<uint8_t>(draw.depth.depth_func);
    key.stencil_enable = draw.depth.stencil_enable;
    if (key.stencil_enable) {
        key.stencil_read = static_cast<uint8_t>(draw.depth.stencil_read_mask);
        key.stencil_write = state.count_pass ? 0u : static_cast<uint8_t>(draw.depth.stencil_write_mask);
        key.stencil_func = static_cast<uint8_t>(draw.depth.stencil_func);
        key.stencil_fail = static_cast<uint8_t>(draw.depth.stencil_fail);
        key.stencil_zfail = static_cast<uint8_t>(draw.depth.stencil_zfail);
        key.stencil_pass = static_cast<uint8_t>(draw.depth.stencil_pass);
    }
    if (state.count_pass) {
        key.blend_enable = 1u;
        key.write_mask = 0x1u;
    } else if (!state.coverage_only) {
        key.blend_enable = draw.blend.blend_enable;
        key.write_mask = draw.blend.color_write_mask;
        if (key.blend_enable) {
            key.src_factor = static_cast<uint8_t>(draw.blend.src_factor);
            key.dst_factor = static_cast<uint8_t>(draw.blend.dst_factor);
            key.blend_op = static_cast<uint8_t>(draw.blend.op);
        }
    }
    const auto found = presenter->pipelines.find(key);
    if (found != presenter->pipelines.end()) return found->second;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = state.shaders.vertex;
    info.fragment_shader = state.shaders.fragment;
    info.primitive_type = state.primitive;
    info.multisample_state.sample_count = state.samples;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    /* D3D11's BACK culling with FrontCounterClockwise for CULL_CLOCKWISE. */
    info.rasterizer_state.cull_mode = state.cull == RECOMP_D3D_CULL_NONE
        ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = state.cull == RECOMP_D3D_CULL_CLOCKWISE
        ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    if (draw.depth_bias != 0 || state.slope_bias != 0.0f) {
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = static_cast<float>(draw.depth_bias);
        info.rasterizer_state.depth_bias_slope_factor = state.slope_bias;
    }
    info.depth_stencil_state.enable_depth_test = key.depth_test;
    info.depth_stencil_state.enable_depth_write = key.depth_write;
    info.depth_stencil_state.compare_op = hostCompareOp(draw.depth.depth_func);
    info.depth_stencil_state.enable_stencil_test = key.stencil_enable;
    if (key.stencil_enable) {
        info.depth_stencil_state.compare_mask = key.stencil_read;
        info.depth_stencil_state.write_mask = key.stencil_write;
        SDL_GPUStencilOpState &face = info.depth_stencil_state.front_stencil_state;
        face.fail_op = hostStencilOp(draw.depth.stencil_fail);
        face.pass_op = hostStencilOp(draw.depth.stencil_pass);
        face.depth_fail_op = hostStencilOp(draw.depth.stencil_zfail);
        face.compare_op = hostCompareOp(draw.depth.stencil_func);
        info.depth_stencil_state.back_stencil_state = face;
    }
    SDL_GPUColorTargetDescription color{};
    color.format = state.color_format;
    color.blend_state.enable_color_write_mask = true;
    color.blend_state.color_write_mask = key.write_mask;
    if (state.count_pass) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    } else if (key.blend_enable) {
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = hostBlendFactor(draw.blend.src_factor, false);
        color.blend_state.dst_color_blendfactor = hostBlendFactor(draw.blend.dst_factor, false);
        color.blend_state.color_blend_op = hostBlendOp(draw.blend.op);
        color.blend_state.src_alpha_blendfactor = hostBlendFactor(draw.blend.src_factor, true);
        color.blend_state.dst_alpha_blendfactor = hostBlendFactor(draw.blend.dst_factor, true);
        color.blend_state.alpha_blend_op = hostBlendOp(draw.blend.op);
    }
    info.target_info.color_target_descriptions = &color;
    info.target_info.num_color_targets = 1u;
    info.target_info.has_depth_stencil_target = state.depth_format != SDL_GPU_TEXTUREFORMAT_INVALID;
    info.target_info.depth_stencil_format = state.depth_format;

    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0u;
    buffer.pitch = state.stride;
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attributes[8]{};
    info.vertex_input_state.vertex_buffer_descriptions = &buffer;
    info.vertex_input_state.num_vertex_buffers = 1u;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes =
        vertexAttributes(state.shaders.layout, state.coverage_only, attributes);

    const auto start = std::chrono::steady_clock::now();
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(presenter->device, &info);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (ms >= 20) {
        std::fprintf(stderr, "recomp d3d presenter: slow pipeline fvf=0x%03X in %lld ms (%zu cached)\n",
            state.fvf, static_cast<long long>(ms), presenter->pipelines.size());
    }
    if (pipeline == nullptr) {
        std::fprintf(stderr, "recomp d3d presenter: draw pipeline fvf=0x%08X failed: %s\n",
            state.fvf, SDL_GetError());
    }
    // Like the D3D11 backend, a failed state is not retried every draw.
    presenter->pipelines.emplace(key, pipeline);
    return pipeline;
}

/* ---- Recording ------------------------------------------------------------ */

uint32_t appendAligned(std::vector<uint8_t> &data, const void *bytes, size_t size)
{
    const size_t offset = (data.size() + 15u) & ~size_t{15u};
    data.resize(offset + size);
    std::memcpy(data.data() + offset, bytes, size);
    return static_cast<uint32_t>(offset);
}

bool ensureRingCapacity(RecompD3dPresenter *presenter, SDL_GPUBuffer *&buffer,
    SDL_GPUTransferBuffer *&transfer, uint32_t &capacity, uint32_t &used, uint32_t required,
    SDL_GPUBufferUsageFlags usage)
{
    if (buffer != nullptr && required <= capacity) return true;
    // The command buffer may still read the old buffer; submit it first.
    submitCommands(presenter);
    uint32_t size = std::max(capacity, usage == SDL_GPU_BUFFERUSAGE_VERTEX
        ? kInitialVertexCapacity : kInitialIndexCapacity);
    while (size < required) size *= 2u;
    if (buffer != nullptr) SDL_ReleaseGPUBuffer(presenter->device, buffer);
    if (transfer != nullptr) SDL_ReleaseGPUTransferBuffer(presenter->device, transfer);
    SDL_GPUBufferCreateInfo buffer_info{usage, size, 0};
    buffer = SDL_CreateGPUBuffer(presenter->device, &buffer_info);
    SDL_GPUTransferBufferCreateInfo transfer_info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    transfer = SDL_CreateGPUTransferBuffer(presenter->device, &transfer_info);
    capacity = buffer != nullptr && transfer != nullptr ? size : 0u;
    used = 0u;
    return capacity != 0u;
}

/* Uploads one ring's segment bytes, cycling the ring when it wraps. */
bool uploadRing(RecompD3dPresenter *presenter, SDL_GPUCopyPass *pass, SDL_GPUBuffer *buffer,
    SDL_GPUTransferBuffer *transfer, uint32_t capacity, uint32_t &used, const std::vector<uint8_t> &bytes,
    uint32_t &base)
{
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    base = (used + 255u) & ~255u;
    const bool wrap = base + size > capacity;
    if (wrap) base = 0u;
    void *mapped = SDL_MapGPUTransferBuffer(presenter->device, transfer, wrap);
    if (mapped == nullptr) return false;
    std::memcpy(static_cast<uint8_t *>(mapped) + base, bytes.data(), size);
    SDL_UnmapGPUTransferBuffer(presenter->device, transfer);
    SDL_GPUTransferBufferLocation source{transfer, base};
    SDL_GPUBufferRegion destination{buffer, base, size};
    SDL_UploadToGPUBuffer(pass, &source, &destination, wrap);
    used = base + size;
    return true;
}

void flushSegment(RecompD3dPresenter *presenter)
{
    Segment &segment = presenter->segment;
    if (segment.draws.empty()) {
        releaseRetiredTextures(presenter);
        return;
    }
    const uint32_t vertex_bytes = static_cast<uint32_t>(segment.vertex_data.size());
    const uint32_t index_bytes = static_cast<uint32_t>(segment.index_data.size());
    bool ready = ensureRingCapacity(presenter, presenter->vertex_buffer, presenter->vertex_transfer,
            presenter->vertex_capacity, presenter->vertex_used, vertex_bytes, SDL_GPU_BUFFERUSAGE_VERTEX) &&
        ensureRingCapacity(presenter, presenter->index_buffer, presenter->index_transfer,
            presenter->index_capacity, presenter->index_used, index_bytes, SDL_GPU_BUFFERUSAGE_INDEX);
    SDL_GPUCommandBuffer *commands = ready ? commandBuffer(presenter) : nullptr;
    uint32_t vertex_base = 0u, index_base = 0u;
    if (commands != nullptr) {
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
        ready = copy != nullptr &&
            uploadRing(presenter, copy, presenter->vertex_buffer, presenter->vertex_transfer,
                presenter->vertex_capacity, presenter->vertex_used, segment.vertex_data, vertex_base) &&
            uploadRing(presenter, copy, presenter->index_buffer, presenter->index_transfer,
                presenter->index_capacity, presenter->index_used, segment.index_data, index_base);
        if (copy != nullptr) SDL_EndGPUCopyPass(copy);
    }
    SDL_GPURenderPass *pass = nullptr;
    if (ready && commands != nullptr) {
        SDL_GPUColorTargetInfo color{};
        color.texture = segment.color;
        color.load_op = SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = segment.depth;
        depth.load_op = SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
        pass = SDL_BeginGPURenderPass(commands, &color, 1, segment.depth != nullptr ? &depth : nullptr);
    }
    if (pass != nullptr) {
        SDL_GPUGraphicsPipeline *pipeline = nullptr;
        uint32_t uniforms = UINT32_MAX, program = UINT32_MAX;
        SDL_GPUTextureSamplerBinding bound[4]{};
        SDL_FColor constants{-1.0f, -1.0f, -1.0f, -1.0f};
        float sampler_bias[4] = {NAN, NAN, NAN, NAN};
        const bool push_bias = presenter->shader_target == RecompShaderTarget::Msl;
        int stencil = -1;
        SDL_GPUViewport viewport{};
        SDL_Rect scissor{-1, -1, -1, -1};
        uint32_t vertex_offset = UINT32_MAX, index_offset = UINT32_MAX;
        for (const QueuedDraw &draw : segment.draws) {
            if (draw.pipeline != pipeline) {
                SDL_BindGPUGraphicsPipeline(pass, draw.pipeline);
                pipeline = draw.pipeline;
            }
            if (vertex_base + draw.vertex_offset != vertex_offset) {
                vertex_offset = vertex_base + draw.vertex_offset;
                const SDL_GPUBufferBinding binding{presenter->vertex_buffer, vertex_offset};
                SDL_BindGPUVertexBuffers(pass, 0u, &binding, 1u);
            }
            if (index_base + draw.index_offset != index_offset) {
                index_offset = index_base + draw.index_offset;
                const SDL_GPUBufferBinding binding{presenter->index_buffer, index_offset};
                SDL_BindGPUIndexBuffer(pass, &binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
            }
            if (draw.uniform_offset != uniforms) {
                uniforms = draw.uniform_offset;
                const float *data = segment.uniform_data.data() + uniforms;
                SDL_PushGPUVertexUniformData(commands, 0u, data, kTransformFloats * sizeof(float));
                SDL_PushGPUFragmentUniformData(commands, 0u, data, kTransformFloats * sizeof(float));
            }
            if (draw.program_offset != UINT32_MAX && draw.program_offset != program) {
                program = draw.program_offset;
                SDL_PushGPUVertexUniformData(commands, 1u, segment.uniform_data.data() + program,
                    kProgramFloats * sizeof(float));
            }
            bool rebind = false;
            for (unsigned slot = 0u; slot < 4u; ++slot) {
                rebind |= bound[slot].texture != draw.textures[slot] || bound[slot].sampler != draw.samplers[slot];
                bound[slot] = {draw.textures[slot], draw.samplers[slot]};
            }
            if (rebind) SDL_BindGPUFragmentSamplers(pass, 0u, bound, 4u);
            if (push_bias && std::memcmp(draw.sampler_bias, sampler_bias, sizeof sampler_bias) != 0) {
                std::memcpy(sampler_bias, draw.sampler_bias, sizeof sampler_bias);
                SDL_PushGPUFragmentUniformData(commands, 1u, sampler_bias, sizeof sampler_bias);
            }
            if (std::memcmp(&draw.blend_constants, &constants, sizeof constants) != 0) {
                constants = draw.blend_constants;
                SDL_SetGPUBlendConstants(pass, constants);
            }
            if (draw.stencil_ref != stencil) {
                stencil = draw.stencil_ref;
                SDL_SetGPUStencilReference(pass, draw.stencil_ref);
            }
            if (std::memcmp(&draw.viewport, &viewport, sizeof viewport) != 0) {
                viewport = draw.viewport;
                SDL_SetGPUViewport(pass, &viewport);
            }
            if (std::memcmp(&draw.scissor, &scissor, sizeof scissor) != 0) {
                scissor = draw.scissor;
                SDL_SetGPUScissor(pass, &scissor);
            }
            SDL_DrawGPUIndexedPrimitives(pass, draw.index_count, 1u, 0u, 0, 0u);
        }
        SDL_EndGPURenderPass(pass);
    } else {
        std::fprintf(stderr, "recomp d3d presenter: dropped %zu draws: %s\n",
            segment.draws.size(), SDL_GetError());
    }
    segment.draws.clear();
    segment.vertex_data.clear();
    segment.index_data.clear();
    segment.uniform_data.clear();
    releaseRetiredTextures(presenter);
}

/* Switches the segment to a target, flushing draws queued for another. */
void useTarget(RecompD3dPresenter *presenter, SDL_GPUTexture *color, SDL_GPUTexture *depth)
{
    Segment &segment = presenter->segment;
    if (segment.color != color || segment.depth != depth) {
        flushSegment(presenter);
        segment.color = color;
        segment.depth = depth;
    }
}

/* Records a clear of a whole target. */
void clearTexture(RecompD3dPresenter *presenter, SDL_GPUTexture *color, const SDL_FColor *clear_color,
    SDL_GPUTexture *depth, const float *clear_depth, const uint8_t *clear_stencil)
{
    SDL_GPUColorTargetInfo color_info{};
    color_info.texture = color;
    color_info.load_op = clear_color != nullptr ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    if (clear_color != nullptr) color_info.clear_color = *clear_color;
    color_info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth_info{};
    depth_info.texture = depth;
    depth_info.load_op = clear_depth != nullptr ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    depth_info.clear_depth = clear_depth != nullptr ? *clear_depth : 0.0f;
    depth_info.store_op = SDL_GPU_STOREOP_STORE;
    depth_info.stencil_load_op = clear_stencil != nullptr ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    depth_info.clear_stencil = clear_stencil != nullptr ? *clear_stencil : 0u;
    depth_info.stencil_store_op = SDL_GPU_STOREOP_STORE;
    if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commandBuffer(presenter), &color_info, 1,
            depth != nullptr ? &depth_info : nullptr)) {
        SDL_EndGPURenderPass(pass);
    }
}

RecompD3dPresenterError submitClear(RecompD3dPresenter *presenter, const RecompD3dPresenterClearCommand &clear)
{
    if ((clear.clear_depth && (!std::isfinite(clear.z) || clear.z < 0.0f || clear.z > 1.0f)) ||
        (clear.clear_stencil && clear.stencil > 0xffu)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    BoundTarget bound;
    const RecompD3dPresenterError error = bindTarget(presenter, clear.target, bound);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    flushSegment(presenter);
    constexpr float byte_to_float = 1.0f / 255.0f;
    const SDL_FColor color = {
        static_cast<float>((clear.color >> 16u) & 0xffu) * byte_to_float,
        static_cast<float>((clear.color >> 8u) & 0xffu) * byte_to_float,
        static_cast<float>(clear.color & 0xffu) * byte_to_float,
        static_cast<float>((clear.color >> 24u) & 0xffu) * byte_to_float,
    };
    const float z = clear.z;
    const uint8_t stencil = static_cast<uint8_t>(clear.stencil);
    /* The guest ignores depth/stencil clear bits when no depth is attached. */
    SDL_GPUTexture *depth = (clear.clear_depth || clear.clear_stencil) ? bound.depth : nullptr;
    if (clear.clear_color || depth != nullptr) {
        clearTexture(presenter, bound.color, clear.clear_color ? &color : nullptr, depth,
            clear.clear_depth ? &z : nullptr, clear.clear_stencil ? &stencil : nullptr);
    }
    if (clear.clear_color) markWritten(presenter, bound);
    presenter->last_bound = bound;
    return RECOMP_D3D_PRESENTER_OK;
}

/* Visibility counts use R16_FLOAT, which blends everywhere SDL_GPU runs, and
   are summed in R32_FLOAT mips: a 2:1 level is an exact box average. */
constexpr SDL_GPUTextureFormat kCountFormat = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
constexpr SDL_GPUTextureFormat kReduceFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;

void releaseQuery(RecompD3dPresenter *presenter, VisibilityQuery &query)
{
    if (query.count != nullptr) SDL_ReleaseGPUTexture(presenter->device, query.count);
    if (query.resolved != nullptr) SDL_ReleaseGPUTexture(presenter->device, query.resolved);
    if (query.reduce != nullptr) SDL_ReleaseGPUTexture(presenter->device, query.reduce);
    if (query.download != nullptr) SDL_ReleaseGPUTransferBuffer(presenter->device, query.download);
    if (query.fence != nullptr) SDL_ReleaseGPUFence(presenter->device, query.fence);
    query = {};
}

/* Gives the open query a counting target of the draw target's size and
   samples, cleared to zero. */
bool prepareCount(RecompD3dPresenter *presenter, const BoundTarget &bound)
{
    VisibilityQuery &query = presenter->visibility_active;
    if (query.count != nullptr) {
        if (query.width == bound.host_width && query.height == bound.host_height && query.samples == bound.samples)
            return true;
        static bool reported;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "recomp d3d presenter: one visibility test spans two target sizes;"
                " later draws are not counted\n");
        }
        return false;
    }
    query.width = bound.host_width;
    query.height = bound.host_height;
    query.samples = bound.samples;
    query.count = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, kCountFormat,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (bound.samples == SDL_GPU_SAMPLECOUNT_1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0u),
        query.width, query.height, 1u, 1u, query.samples);
    if (query.samples != SDL_GPU_SAMPLECOUNT_1) {
        query.resolved = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, kCountFormat,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, query.width, query.height);
    }
    uint32_t levels = 1u;
    while ((std::max(query.width, query.height) >> levels) != 0u) ++levels;
    query.reduce = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, kReduceFormat,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, query.width, query.height, 1u, levels);
    if (query.count == nullptr || query.reduce == nullptr ||
        (query.samples != SDL_GPU_SAMPLECOUNT_1 && query.resolved == nullptr)) {
        std::fprintf(stderr, "recomp d3d presenter: visibility target failed: %s\n", SDL_GetError());
        const uint32_t index = query.index;
        releaseQuery(presenter, query);
        query.index = index;
        return false;
    }
    const SDL_FColor zero{};
    clearTexture(presenter, query.count, &zero, nullptr, nullptr, nullptr);
    return true;
}

/* Everything a queued draw needs beyond its pipeline. */
struct DrawBindings {
    SDL_GPUTexture *textures[4];
    SDL_GPUSampler *samplers[4];
    float sampler_bias[4];
    SDL_FColor blend_constants;
    SDL_GPUViewport viewport;
    SDL_Rect scissor;
};

void queueDraw(RecompD3dPresenter *presenter, SDL_GPUGraphicsPipeline *pipeline, const void *vertices,
    uint32_t vertex_bytes, const void *indices, uint32_t index_count, const float *constants,
    bool program, const DrawBindings &bindings, uint8_t stencil_ref)
{
    Segment &segment = presenter->segment;
    QueuedDraw queued;
    queued.pipeline = pipeline;
    queued.vertex_offset = appendAligned(segment.vertex_data, vertices, vertex_bytes);
    queued.index_offset = appendAligned(segment.index_data, indices, index_count * sizeof(uint16_t));
    queued.index_count = index_count;
    /* The Transform buffer is the draw constants without vc[], which goes to
       the vertex stage's second buffer; consecutive draws often repeat both. */
    float transform[kTransformFloats];
    std::memcpy(transform, constants, 140u * sizeof(float));
    std::memcpy(transform + 140u, constants + 140u + kProgramFloats, (kTransformFloats - 140u) * sizeof(float));
    std::vector<float> &uniforms = segment.uniform_data;
    if (!segment.draws.empty() && segment.draws.back().uniform_offset + kTransformFloats <= uniforms.size() &&
        std::memcmp(uniforms.data() + segment.draws.back().uniform_offset, transform, sizeof transform) == 0) {
        queued.uniform_offset = segment.draws.back().uniform_offset;
    } else {
        queued.uniform_offset = static_cast<uint32_t>(uniforms.size());
        uniforms.insert(uniforms.end(), transform, transform + kTransformFloats);
    }
    if (program) {
        const float *vc = constants + 140u;
        if (!segment.draws.empty() && segment.draws.back().program_offset != UINT32_MAX &&
            std::memcmp(uniforms.data() + segment.draws.back().program_offset, vc,
                kProgramFloats * sizeof(float)) == 0) {
            queued.program_offset = segment.draws.back().program_offset;
        } else {
            queued.program_offset = static_cast<uint32_t>(uniforms.size());
            uniforms.insert(uniforms.end(), vc, vc + kProgramFloats);
        }
    }
    std::copy(std::begin(bindings.textures), std::end(bindings.textures), queued.textures);
    std::copy(std::begin(bindings.samplers), std::end(bindings.samplers), queued.samplers);
    std::copy(std::begin(bindings.sampler_bias), std::end(bindings.sampler_bias), queued.sampler_bias);
    queued.blend_constants = bindings.blend_constants;
    queued.stencil_ref = stencil_ref;
    queued.viewport = bindings.viewport;
    queued.scissor = bindings.scissor;
    segment.draws.push_back(queued);
}

/* The D3D11 backend widens XYZ|DIFFUSE lines in a geometry shader (gs_main
   in d3d_draw_shader.cpp), which SDL_GPU lacks. This is that shader on the
   CPU: it clips each transformed segment, then emits a strip of quads, which
   are drawn as pretransformed XYZRHW|DIFFUSE triangles. */
struct LineVertex {
    float clip[4];
    float color[4];
};

struct WideVertex {
    float x, y, z, rhw;
    uint32_t diffuse;
};

uint32_t packColor(const float (&color)[4])
{
    const auto byte = [](float value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    // Memory order B, G, R, A, as the guest stream stores A8R8G8B8.
    return byte(color[2]) | byte(color[1]) << 8u | byte(color[0]) << 16u | byte(color[3]) << 24u;
}

void expandLine(LineVertex a, LineVertex b, const float *style, std::vector<WideVertex> &out)
{
    if (style[0] <= 0.0f) return;
    for (unsigned plane = 0u; plane < 3u; ++plane) {
        const auto distance = [plane](const LineVertex &v) {
            return plane == 0u ? v.clip[2] : plane == 1u ? v.clip[3] - v.clip[2] : v.clip[3] - 1e-6f;
        };
        const float da = distance(a), db = distance(b);
        if (da < 0.0f && db < 0.0f) return;
        if (da < 0.0f || db < 0.0f) {
            const float t = da / (da - db);
            LineVertex c{};
            for (unsigned i = 0u; i < 4u; ++i) {
                c.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
                c.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
            }
            (da < 0.0f ? a : b) = c;
        }
    }
    const float dx = (b.clip[0] / b.clip[3] - a.clip[0] / a.clip[3]) / style[2];
    const float dy = (b.clip[1] / b.clip[3] - a.clip[1] / a.clip[3]) / style[3];
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-6f) return;
    float side[2];
    if (style[1] > 0.0f) {
        side[0] = -dy / length;
        side[1] = dx / length;
    } else if (std::fabs(dx) >= std::fabs(dy)) {
        side[0] = 0.0f;
        side[1] = 1.0f;
    } else {
        side[0] = 1.0f;
        side[1] = 0.0f;
    }
    const unsigned layers = style[1] > 0.0f ? 4u : 2u;
    std::vector<WideVertex> strip;
    for (unsigned i = 0u; i < layers; ++i) {
        const float offset = style[1] > 0.0f
            ? (i == 0u ? -(style[0] + 1.0f) * 0.5f : i == 1u ? -std::fabs(style[0] - 1.0f) * 0.5f
                : i == 2u ? std::fabs(style[0] - 1.0f) * 0.5f : (style[0] + 1.0f) * 0.5f)
            : (i == 0u ? -style[0] * 0.5f : style[0] * 0.5f);
        const float coverage = style[1] > 0.0f ? (i == 0u || i == 3u ? 0.0f : std::min(style[0], 1.0f)) : 1.0f;
        for (const LineVertex *end : {&a, &b}) {
            float clip[4] = {end->clip[0], end->clip[1], end->clip[2], end->clip[3]};
            clip[0] += side[0] * offset * style[2] * clip[3];
            clip[1] += side[1] * offset * style[3] * clip[3];
            float color[4] = {end->color[0], end->color[1], end->color[2], end->color[3] * coverage};
            strip.push_back({clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3], 1.0f / clip[3],
                packColor(color)});
        }
    }
    for (size_t i = 2u; i < strip.size(); ++i) {
        out.push_back(strip[i - 2u]);
        out.push_back(strip[i - 1u]);
        out.push_back(strip[i]);
    }
}

RecompD3dPresenterError submitDraw(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const bool screen_offset = draw.screen_space_offset[0] != 0.0f || draw.screen_space_offset[1] != 0.0f;
    const bool geometry_line = recomp_d3d_draw_geometry_line(draw);
    // shortcut: programmable offsets are unsupported, add their viewport contract when needed.
    if (draw.vertex_bytes == nullptr || draw.index_bytes == nullptr ||
        (draw.texture_factor_color_scale != 0u && draw.texture_factor_color_scale != 2u &&
         draw.texture_factor_color_scale != 4u) ||
        draw.vertex_stride == 0u || draw.vertex_count == 0u ||
        draw.index_count == 0u || !std::isfinite(draw.line_width) || draw.line_width < 0.0f ||
        (geometry_line && (draw.fvf != 0x42u || draw.program_count || draw.primitive_type != 2u ||
            draw.has_texture || draw.directional.enabled)) ||
        static_cast<unsigned>(draw.cull_mode) > RECOMP_D3D_CULL_COUNTER_CLOCKWISE ||
        !std::isfinite(draw.slope_scaled_depth_bias) ||
        !std::isfinite(draw.screen_space_offset[0]) || !std::isfinite(draw.screen_space_offset[1]) ||
        (screen_offset && draw.program_count) ||
        !std::isfinite(draw.mip_lod_bias) ||
        draw.mip_lod_bias < kMipLodBiasMin || draw.mip_lod_bias > kMipLodBiasMax ||
        !std::isfinite(draw.reflection_mip_lod_bias) ||
        draw.reflection_mip_lod_bias < kMipLodBiasMin || draw.reflection_mip_lod_bias > kMipLodBiasMax) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    const bool sub_viewport = draw.has_transform && !draw.program_count && draw.viewport[2] > 0.0f;

    RecompD3dVertexLayout layout;
    if (!recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.stride > draw.vertex_stride ||
        (!layout.pretransformed && !draw.has_transform && !draw.program_count) ||
        (draw.blend_weight_count != 0u && layout.blend_weight_count != draw.blend_weight_count)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    SDL_GPUPrimitiveType primitive;
    switch (draw.primitive_type) {
    case 2u:
        if (draw.index_count < 2u || draw.index_count % 2u != 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        primitive = geometry_line ? SDL_GPU_PRIMITIVETYPE_TRIANGLELIST : SDL_GPU_PRIMITIVETYPE_LINELIST;
        break;
    case 6u:
        primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        break;
    default:
        /* SDL_GPU has no triangle fan; fans become a list below. */
        primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        break;
    }

    const bool scaled_bias = presenter->scale != 1.0f &&
        (!draw.target.offscreen || (draw.target.color.width == presenter->config.width &&
            draw.target.color.height == presenter->config.height)) &&
        draw.slope_scaled_depth_bias != 0.0f;
    if (draw.program_count > 136u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    // Widened lines draw as the pretransformed XYZRHW|DIFFUSE triangles they expand to.
    const uint32_t shader_fvf = geometry_line ? 0x44u : draw.fvf;
    const RecompD3dDrawShaderKey shader_key{shader_fvf, draw.program_count,
        draw.program_count ? draw.program : nullptr, false, scaled_bias,
        scaled_bias && presenter->msaa_count > 1u};
    const ShaderPair shaders = lookupShaders(presenter, shader_key, false);
    if (shaders.vertex == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;

    BoundTarget bound;
    const RecompD3dPresenterError target_error = bindTarget(presenter, draw.target, bound);
    if (target_error != RECOMP_D3D_PRESENTER_OK) return target_error;
    SDL_Rect scissor{0, 0, static_cast<int>(bound.host_width), static_cast<int>(bound.host_height)};
    if (draw.has_scissor) {
        if (draw.scissor[0] < 0 || draw.scissor[1] < 0 ||
            draw.scissor[2] < draw.scissor[0] || draw.scissor[3] < draw.scissor[1])
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        double edges[4];
        for (unsigned i = 0u; i < 4u; ++i) {
            edges[i] = draw.scissor[i] * static_cast<double>(i % 2u == 0u ? bound.scale_x : bound.scale_y);
            if (!std::isfinite(edges[i]) || edges[i] > 32767.0) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            edges[i] = std::floor(edges[i] + 0.5);
        }
        // Metal requires the scissor inside the target.
        const int left = static_cast<int>(std::min<double>(edges[0], bound.host_width));
        const int top = static_cast<int>(std::min<double>(edges[1], bound.host_height));
        const int right = static_cast<int>(std::min<double>(edges[2], bound.host_width));
        const int bottom = static_cast<int>(std::min<double>(edges[3], bound.host_height));
        scissor = {left, top, right - left, bottom - top};
    }
    if (draw.has_texture) {
        const RenderTargetEntry *sampled = findRenderTarget(presenter, draw.texture);
        if (sampled != nullptr && sampled->render == bound.color) {
            std::fprintf(stderr, "recomp d3d presenter: cannot sample active render target data=0x%08X\n",
                draw.texture.data);
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
    }
    useTarget(presenter, bound.color, bound.depth);

    SDL_GPUTexture *texture = draw.has_texture ? lookupTexture(presenter, draw) : nullptr;
    SDL_GPUTexture *mask = nullptr, *bump = nullptr;
    if (draw.has_alpha_mask) {
        if (layout.texcoord_count != 2u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        RecompD3dPresenterDrawCommand lookup{};
        lookup.texture = draw.alpha_mask;
        lookup.texture_bytes = draw.alpha_mask_bytes;
        lookup.texture_byte_count = draw.alpha_mask_byte_count;
        lookup.palette_bytes = draw.alpha_mask_palette;
        lookup.palette_byte_count = draw.alpha_mask_palette_byte_count;
        mask = lookupTexture(presenter, lookup);
        if (mask == nullptr || texture == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    const auto reflectionLookup = [&](const RecompD3dTextureDesc &desc, const void *bytes, uint32_t count,
                                      bool bump_env = false) {
        RecompD3dPresenterDrawCommand lookup{};
        lookup.bump_env = bump_env;
        lookup.texture = desc;
        lookup.texture_bytes = bytes;
        lookup.texture_byte_count = count;
        return lookupTexture(presenter, lookup);
    };
    if (draw.has_reflection || draw.program_alpha_mask) {
        if (draw.has_alpha_mask || draw.four_tap_filter || layout.pretransformed ||
            (draw.has_reflection && draw.program_alpha_mask) ||
            (draw.program_alpha_mask && layout.normal_offset == RECOMP_D3D_FVF_ABSENT) ||
            layout.texcoord_count == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        if (mask == nullptr || texture == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.distortion) {
        if (draw.has_alpha_mask || draw.has_reflection || draw.program_alpha_mask ||
            draw.four_tap_filter || layout.texcoord_count != 4u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        if (mask == nullptr || texture == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.stage1_alpha_mask) {
        if (draw.has_alpha_mask || draw.has_reflection || draw.program_count || draw.distortion ||
            draw.four_tap_filter || (draw.stage1_kill_only ? layout.texcoord_count == 0u
                : layout.texcoord_count != 2u)) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        if (mask == nullptr || texture == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.reflection_stage2) {
        if (!draw.has_reflection || draw.program_count || draw.bump_env || draw.distortion ||
            draw.reflection_premultiplied || draw.reflection_skip_diffuse)
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        bump = reflectionLookup(draw.reflection_stage2_texture, draw.reflection_stage2_bytes,
            draw.reflection_stage2_byte_count);
        if (bump == nullptr || mask == nullptr || texture == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.bump_env) {
        if (!draw.program_count || draw.has_alpha_mask || draw.has_reflection || draw.program_alpha_mask ||
            draw.distortion || draw.stage1_alpha_mask || draw.four_tap_filter || draw.bright_pass)
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        bump = reflectionLookup(draw.alpha_mask, draw.alpha_mask_bytes, draw.alpha_mask_byte_count, true);
        if (bump == nullptr || mask == nullptr || texture == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.four_tap_filter && (texture == nullptr ||
            !std::isfinite(draw.four_tap_scale) || !std::isfinite(draw.four_tap_sum_scale))) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.bright_pass && (texture == nullptr || draw.four_tap_filter ||
            draw.has_alpha_mask || draw.has_reflection || draw.program_count ||
            layout.texcoord_count != 1u || draw.texture.width == 0u || draw.texture.height == 0u)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.luminance_tint && (texture == nullptr || draw.four_tap_filter || draw.bright_pass ||
            draw.has_alpha_mask || draw.has_reflection || draw.program_count || layout.texcoord_count != 1u)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    countDrawTexture(draw, texture != nullptr);

    RecompD3dDrawTargetState target_state{};
    target_state.viewport[2] = static_cast<float>(bound.host_width);
    target_state.viewport[3] = static_cast<float>(bound.host_height);
    target_state.viewport[5] = 1.0f;
    target_state.scale_x = bound.scale_x;
    target_state.scale_y = bound.scale_y;
    float constants[kRecompD3dDrawConstantFloats];
    if (!recomp_d3d_draw_constants(draw, layout, target_state, texture != nullptr, scaled_bias, constants)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }

    const auto *source_indices = static_cast<const uint16_t *>(draw.index_bytes);
    const void *vertices = draw.vertex_bytes;
    uint32_t vertex_bytes = draw.vertex_stride * draw.vertex_count;
    uint32_t stride = draw.vertex_stride;
    std::vector<uint16_t> expanded_indices;
    std::vector<WideVertex> wide;
    const void *indices = draw.index_bytes;
    uint32_t index_count = draw.index_count;
    if (draw.primitive_type == 7u) {
        if (draw.triangle_count > 4096u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        expanded_indices.resize(draw.triangle_count * 3u);
        for (uint32_t i = 0u; i < draw.triangle_count; ++i) {
            expanded_indices[i * 3u + 0u] = source_indices[0];
            expanded_indices[i * 3u + 1u] = source_indices[i + 1u];
            expanded_indices[i * 3u + 2u] = source_indices[i + 2u];
        }
        indices = expanded_indices.data();
        index_count = draw.triangle_count * 3u;
    }
    if (geometry_line) {
        const float *style = constants + 140u + kProgramFloats + 340u + 8u;
        const auto fetch = [&](uint16_t index, LineVertex &vertex) {
            if (index >= draw.vertex_count) return false;
            const auto *bytes = static_cast<const uint8_t *>(draw.vertex_bytes) +
                static_cast<size_t>(index) * draw.vertex_stride;
            float position[3];
            uint32_t diffuse;
            std::memcpy(position, bytes + layout.position_offset, sizeof position);
            std::memcpy(&diffuse, bytes + layout.diffuse_offset, sizeof diffuse);
            for (unsigned column = 0u; column < 4u; ++column) {
                vertex.clip[column] = position[0] * constants[column] + position[1] * constants[4u + column] +
                    position[2] * constants[8u + column] + constants[12u + column];
            }
            vertex.color[0] = ((diffuse >> 16u) & 0xffu) / 255.0f;
            vertex.color[1] = ((diffuse >> 8u) & 0xffu) / 255.0f;
            vertex.color[2] = (diffuse & 0xffu) / 255.0f;
            vertex.color[3] = (diffuse >> 24u) / 255.0f;
            return true;
        };
        for (uint32_t i = 0u; i + 1u < draw.index_count; i += 2u) {
            LineVertex a{}, b{};
            if (!fetch(source_indices[i], a) || !fetch(source_indices[i + 1u], b)) {
                return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            }
            expandLine(a, b, style, wide);
        }
        if (wide.empty() || wide.size() > 65535u) return wide.empty() ? RECOMP_D3D_PRESENTER_OK
            : RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        // The expanded vertices are clip space already.
        std::fill(constants, constants + 16, 0.0f);
        constants[0] = constants[5] = constants[10] = constants[15] = 1.0f;
        expanded_indices.resize(wide.size());
        for (size_t i = 0u; i < wide.size(); ++i) expanded_indices[i] = static_cast<uint16_t>(i);
        vertices = wide.data();
        vertex_bytes = static_cast<uint32_t>(wide.size() * sizeof(WideVertex));
        stride = sizeof(WideVertex);
        indices = expanded_indices.data();
        index_count = static_cast<uint32_t>(wide.size());
    }

    PipelineState state{};
    state.draw = &draw;
    state.shaders = shaders;
    state.fvf = shader_fvf;
    state.stride = stride;
    state.primitive = primitive;
    state.cull = geometry_line ? RECOMP_D3D_CULL_NONE : draw.cull_mode;
    state.color_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    state.depth_format = bound.depth != nullptr ? presenter->depth_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    state.samples = bound.samples;
    state.slope_bias = scaled_bias ? 0.0f : draw.slope_scaled_depth_bias;
    SDL_GPUGraphicsPipeline *pipeline = lookupPipeline(presenter, state);
    if (pipeline == nullptr) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;

    const bool volume = draw.texture.volume_depth != 0u;
    DrawBindings bindings{};
    bindings.textures[0] = !volume && texture != nullptr ? texture : presenter->white_texture;
    bindings.textures[1] = mask != nullptr ? mask : presenter->white_texture;
    bindings.textures[2] = bump != nullptr ? bump : presenter->white_texture;
    bindings.textures[3] = volume && texture != nullptr ? texture : presenter->white_volume;
    bindings.samplers[0] = draw.four_tap_filter || draw.has_alpha_mask ||
            (draw.program_count && (!draw.address_u || !draw.address_v))
        ? presenter->filter_sampler
        : lookupDrawSampler(presenter, draw.address_u, draw.address_v, draw.point_mip_filter,
            draw.mip_lod_bias, volume ? draw.address_w : 1u);
    bindings.samplers[1] = draw.program_alpha_mask ? presenter->program_mask_sampler
        : draw.has_reflection || draw.distortion || draw.stage1_alpha_mask || draw.bump_env
        ? lookupDrawSampler(presenter, draw.reflection_address_u, draw.reflection_address_v,
            draw.reflection_point_mip_filter, draw.reflection_mip_lod_bias)
        : presenter->filter_sampler;
    bindings.samplers[2] = draw.reflection_stage2
        ? lookupDrawSampler(presenter, draw.reflection_stage2_address_u, draw.reflection_stage2_address_v,
            draw.reflection_stage2_point_mip_filter, draw.reflection_stage2_mip_lod_bias)
        : draw.bump_env ? lookupDrawSampler(presenter, draw.bump_address_u, draw.bump_address_v, false, 0.0f)
        : presenter->filter_sampler;
    // The volume samples with the stage-0 sampler, as guest_sampler on D3D11.
    bindings.samplers[3] = bindings.samplers[0];
    // The LOD bias each chosen sampler carries; Metal applies it in the shader.
    const auto biasOf = [&](unsigned slot, float bias) {
        bindings.sampler_bias[slot] = bindings.samplers[slot] == presenter->filter_sampler ||
            bindings.samplers[slot] == presenter->program_mask_sampler ? 0.0f : bias;
    };
    biasOf(0u, draw.mip_lod_bias);
    biasOf(1u, draw.has_reflection || draw.distortion || draw.stage1_alpha_mask || draw.bump_env
        ? draw.reflection_mip_lod_bias : 0.0f);
    biasOf(2u, draw.reflection_stage2 ? draw.reflection_stage2_mip_lod_bias : 0.0f);
    bindings.sampler_bias[3] = bindings.sampler_bias[0];
    for (SDL_GPUSampler *sampler : bindings.samplers) {
        if (sampler == nullptr) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    {
        const uint32_t color = draw.blend.constant_color;
        const float alpha = ((color >> 24u) & 255u) / 255.0f;
        /* One blend constant, so constant alpha replicates A. shortcut: mixing
           constant color and constant alpha in one draw is unsupported. */
        const auto alpha_factor = [](RecompD3dBlendFactor factor) {
            return factor == RECOMP_D3D_BLEND_CONSTANT_ALPHA || factor == RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA;
        };
        const bool constant_alpha = alpha_factor(draw.blend.src_factor) || alpha_factor(draw.blend.dst_factor);
        bindings.blend_constants = {
            constant_alpha ? alpha : ((color >> 16u) & 255u) / 255.0f,
            constant_alpha ? alpha : ((color >> 8u) & 255u) / 255.0f,
            constant_alpha ? alpha : (color & 255u) / 255.0f,
            alpha};
    }
    /* D3D8 maps clip space onto the viewport and clips there, so a sub-rect
       viewport (the select-screen portrait) must not reach the full target. */
    bindings.viewport = sub_viewport
        ? SDL_GPUViewport{draw.viewport[0] * bound.scale_x, draw.viewport[1] * bound.scale_y,
            draw.viewport[2] * bound.scale_x, draw.viewport[3] * bound.scale_y, draw.viewport[4], draw.viewport[5]}
        : SDL_GPUViewport{0.0f, 0.0f, static_cast<float>(bound.host_width),
            static_cast<float>(bound.host_height), 0.0f, 1.0f};
    bindings.scissor = scissor;

    /* An open visibility test counts this draw's passing samples first, before
       its own depth writes, into the test's counting target. */
    if (presenter->visibility_open && prepareCount(presenter, bound)) {
        const ShaderPair counting = lookupShaders(presenter, shader_key, true);
        PipelineState count_state = state;
        count_state.shaders = counting;
        count_state.color_format = kCountFormat;
        count_state.count_pass = true;
        SDL_GPUGraphicsPipeline *count_pipeline =
            counting.vertex != nullptr ? lookupPipeline(presenter, count_state) : nullptr;
        if (count_pipeline != nullptr) {
            useTarget(presenter, presenter->visibility_active.count, bound.depth);
            queueDraw(presenter, count_pipeline, vertices, vertex_bytes, indices, index_count, constants,
                draw.program_count != 0u, bindings, static_cast<uint8_t>(draw.depth.stencil_ref));
            useTarget(presenter, bound.color, bound.depth);
        }
    }
    queueDraw(presenter, pipeline, vertices, vertex_bytes, indices, index_count, constants,
        draw.program_count != 0u, bindings, static_cast<uint8_t>(draw.depth.stencil_ref));
    markWritten(presenter, bound);
    presenter->last_bound = bound;
    ++presenter->draw_count;
    return RECOMP_D3D_PRESENTER_OK;
}

/* ---- Visibility ----------------------------------------------------------- */

/* A repeated BEGIN restarts counting, as the SDK's CLEAR_REPORT_VALUE does. */
RecompD3dPresenterError beginVisibility(RecompD3dPresenter *presenter)
{
    if (presenter->visibility_open) {
        if (presenter->visibility_active.count != nullptr) {
            flushSegment(presenter);
            const SDL_FColor zero{};
            clearTexture(presenter, presenter->visibility_active.count, &zero, nullptr, nullptr, nullptr);
        }
        return RECOMP_D3D_PRESENTER_OK;
    }
    presenter->visibility_active = {};
    presenter->visibility_open = true;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError endVisibility(RecompD3dPresenter *presenter, uint32_t index, const BoundTarget &last)
{
    if (!presenter->visibility_open) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    presenter->visibility_open = false;
    VisibilityQuery query = presenter->visibility_active;
    presenter->visibility_active = {};
    query.index = index;
    // Host pixels per guest pixel of the target bound when counting ends.
    query.host_pixels_per_guest_pixel = last.scale_x * last.scale_y;
    if (query.count != nullptr) {
        flushSegment(presenter);
        SDL_GPUCommandBuffer *commands = commandBuffer(presenter);
        SDL_GPUTexture *counts = query.count;
        if (query.samples != SDL_GPU_SAMPLECOUNT_1) {
            resolveTarget(presenter, query.count, query.resolved);
            counts = query.resolved;
        }
        SDL_GPUBlitInfo widen{};
        widen.source.texture = counts;
        widen.source.w = query.width;
        widen.source.h = query.height;
        widen.destination.texture = query.reduce;
        widen.destination.w = query.width;
        widen.destination.h = query.height;
        widen.load_op = SDL_GPU_LOADOP_DONT_CARE;
        widen.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(commands, &widen);
        /* Averaging mip levels sums the counts without a compute pass. Read
           the deepest level each halving of which stays even, so every texel
           is an exact box average, or one of at most 16x16 texels. */
        SDL_GenerateMipmapsForGPUTexture(commands, query.reduce);
        query.level = 0u;
        while (((query.width >> query.level) & 1u) == 0u && ((query.height >> query.level) & 1u) == 0u &&
               std::max(query.width >> query.level, query.height >> query.level) > 16u) {
            ++query.level;
        }
        query.level_width = std::max(query.width >> query.level, 1u);
        query.level_height = std::max(query.height >> query.level, 1u);
        SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
            query.level_width * query.level_height * 4u, 0};
        query.download = SDL_CreateGPUTransferBuffer(presenter->device, &info);
        SDL_GPUCopyPass *pass = query.download != nullptr ? SDL_BeginGPUCopyPass(commands) : nullptr;
        if (pass == nullptr) {
            releaseQuery(presenter, query);
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        SDL_GPUTextureRegion region{query.reduce, query.level, 0u, 0u, 0u, 0u,
            query.level_width, query.level_height, 1u};
        SDL_GPUTextureTransferInfo destination{query.download, 0u, 0u, 0u};
        SDL_DownloadFromGPUTexture(pass, &region, &destination);
        SDL_EndGPUCopyPass(pass);
        /* The result is polled next frame; submitting now, as the SDK's push
           buffer kick does, lets the GPU finish it meanwhile. */
        query.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
        presenter->command_buffer = nullptr;
        if (query.fence == nullptr) {
            releaseQuery(presenter, query);
            return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
    }
    presenter->visibility_pending.push_back(query);
    return RECOMP_D3D_PRESENTER_OK;
}

/* ---- Coverage --------------------------------------------------------------- */

/* The D3D11 backend draws a coverage probe under an occlusion query and waits
   for it. A synchronous GPU readback here would stall the frame, so the probe
   rasterizes on the CPU: transform, clip to 0 <= z <= w, cull, and test pixel
   centers (sample positions with MSAA) inside viewport, scissor and target. */
bool triangleCovers(const float (&clip)[3][4], RecompD3dCullMode cull, const float (&rect)[4],
    const float (&test)[4], SDL_GPUSampleCount samples)
{
    // Clip against z >= 0, z <= w and w > 0 (Sutherland-Hodgman on a small polygon).
    std::vector<std::array<float, 4>> polygon = {
        {clip[0][0], clip[0][1], clip[0][2], clip[0][3]},
        {clip[1][0], clip[1][1], clip[1][2], clip[1][3]},
        {clip[2][0], clip[2][1], clip[2][2], clip[2][3]}};
    for (unsigned plane = 0u; plane < 3u && !polygon.empty(); ++plane) {
        const auto distance = [plane](const std::array<float, 4> &v) {
            return plane == 0u ? v[2] : plane == 1u ? v[3] - v[2] : v[3] - 1e-6f;
        };
        std::vector<std::array<float, 4>> kept;
        for (size_t i = 0u; i < polygon.size(); ++i) {
            const auto &a = polygon[i], &b = polygon[(i + 1u) % polygon.size()];
            const float da = distance(a), db = distance(b);
            if (da >= 0.0f) kept.push_back(a);
            if ((da >= 0.0f) != (db >= 0.0f)) {
                const float t = da / (da - db);
                std::array<float, 4> c;
                for (unsigned k = 0u; k < 4u; ++k) c[k] = a[k] + (b[k] - a[k]) * t;
                kept.push_back(c);
            }
        }
        polygon.swap(kept);
    }
    if (polygon.size() < 3u) return false;
    /* Window space with y down: x' = left + (x/w + 1) * width/2, y' = top + (1 - y/w) * height/2.
       Rasterizers snap vertices to 1/256 pixel (8 subpixel bits), which can
       give a collinear triangle area, so the probe snaps too. */
    const auto snap = [](float value) { return std::nearbyint(value * 256.0f) / 256.0f; };
    std::vector<std::array<float, 2>> screen;
    for (const auto &v : polygon) {
        screen.push_back({snap(rect[0] + (v[0] / v[3] + 1.0f) * 0.5f * (rect[2] - rect[0])),
            snap(rect[1] + (1.0f - v[1] / v[3]) * 0.5f * (rect[3] - rect[1]))});
    }
    float area = 0.0f;
    for (size_t i = 0u; i < screen.size(); ++i) {
        const auto &a = screen[i], &b = screen[(i + 1u) % screen.size()];
        area += a[0] * b[1] - b[0] * a[1];
    }
    if (area == 0.0f) return false;
    /* With y down, a positive area is clockwise on screen. D3D11 culls
       clockwise triangles for CULL_CLOCKWISE (FrontCounterClockwise) and
       counterclockwise ones otherwise. */
    if (cull == RECOMP_D3D_CULL_CLOCKWISE && area > 0.0f) return false;
    if (cull == RECOMP_D3D_CULL_COUNTER_CLOCKWISE && area < 0.0f) return false;
    float min_x = test[2], min_y = test[3], max_x = test[0], max_y = test[1];
    for (const auto &p : screen) {
        min_x = std::min(min_x, p[0]);
        min_y = std::min(min_y, p[1]);
        max_x = std::max(max_x, p[0]);
        max_y = std::max(max_y, p[1]);
    }
    // Pixels whose samples can lie inside both the triangle and the test rectangle.
    const int x0 = static_cast<int>(std::floor(std::max(min_x, test[0])));
    const int y0 = static_cast<int>(std::floor(std::max(min_y, test[1])));
    const int x1 = static_cast<int>(std::ceil(std::min(max_x, test[2])));
    const int y1 = static_cast<int>(std::ceil(std::min(max_y, test[3])));
    static const float centers[1][2] = {{0.5f, 0.5f}};
    static const float two[2][2] = {{0.75f, 0.75f}, {0.25f, 0.25f}};
    static const float four[4][2] = {{0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f}};
    static const float eight[8][2] = {{0.5625f, 0.3125f}, {0.4375f, 0.6875f}, {0.8125f, 0.5625f},
        {0.3125f, 0.1875f}, {0.1875f, 0.8125f}, {0.0625f, 0.4375f}, {0.6875f, 0.9375f}, {0.9375f, 0.0625f}};
    const float (*points)[2] = centers;
    unsigned point_count = 1u;
    if (samples == SDL_GPU_SAMPLECOUNT_2) { points = two; point_count = 2u; }
    if (samples == SDL_GPU_SAMPLECOUNT_4) { points = four; point_count = 4u; }
    if (samples == SDL_GPU_SAMPLECOUNT_8) { points = eight; point_count = 8u; }
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            for (unsigned s = 0u; s < point_count; ++s) {
                const float px = x + points[s][0], py = y + points[s][1];
                bool inside = px >= test[0] && px < test[2] && py >= test[1] && py < test[3];
                for (size_t i = 0u; i < screen.size() && inside; ++i) {
                    const auto &a = screen[i], &b = screen[(i + 1u) % screen.size()];
                    inside = sign * ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) >= 0.0f;
                }
                if (inside) return true;
            }
        }
    }
    return false;
}

/* ---- Presentation ------------------------------------------------------------ */

/* The Windows virtual-key code of each key the input host reads; the
   keyboard model latches presses between guest samples, as on Windows. */
uint32_t virtualKey(SDL_Scancode code)
{
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) return 'A' + (code - SDL_SCANCODE_A);
    switch (code) {
    case SDL_SCANCODE_BACKSPACE: return 0x08u;
    case SDL_SCANCODE_RETURN: return 0x0du;
    case SDL_SCANCODE_LEFT: return 0x25u;
    case SDL_SCANCODE_UP: return 0x26u;
    case SDL_SCANCODE_RIGHT: return 0x27u;
    case SDL_SCANCODE_DOWN: return 0x28u;
    default: return RECOMP_INPUT_KEY_COUNT;
    }
}

void pumpEvents(RecompD3dPresenter *presenter)
{
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) && !event.key.repeat) {
            recomp_input_keyboard_event(virtualKey(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
        } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            recomp_input_keyboard_reset();
        } else if (event.type == SDL_EVENT_QUIT) {
            presenter->close_requested = true;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            (event.window.windowID == 0u || event.window.windowID == SDL_GetWindowID(presenter->window))) {
            presenter->close_requested = true;
        }
    }
}

/* Writes BGRA rows, top row first, as a bottom-up 24-bit BMP. */
void writeBmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    const uint32_t row_bytes = width * 3u;
    const uint32_t padded = (row_bytes + 3u) & ~3u;
    const uint32_t image_bytes = padded * height;
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) return;
    uint8_t header[54]{};
    const uint32_t total = 54u + image_bytes, offset = 54u, info_size = 40u;
    const uint16_t planes = 1u, bpp = 24u;
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &total, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &info_size, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    std::fwrite(header, 1, sizeof header, file);
    std::vector<uint8_t> row(padded, 0u);
    for (uint32_t y = 0u; y < height; ++y) {
        const uint8_t *source = pixels + static_cast<size_t>(height - 1u - y) * width * 4u;
        for (uint32_t x = 0u; x < width; ++x) std::memcpy(&row[x * 3u], source + x * 4u, 3u);
        std::fwrite(row.data(), 1, padded, file);
    }
    std::fclose(file);
}

/* Downloads a whole single-sample BGRA texture into a BMP and waits for it. */
void dumpTexture(RecompD3dPresenter *presenter, SDL_GPUTexture *texture, uint32_t width, uint32_t height,
    const char *path)
{
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, width * height * 4u, 0};
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(presenter->device, &info);
    if (download == nullptr) return;
    SDL_GPUCommandBuffer *commands = commandBuffer(presenter);
    if (SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands)) {
        SDL_GPUTextureRegion region{texture, 0u, 0u, 0u, 0u, 0u, width, height, 1u};
        SDL_GPUTextureTransferInfo destination{download, 0u, 0u, 0u};
        SDL_DownloadFromGPUTexture(pass, &region, &destination);
        SDL_EndGPUCopyPass(pass);
    }
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    presenter->command_buffer = nullptr;
    if (fence != nullptr) {
        SDL_WaitForGPUFences(presenter->device, true, &fence, 1u);
        SDL_ReleaseGPUFence(presenter->device, fence);
        if (const auto *pixels = static_cast<const uint8_t *>(
                SDL_MapGPUTransferBuffer(presenter->device, download, false))) {
            writeBmp(path, pixels, width, height);
            SDL_UnmapGPUTransferBuffer(presenter->device, download);
            std::fprintf(stderr, "recomp d3d presenter: frame dump path=%s size=%ux%u present=%u\n",
                path, width, height, presenter->present_count + 1u);
        }
    }
    SDL_ReleaseGPUTransferBuffer(presenter->device, download);
}

/* RECOMP_D3D_FRAME_DUMP names the BMP path; AT and COUNT select presents and
   INTERVAL_MS spaces captures, with the D3D11 backend's rules. */
const char *frameDumpPath(RecompD3dPresenter *presenter, uint32_t present_count, char (&burst)[1024])
{
    const char *path = std::getenv("RECOMP_D3D_FRAME_DUMP");
    if (path == nullptr || path[0] == '\0') return nullptr;
    const char *count_text = std::getenv("RECOMP_D3D_FRAME_DUMP_COUNT");
    const unsigned count = count_text ? static_cast<unsigned>(std::strtoul(count_text, nullptr, 10)) : 1u;
    if (presenter->frame_dump_count >= (count == 0u ? 1u : count)) return nullptr;
    const char *at_text = std::getenv("RECOMP_D3D_FRAME_DUMP_AT");
    const unsigned at = at_text && at_text[0] ? static_cast<unsigned>(std::strtoul(at_text, nullptr, 10)) : 1u;
    if (present_count < at) return nullptr;
    const char *interval_text = std::getenv("RECOMP_D3D_FRAME_DUMP_INTERVAL_MS");
    const uint64_t interval_ms = interval_text ? std::strtoul(interval_text, nullptr, 10) : 0u;
    const uint64_t now = SDL_GetTicks();
    if (interval_ms != 0u && now < presenter->next_frame_dump_ms) return nullptr;
    presenter->next_frame_dump_ms = now + interval_ms;
    const unsigned index = presenter->frame_dump_count++;
    if (count > 1u) {
        std::snprintf(burst, sizeof burst, "%s.%03u.bmp", path, index);
        return burst;
    }
    return path;
}

const char kGammaShader[] =
    "[[vk::binding(0, 2)]] Texture2D pixels : register(t0);\n"
    "[[vk::binding(0, 2)]] SamplerState pixel_sampler : register(s0);\n"
    "[[vk::binding(0, RECOMP_UNIFORM_SET)]] cbuffer Gamma : register(b0) { float4 ramp[256]; };\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    " float2 p = float2((id << 1) & 2, id & 2);\n"
    " return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }\n"
    "float4 ps_main(float4 p : SV_Position) : SV_Target {\n"
    " float4 c = pixels.Load(int3(p.xy, 0));\n"
    " uint3 i = (uint3)(saturate(c.rgb) * 255 + 0.5);\n"
    " return float4(ramp[i.r].r, ramp[i.g].g, ramp[i.b].b, c.a); }\n";

/* SMAA 1x (iryoku/smaa, ultra preset) on the final resolved image, from the
   same HLSL as the Direct3D 11 presenter. Textures 0-4 are color, edges,
   area, search and weights; slots 0 and 1 also carry the linear and point
   samplers the shader names. */
bool createSmaa(RecompD3dPresenter *presenter)
{
#ifdef RECOMP_SMAA
    static const char prefix[] =
        "#define SMAA_CUSTOM_SL\n#define SMAA_PRESET_ULTRA\n"
        "[[vk::binding(0, 2)]] SamplerState LinearSampler : register(s0);\n"
        "[[vk::binding(1, 2)]] SamplerState PointSampler : register(s1);\n"
        "#define SMAATexture2D(tex) Texture2D tex\n"
        "#define SMAATexturePass2D(tex) tex\n"
        "#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)\n"
        "#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)\n"
        "#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)\n"
        "#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)\n"
        "#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)\n"
        "#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)\n"
        "#define SMAA_FLATTEN [flatten]\n#define SMAA_BRANCH [branch]\n";
    static const char suffix[] =
        "\n[[vk::binding(0, 2)]] Texture2D colorTex : register(t0);\n"
        "[[vk::binding(1, 2)]] Texture2D edgesTex : register(t1);\n"
        "[[vk::binding(2, 2)]] Texture2D areaTex : register(t2);\n"
        "[[vk::binding(3, 2)]] Texture2D searchTex : register(t3);\n"
        "[[vk::binding(4, 2)]] Texture2D blendTex : register(t4);\n"
        "void fullscreen(uint id, out float4 pos, out float2 uv) {\n"
        " uv = float2((id << 1) & 2, id & 2); pos = float4(uv * float2(2,-2) + float2(-1,1), 0, 1); }\n"
        "void vs_edges(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0,\n"
        " out float4 offset[3] : TEXCOORD1) { fullscreen(id, pos, uv); SMAAEdgeDetectionVS(uv, offset); }\n"
        "float2 ps_edges(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 offset[3] : TEXCOORD1)\n"
        " : SV_Target { return SMAALumaEdgeDetectionPS(uv, offset, colorTex); }\n"
        "void vs_weights(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0,\n"
        " out float2 pix : TEXCOORD1, out float4 offset[3] : TEXCOORD2) {\n"
        " fullscreen(id, pos, uv); SMAABlendingWeightCalculationVS(uv, pix, offset); }\n"
        "float4 ps_weights(float4 pos : SV_Position, float2 uv : TEXCOORD0, float2 pix : TEXCOORD1,\n"
        " float4 offset[3] : TEXCOORD2) : SV_Target {\n"
        " return SMAABlendingWeightCalculationPS(uv, pix, offset, edgesTex, areaTex, searchTex, 0); }\n"
        "void vs_blend(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0,\n"
        " out float4 offset : TEXCOORD1) { fullscreen(id, pos, uv); SMAANeighborhoodBlendingVS(uv, offset); }\n"
        "float4 ps_blend(float4 pos : SV_Position, float2 uv : TEXCOORD0, float4 offset : TEXCOORD1)\n"
        " : SV_Target { return SMAANeighborhoodBlendingPS(uv, offset, colorTex, blendTex); }\n";
    const uint32_t width = mainWidth(presenter), height = mainHeight(presenter);
    char metrics[160];
    std::snprintf(metrics, sizeof metrics, "SMAA_RT_METRICS=float4(1.0/%u.0,1.0/%u.0,%u.0,%u.0)",
        width, height, width, height);
    std::string source = prefix;
    source.append(reinterpret_cast<const char *>(kSmaaSource), sizeof kSmaaSource);
    source += suffix;
    const std::vector<std::string> defines = {metrics};
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

bool ensureGammaPass(RecompD3dPresenter *presenter)
{
    if (presenter->gamma_pipeline != nullptr) return true;
    SDL_GPUShader *vertex = createShader(presenter, kGammaShader, true, {}, 0u, 0u);
    SDL_GPUShader *fragment = vertex ? createShader(presenter, kGammaShader, false, {}, 1u, 1u) : nullptr;
    presenter->gamma_target = fragment ? createTexture(presenter, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
        mainWidth(presenter), mainHeight(presenter)) : nullptr;
    if (presenter->gamma_target != nullptr) {
        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1u;
        presenter->gamma_pipeline = SDL_CreateGPUGraphicsPipeline(presenter->device, &info);
    }
    if (presenter->gamma_pipeline == nullptr) {
        std::fprintf(stderr, "recomp d3d presenter: gamma pass unavailable: %s\n", SDL_GetError());
        if (presenter->gamma_target != nullptr) SDL_ReleaseGPUTexture(presenter->device, presenter->gamma_target);
        presenter->gamma_target = nullptr;
    }
    if (vertex != nullptr) SDL_ReleaseGPUShader(presenter->device, vertex);
    if (fragment != nullptr) SDL_ReleaseGPUShader(presenter->device, fragment);
    return presenter->gamma_pipeline != nullptr;
}

RecompD3dPresenterError submitGamma(RecompD3dPresenter *presenter, const uint8_t (&ramp)[3][256])
{
    bool enabled = false;
    float values[256][4]{};
    for (unsigned i = 0u; i < 256u; ++i) {
        for (unsigned channel = 0u; channel < 3u; ++channel) {
            values[i][channel] = ramp[channel][i] / 255.0f;
            enabled |= ramp[channel][i] != i;
        }
    }
    if (enabled && !ensureGammaPass(presenter)) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    std::memcpy(presenter->gamma_ramp, values, sizeof values);
    presenter->gamma_enabled = enabled;
    return RECOMP_D3D_PRESENTER_OK;
}

void reportPerformance(RecompD3dPresenter *presenter)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (presenter->second_presents++ == 0u) presenter->second_start = now;
    else presenter->second_max_ms = std::max(presenter->second_max_ms,
        std::chrono::duration<double, std::milli>(now - presenter->last_present).count());
    presenter->last_present = now;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - presenter->second_start).count();
    if (elapsed_ms < 1000.0) return;
    const uint32_t frames = presenter->second_presents - 1u;
    const double fps = frames * 1000.0 / elapsed_ms;
    char title[96];
    std::snprintf(title, sizeof title, "DOA2U Recomp%s | %.1f FPS | %.1f ms/frame",
        unpacedRun() ? " (unpaced)" : "", fps, elapsed_ms / frames);
    SDL_SetWindowTitle(presenter->window, title);
    std::fprintf(stderr, "recomp performance: present=%u fps=%.2f frame_ms=%.3f max_ms=%.1f draws=%u\n",
        presenter->present_count, fps, elapsed_ms / frames, presenter->second_max_ms,
        frames ? presenter->draw_count / frames : 0u);
    presenter->second_presents = 1u;
    presenter->second_start = now;
    presenter->second_max_ms = 0.0;
    presenter->draw_count = 0u;
}

RecompD3dPresenterError submitPresent(RecompD3dPresenter *presenter, const RecompD3dPresenterPresentCommand &present)
{
    if (present.effective_flags != 5u || present.swap_counter == 0u) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    pumpEvents(presenter);
    if (presenter->close_requested) return RECOMP_D3D_PRESENTER_CLOSED;
    flushSegment(presenter);
    resolveMain(presenter);
    SDL_GPUCommandBuffer *commands = commandBuffer(presenter);
    if (commands == nullptr) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    /* Keep guest pixels uncorrected: gamma is display state and must not feed
       back into later draws or compound when a frame is presented twice. */
    SDL_GPUTexture *shown = presenter->main_resolve;
    if (presenter->smaa && presenter->smaa_pipelines[0] == nullptr && !createSmaa(presenter)) {
        presenter->smaa = false;
    }
    if (presenter->smaa) shown = runSmaa(presenter, commands, shown);
    if (presenter->gamma_enabled) {
        SDL_GPUColorTargetInfo target{};
        target.texture = presenter->gamma_target;
        target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        target.store_op = SDL_GPU_STOREOP_STORE;
        if (SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr)) {
            SDL_BindGPUGraphicsPipeline(pass, presenter->gamma_pipeline);
            const SDL_GPUTextureSamplerBinding pixels{shown, presenter->point_sampler};
            SDL_BindGPUFragmentSamplers(pass, 0u, &pixels, 1u);
            SDL_PushGPUFragmentUniformData(commands, 0u, presenter->gamma_ramp, sizeof presenter->gamma_ramp);
            SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
            SDL_EndGPURenderPass(pass);
            shown = presenter->gamma_target;
        }
    }
    SDL_GPUTexture *swapchain = nullptr;
    Uint32 swap_width = 0u, swap_height = 0u;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(commands, presenter->window, &swapchain, &swap_width, &swap_height) &&
        swapchain != nullptr && swap_width != 0u && swap_height != 0u) {
        // The window keeps the guest aspect; the rest of the swapchain is black.
        const float source_aspect = static_cast<float>(presentClientWidth(presenter, mainHeight(presenter))) /
            mainHeight(presenter);
        int x = 0, y = 0, width = static_cast<int>(swap_width), height = static_cast<int>(swap_height);
        if (static_cast<float>(swap_width) / swap_height > source_aspect) {
            width = static_cast<int>(swap_height * source_aspect + 0.5f);
            x = (static_cast<int>(swap_width) - width) / 2;
        } else {
            height = static_cast<int>(swap_width / source_aspect + 0.5f);
            y = (static_cast<int>(swap_height) - height) / 2;
        }
        SDL_GPUBlitInfo blit{};
        blit.source.texture = shown;
        blit.source.w = mainWidth(presenter);
        blit.source.h = mainHeight(presenter);
        blit.destination.texture = swapchain;
        blit.destination.x = static_cast<Uint32>(x);
        blit.destination.y = static_cast<Uint32>(y);
        blit.destination.w = static_cast<Uint32>(width);
        blit.destination.h = static_cast<Uint32>(height);
        blit.load_op = SDL_GPU_LOADOP_CLEAR;
        blit.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
    }
    // Capture what the window shows, at the main target size, as the D3D11 backend does.
    char burst[1024];
    if (const char *path = frameDumpPath(presenter, presenter->present_count + 1u, burst)) {
        dumpTexture(presenter, shown, mainWidth(presenter), mainHeight(presenter), path);
    }
    if (presenter->draw_log_active) {
        char path[1024];
        std::snprintf(path, sizeof path, "%s-%u.bmp", presenter->draw_log_trigger, presenter->present_count + 1u);
        dumpTexture(presenter, shown, mainWidth(presenter), mainHeight(presenter), path);
        for (RenderTargetEntry &entry : presenter->render_targets) {
            if (entry.multisampled && !entry.resolved) {
                resolveTarget(presenter, entry.render, entry.resolve);
                entry.resolved = true;
            }
            std::snprintf(path, sizeof path, "%s-%u-rt%08x.bmp", presenter->draw_log_trigger,
                presenter->present_count + 1u, entry.desc.data);
            dumpTexture(presenter, entry.resolve, entry.host_width, entry.host_height, path);
        }
        std::fprintf(stderr, "recomp draw log: end present=%u draws=%u\n",
            presenter->present_count + 1u, presenter->draw_log_index);
        presenter->draw_log_active = false;
        unlink(presenter->draw_log_trigger);
    }
    /* Keep the presented guest frame for GetBackBuffer(-1) once a draw has used it. */
    if (presenter->previous_frame != nullptr) copyGuestBuffer(presenter, presenter->previous_frame);
    submitCommands(presenter);
    ++presenter->present_count;
    if (presenter->draw_log_trigger != nullptr && !presenter->draw_log_active &&
        fileExists(presenter->draw_log_trigger)) {
        presenter->draw_log_active = true;
        presenter->draw_log_index = 0u;
        std::fprintf(stderr, "recomp draw log: begin present=%u\n", presenter->present_count + 1u);
    }
    if (presenter->performance_counter) reportPerformance(presenter);
    return RECOMP_D3D_PRESENTER_OK;
}

void logDraw(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &d,
    RecompD3dPresenterError result, double us)
{
    const auto describe = [](bool on, const RecompD3dTextureDesc &t, char (&out)[48]) {
        if (on) std::snprintf(out, sizeof out, "%02x:%ux%u@%08x%s", t.format_byte,
            t.width, t.height, t.data, t.render_target ? "rt" : "");
    };
    char tex[48] = "-", mask[48] = "-", refl[48] = "-", refl2[48] = "-", target[48] = "back";
    describe(d.has_texture, d.texture, tex);
    describe(d.has_alpha_mask, d.alpha_mask, mask);
    describe(d.has_reflection || d.bump_env || d.bump_water || d.distortion, d.reflection_texture, refl);
    describe(d.reflection_stage2, d.reflection_stage2_texture, refl2);
    describe(d.target.offscreen, d.target.color, target);
    const struct { bool on; const char *name; } features[] = {
        {d.directional.enabled, "light"}, {d.blend_weight_count != 0u, "skin"},
        {d.texgen_reflection, "texgen"}, {d.texgen_projected, "proj"},
        {d.texture_transform_uv, "uvxf"}, {d.reflection_mesh_uv, "reflmesh"},
        {d.distortion, "distort"}, {d.bump_env, "bump"}, {d.bump_water, "water"},
        {d.four_tap_filter, "4tap"}, {d.bright_pass, "bright"}, {d.luminance_tint, "lumtint"},
        {d.alpha_kill, "akill"}, {d.stage1_alpha_mask, "s1mask"},
        {d.texture_is_backbuffer, "fromback"}, {d.texture_is_front_buffer, "fromfront"},
        {d.has_scissor, "scissor"}, {d.target.no_depth, "nodepth"}};
    char flags[192] = "";
    size_t used = 0u;
    for (const auto &feature : features) {
        if (feature.on && used < sizeof flags)
            used += std::snprintf(flags + used, sizeof flags - used, "%s%s", used ? "," : "", feature.name);
    }
    std::fprintf(stderr,
        "recomp draw: present=%u n=%u us=%.1f err=%d prim=%u idx=%u tri=%u vtx=%u stride=%u "
        "fvf=%08x prog=%u tex=%s mask=%s refl=%s refl2=%s target=%s blend=%d:%d/%d/%d "
        "depth=%d%d:%d atest=%d:%d:%u cull=%d vp=%g,%g,%g,%g flags=%s\n",
        presenter->present_count + 1u, presenter->draw_log_index, us, static_cast<int>(result),
        d.primitive_type, d.index_count, d.triangle_count, d.vertex_count, d.vertex_stride,
        d.fvf, d.program_count, tex, mask, refl, refl2, target,
        d.blend.blend_enable ? 1 : 0, static_cast<int>(d.blend.src_factor),
        static_cast<int>(d.blend.dst_factor), static_cast<int>(d.blend.op),
        d.depth.depth_test_enable ? 1 : 0, d.depth.depth_write_enable ? 1 : 0,
        static_cast<int>(d.depth.depth_func), d.depth.alpha_test_enable ? 1 : 0,
        static_cast<int>(d.depth.alpha_func), d.depth.alpha_ref, static_cast<int>(d.cull_mode),
        d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], flags[0] ? flags : "-");
    ++presenter->draw_log_index;
}

void releasePresenter(RecompD3dPresenter *presenter)
{
    if (presenter->device == nullptr) return;
    flushSegment(presenter);
    submitCommands(presenter);
    SDL_WaitForGPUIdle(presenter->device);
    releaseQuery(presenter, presenter->visibility_active);
    for (VisibilityQuery &query : presenter->visibility_pending) releaseQuery(presenter, query);
    for (VisibilityQuery &query : presenter->visibility_free) releaseQuery(presenter, query);
    for (TextureEntry &entry : presenter->textures) {
        if (entry.texture != nullptr) SDL_ReleaseGPUTexture(presenter->device, entry.texture);
    }
    releaseRetiredTextures(presenter);
    for (RenderTargetEntry &entry : presenter->render_targets) {
        if (entry.resolve != nullptr && entry.resolve != entry.render) SDL_ReleaseGPUTexture(presenter->device, entry.resolve);
        if (entry.render != nullptr) SDL_ReleaseGPUTexture(presenter->device, entry.render);
    }
    for (DepthTargetEntry &entry : presenter->depth_targets) SDL_ReleaseGPUTexture(presenter->device, entry.texture);
    for (auto &entry : presenter->pipelines) {
        if (entry.second != nullptr) SDL_ReleaseGPUGraphicsPipeline(presenter->device, entry.second);
    }
    for (auto &entry : presenter->shaders) {
        if (entry.second.vertex != nullptr) SDL_ReleaseGPUShader(presenter->device, entry.second.vertex);
        if (entry.second.fragment != nullptr) SDL_ReleaseGPUShader(presenter->device, entry.second.fragment);
    }
    for (SamplerEntry &entry : presenter->samplers) SDL_ReleaseGPUSampler(presenter->device, entry.sampler);
    for (SDL_GPUSampler *sampler : {presenter->draw_sampler, presenter->filter_sampler,
            presenter->program_mask_sampler, presenter->point_sampler}) {
        if (sampler != nullptr) SDL_ReleaseGPUSampler(presenter->device, sampler);
    }
    if (presenter->gamma_pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(presenter->device, presenter->gamma_pipeline);
    for (SDL_GPUGraphicsPipeline *pipeline : presenter->smaa_pipelines) {
        if (pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(presenter->device, pipeline);
    }
    for (SDL_GPUTexture *texture : presenter->smaa_textures) {
        if (texture != nullptr) SDL_ReleaseGPUTexture(presenter->device, texture);
    }
    for (SDL_GPUTexture *texture : {presenter->gamma_target, presenter->back_buffer_copy, presenter->previous_frame,
            presenter->white_texture, presenter->white_volume, presenter->main_depth}) {
        if (texture != nullptr) SDL_ReleaseGPUTexture(presenter->device, texture);
    }
    if (presenter->main_resolve != nullptr && presenter->main_resolve != presenter->main_color)
        SDL_ReleaseGPUTexture(presenter->device, presenter->main_resolve);
    if (presenter->main_color != nullptr) SDL_ReleaseGPUTexture(presenter->device, presenter->main_color);
    for (SDL_GPUBuffer *buffer : {presenter->vertex_buffer, presenter->index_buffer}) {
        if (buffer != nullptr) SDL_ReleaseGPUBuffer(presenter->device, buffer);
    }
    for (SDL_GPUTransferBuffer *buffer : {presenter->vertex_transfer, presenter->index_transfer}) {
        if (buffer != nullptr) SDL_ReleaseGPUTransferBuffer(presenter->device, buffer);
    }
    if (presenter->window != nullptr) {
        SDL_ReleaseWindowFromGPUDevice(presenter->device, presenter->window);
        SDL_DestroyWindow(presenter->window);
    }
    SDL_DestroyGPUDevice(presenter->device);
    presenter->device = nullptr;
}

SDL_GPUSampleCount supportedSamples(SDL_GPUDevice *device, SDL_GPUTextureFormat depth, uint32_t requested)
{
    const struct { uint32_t count; SDL_GPUSampleCount samples; } counts[] = {
        {8u, SDL_GPU_SAMPLECOUNT_8}, {4u, SDL_GPU_SAMPLECOUNT_4}, {2u, SDL_GPU_SAMPLECOUNT_2}};
    for (const auto &count : counts) {
        if (count.count <= requested &&
            SDL_GPUTextureSupportsSampleCount(device, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, count.samples) &&
            SDL_GPUTextureSupportsSampleCount(device, depth, count.samples) &&
            SDL_GPUTextureSupportsSampleCount(device, kCountFormat, count.samples)) {
            return count.samples;
        }
    }
    return SDL_GPU_SAMPLECOUNT_1;
}

bool createResources(RecompD3dPresenter *presenter)
{
    const uint32_t width = mainWidth(presenter), height = mainHeight(presenter);
    presenter->main_color = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (presenter->msaa_count == 1u ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0u),
        width, height, 1u, 1u, presenter->msaa);
    presenter->main_resolve = presenter->msaa_count == 1u ? presenter->main_color : createTexture(presenter,
        SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, width, height);
    presenter->main_depth = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D, presenter->depth_format,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, width, height, 1u, 1u, presenter->msaa);
    presenter->white_texture = createTexture(presenter, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1u, 1u);
    presenter->white_volume = createTexture(presenter, SDL_GPU_TEXTURETYPE_3D,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1u, 1u, 1u);
    if (presenter->main_color == nullptr || presenter->main_resolve == nullptr ||
        presenter->main_depth == nullptr || presenter->white_texture == nullptr ||
        presenter->white_volume == nullptr) return false;
    const uint32_t white = 0xffffffffu;
    if (!uploadTexture(presenter, presenter->white_texture, &white, 4u, 0u, 1u, 1u) ||
        !uploadTexture(presenter, presenter->white_volume, &white, 4u, 0u, 1u, 1u)) return false;
    presenter->draw_sampler = createSampler(presenter, SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
        SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_SAMPLERADDRESSMODE_REPEAT, false, 0.0f);
    presenter->filter_sampler = createSampler(presenter, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, true, 0.0f);
    presenter->program_mask_sampler = createSampler(presenter, SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
        SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_SAMPLERADDRESSMODE_REPEAT, true, 0.0f);
    presenter->point_sampler = createSampler(presenter, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, true, 0.0f,
        SDL_GPU_FILTER_NEAREST);
    if (presenter->draw_sampler == nullptr || presenter->filter_sampler == nullptr ||
        presenter->program_mask_sampler == nullptr || presenter->point_sampler == nullptr) return false;
    return ensureRingCapacity(presenter, presenter->vertex_buffer, presenter->vertex_transfer,
            presenter->vertex_capacity, presenter->vertex_used, 0u, SDL_GPU_BUFFERUSAGE_VERTEX) &&
        ensureRingCapacity(presenter, presenter->index_buffer, presenter->index_transfer,
            presenter->index_capacity, presenter->index_used, 0u, SDL_GPU_BUFFERUSAGE_INDEX);
}

RecompD3dPresenterError checkOwner(RecompD3dPresenter *presenter)
{
    if (presenter == nullptr || presenter != active_presenter) return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    return std::this_thread::get_id() == presenter->owner_thread
        ? RECOMP_D3D_PRESENTER_OK : RECOMP_D3D_PRESENTER_WRONG_THREAD;
}

} // namespace

RecompD3dPresenterError d3d_backend_create(const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr || config->width == 0u || config->height == 0u ||
        config->color_format != RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM ||
        config->depth_format != RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != nullptr || active_presenter != nullptr) return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    if (!recomp_shader_compiler_available()) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "recomp d3d presenter: SDL video failed: %s\n", SDL_GetError());
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    auto *created = new (std::nothrow) RecompD3dPresenter;
    if (created == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    created->config = *config;
    created->owner_thread = std::this_thread::get_id();
    created->widescreen = envFlag("RECOMP_D3D_WIDESCREEN");
    created->performance_counter = envFlag("RECOMP_PERF_COUNTER");
    created->draw_log_trigger = std::getenv("RECOMP_D3D_DRAW_LOG");
    created->smaa = envFlag("RECOMP_D3D_SMAA");
    if (const char *scale = std::getenv("RECOMP_D3D_SCALE")) {
        const float value = static_cast<float>(std::atof(scale));
        if (std::isfinite(value)) created->scale = std::clamp(value, 1.0f, 8.0f);
    }
    const auto fail = [&](const char *what) {
        std::fprintf(stderr, "recomp d3d presenter: %s: %s\n", what, SDL_GetError());
        releasePresenter(created);
        delete created;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    };
    created->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (created->device == nullptr) return fail("SDL_GPU device");
    if (SDL_GetGPUShaderFormats(created->device) & SDL_GPU_SHADERFORMAT_SPIRV) {
        created->shader_target = RecompShaderTarget::Spirv;
        created->shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
    } else {
        created->shader_target = RecompShaderTarget::Msl;
        created->shader_format = SDL_GPU_SHADERFORMAT_MSL;
    }
    /* D24S8 is missing on Apple GPUs and AMD's Vulkan drivers. Its constant
       depth bias unit matches D32F's for depths between 0.5 and 1. */
    if (!SDL_GPUTextureSupportsFormat(created->device, created->depth_format, SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        created->depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    }
    if (const char *msaa = std::getenv("RECOMP_D3D_MSAA")) {
        created->msaa = supportedSamples(created->device, created->depth_format,
            static_cast<uint32_t>(std::clamp(std::atoi(msaa), 1, 32)));
        created->msaa_count = sampleCount(created->msaa);
    }
    created->copy_mips = created->scale != 1.0f;

    const uint32_t main_width = mainWidth(created), main_height = mainHeight(created);
    const uint32_t display_width = presentClientWidth(created, main_height);
    uint32_t window_width = display_width, window_height = main_height;
    if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay())) {
        // Beyond the screen the swapchain downsamples into a screen-sized window.
        const uint32_t max_width = static_cast<uint32_t>(mode->w);
        const uint32_t max_height = static_cast<uint32_t>(mode->h > 80 ? mode->h - 80 : mode->h);
        const float aspect = static_cast<float>(display_width) / main_height;
        if (window_width > max_width) {
            window_width = max_width;
            window_height = static_cast<uint32_t>(window_width / aspect + 0.5f);
        }
        if (window_height > max_height) {
            window_height = max_height;
            window_width = static_cast<uint32_t>(window_height * aspect + 0.5f);
        }
    }
    // Steam sets these in Game Mode and on the Deck, where gamescope would only letterbox a window.
    const bool steam = envFlag("SteamTenfoot") || envFlag("SteamDeck");
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (std::getenv("RECOMP_HEADLESS") != nullptr) flags |= SDL_WINDOW_HIDDEN;
    else if (steam) flags |= SDL_WINDOW_FULLSCREEN;
    created->window = SDL_CreateWindow(unpacedRun() ? "DOA2U Recomp (unpaced)" : "DOA2U Recomp",
        static_cast<int>(window_width), static_cast<int>(window_height), flags);
    if (created->window == nullptr) return fail("window");
    if (!SDL_ClaimWindowForGPUDevice(created->device, created->window)) return fail("swapchain");
    const SDL_GPUPresentMode present_mode = preferMailbox(immediate_present) &&
            SDL_WindowSupportsGPUPresentMode(created->device, created->window, SDL_GPU_PRESENTMODE_MAILBOX)
        ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
    SDL_SetGPUSwapchainParameters(created->device, created->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present_mode);
    if (!createResources(created)) return fail("presenter resources");
    std::fprintf(stderr, "recomp d3d presenter: %s target=%ux%u msaa=%u window=%ux%u\n",
        SDL_GetGPUDeviceDriver(created->device), main_width, main_height, created->msaa_count,
        window_width, window_height);
    try {
        created->precompile = std::thread(precompileDrawShaders, created);
    } catch (const std::system_error &) {
        // Shaders then compile on first use.
    }
    active_presenter = created;
    *presenter = created;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_submit(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    const RecompD3dPresenterError owner = checkOwner(presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    // Events are pumped at each present; a close reaches the guest there and stays.
    if (presenter->close_requested) return RECOMP_D3D_PRESENTER_CLOSED;
    if (command == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
        return submitGamma(presenter, command->data.gamma);
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR:
        return submitClear(presenter, command->data.clear);
    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT:
        return submitPresent(presenter, command->data.present);
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW: {
        const auto start = std::chrono::steady_clock::now();
        const auto error = submitDraw(presenter, command->data.draw);
        if (presenter->draw_log_active) {
            logDraw(presenter, command->data.draw, error, std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count());
        }
        return error;
    }
    case RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN:
        return beginVisibility(presenter);
    case RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END: {
        BoundTarget last = presenter->last_bound;
        if (last.host_width == 0u) {
            last.scale_x = static_cast<float>(mainWidth(presenter)) / presenter->config.width;
            last.scale_y = static_cast<float>(mainHeight(presenter)) / presenter->config.height;
        }
        return endVisibility(presenter, command->data.visibility_index, last);
    }
    case RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR:
        return submitCopyMainColor(presenter, command->data.copy_destination);
    default:
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
}

RecompD3dPresenterError d3d_backend_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered)
{
    if (covered) *covered = true;
    const RecompD3dPresenterError owner = checkOwner(presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    if (draw == nullptr || covered == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    RecompD3dVertexLayout layout;
    if (draw->program_count != 0u || !recomp_d3d_fvf_layout(draw->fvf, &layout) ||
        layout.pretransformed || draw->primitive_type < 5u || draw->primitive_type > 7u ||
        !draw->has_transform || draw->blend_weight_count > 3u ||
        (draw->blend_weight_count != 0u && draw->blend_weight_count != layout.blend_weight_count))
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    if (draw->vertex_bytes == nullptr || draw->index_bytes == nullptr ||
        draw->vertex_count == 0u || draw->vertex_count > 65536u || draw->index_count < 3u ||
        static_cast<uint64_t>(draw->index_count) * sizeof(uint16_t) > 64u * 1024u * 1024u ||
        draw->vertex_stride < layout.stride ||
        static_cast<uint64_t>(draw->vertex_stride) * draw->vertex_count > 64u * 1024u * 1024u ||
        draw->triangle_count != recomp_d3d_draw_triangle_count(draw->primitive_type, draw->index_count))
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const auto *indices = static_cast<const uint16_t *>(draw->index_bytes);
    for (uint32_t i = 0u; i < draw->index_count; ++i) {
        if (indices[i] >= draw->vertex_count) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
        const auto *vertex = static_cast<const uint8_t *>(draw->vertex_bytes) +
            static_cast<size_t>(indices[i]) * draw->vertex_stride;
        for (uint32_t j = 0u; j < 3u + draw->blend_weight_count; ++j) {
            float value;
            std::memcpy(&value, vertex + j * sizeof value, sizeof value);
            if (!std::isfinite(value)) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
        }
    }
    for (uint32_t i = 0u; i <= draw->blend_weight_count; ++i) {
        const float *matrix = i == 0u ? draw->transform : draw->blend_transforms[i - 1u];
        for (uint32_t j = 0u; j < 16u; ++j)
            if (!std::isfinite(matrix[j])) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    for (float value : draw->viewport)
        if (!std::isfinite(value)) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    if (draw->viewport[2] < 0.0f || (draw->viewport[2] > 0.0f &&
        (draw->viewport[3] <= 0.0f || draw->viewport[4] < 0.0f || draw->viewport[5] > 1.0f ||
         draw->viewport[4] > draw->viewport[5])))
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    /* A nested probe would add its samples to the guest's open query. */
    if (presenter->visibility_open) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    if (!std::isfinite(draw->screen_space_offset[0]) || !std::isfinite(draw->screen_space_offset[1]) ||
        static_cast<unsigned>(draw->cull_mode) > RECOMP_D3D_CULL_COUNTER_CLOCKWISE)
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;

    BoundTarget bound;
    const RecompD3dPresenterError target_error = bindTarget(presenter, draw->target, bound);
    if (target_error != RECOMP_D3D_PRESENTER_OK) return target_error;
    float rect[4] = {0.0f, 0.0f, static_cast<float>(bound.host_width), static_cast<float>(bound.host_height)};
    float clip_rect[4] = {rect[0], rect[1], rect[2], rect[3]};
    if (draw->has_scissor) {
        if (draw->scissor[0] < 0 || draw->scissor[1] < 0 ||
            draw->scissor[2] < draw->scissor[0] || draw->scissor[3] < draw->scissor[1])
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        for (unsigned i = 0u; i < 4u; ++i) {
            const double edge = draw->scissor[i] * static_cast<double>(i % 2u == 0u ? bound.scale_x : bound.scale_y);
            if (!std::isfinite(edge) || edge > 32767.0) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            const float rounded = static_cast<float>(std::floor(edge + 0.5));
            clip_rect[i] = i < 2u ? std::max(clip_rect[i], rounded) : std::min(clip_rect[i], rounded);
        }
    }
    if (draw->viewport[2] > 0.0f) {
        const float guest[4] = {draw->viewport[0] * bound.scale_x, draw->viewport[1] * bound.scale_y,
            draw->viewport[2] * bound.scale_x, draw->viewport[3] * bound.scale_y};
        if (guest[0] < -32768.0f || guest[1] < -32768.0f || guest[0] + guest[2] > 32767.0f ||
            guest[1] + guest[3] > 32767.0f) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        rect[0] = guest[0];
        rect[1] = guest[1];
        rect[2] = guest[0] + guest[2];
        rect[3] = guest[1] + guest[3];
        // The viewport clips too, as D3D8 clips to it.
        clip_rect[0] = std::max(clip_rect[0], rect[0]);
        clip_rect[1] = std::max(clip_rect[1], rect[1]);
        clip_rect[2] = std::min(clip_rect[2], rect[2]);
        clip_rect[3] = std::min(clip_rect[3], rect[3]);
    }
    // The shared constants compose the screen offset into every transform.
    RecompD3dPresenterDrawCommand geometry{};
    geometry.fvf = draw->fvf;
    geometry.has_transform = true;
    geometry.blend_weight_count = draw->blend_weight_count;
    std::memcpy(geometry.transform, draw->transform, sizeof geometry.transform);
    std::memcpy(geometry.blend_transforms, draw->blend_transforms, sizeof geometry.blend_transforms);
    std::memcpy(geometry.viewport, draw->viewport, sizeof geometry.viewport);
    std::memcpy(geometry.screen_space_offset, draw->screen_space_offset, sizeof geometry.screen_space_offset);
    RecompD3dDrawTargetState target_state{};
    target_state.viewport[2] = static_cast<float>(bound.host_width);
    target_state.viewport[3] = static_cast<float>(bound.host_height);
    target_state.viewport[5] = 1.0f;
    target_state.scale_x = bound.scale_x;
    target_state.scale_y = bound.scale_y;
    float constants[kRecompD3dDrawConstantFloats];
    if (!recomp_d3d_draw_constants(geometry, layout, target_state, false, false, constants))
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    const auto transform = [&](uint16_t index, float (&clip)[4]) {
        const auto *vertex = static_cast<const uint8_t *>(draw->vertex_bytes) +
            static_cast<size_t>(index) * draw->vertex_stride;
        float position[3], weights[3] = {};
        std::memcpy(position, vertex, sizeof position);
        std::memcpy(weights, vertex + 12u, draw->blend_weight_count * sizeof(float));
        const auto apply = [&](const float *matrix, float weight) {
            for (unsigned column = 0u; column < 4u; ++column) {
                clip[column] += weight * (position[0] * matrix[column] + position[1] * matrix[4u + column] +
                    position[2] * matrix[8u + column] + matrix[12u + column]);
            }
        };
        std::fill(clip, clip + 4, 0.0f);
        if (draw->blend_weight_count == 0u) {
            apply(constants, 1.0f);
            return;
        }
        float remainder = 1.0f;
        for (uint32_t i = 0u; i < draw->blend_weight_count; ++i) {
            apply(constants + 16u * i, weights[i]);
            remainder -= weights[i];
        }
        apply(constants + 16u * draw->blend_weight_count, remainder);
    };
    if (clip_rect[2] <= clip_rect[0] || clip_rect[3] <= clip_rect[1]) {
        *covered = false;
        return RECOMP_D3D_PRESENTER_OK;
    }
    bool hit = false;
    for (uint32_t t = 0u; t < draw->triangle_count && !hit; ++t) {
        uint16_t corner[3];
        if (draw->primitive_type == RECOMP_D3D_PT_TRIANGLELIST) {
            corner[0] = indices[t * 3u]; corner[1] = indices[t * 3u + 1u]; corner[2] = indices[t * 3u + 2u];
        } else if (draw->primitive_type == RECOMP_D3D_PT_TRIANGLESTRIP) {
            // Odd strip triangles swap two corners to keep their winding.
            corner[0] = indices[t]; corner[1] = indices[t + 1u + (t & 1u)]; corner[2] = indices[t + 2u - (t & 1u)];
        } else {
            corner[0] = indices[0]; corner[1] = indices[t + 1u]; corner[2] = indices[t + 2u];
        }
        float clip[3][4];
        for (unsigned i = 0u; i < 3u; ++i) transform(corner[i], clip[i]);
        hit = triangleCovers(clip, draw->cull_mode, rect, clip_rect, bound.samples);
    }
    *covered = hit;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size, bool targets)
{
    const RecompD3dPresenterError owner = checkOwner(presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    if (base >= 0x80000000u && base < 0x84000000u) base -= 0x80000000u;
    const uint64_t end = static_cast<uint64_t>(base) + size;
    if (size == 0u || end > 0x100000000ull) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    // Queued draws may still bind what is released here.
    flushSegment(presenter);
    const auto released = [base, end](uint32_t data) { return data >= base && data < end; };
    const auto colors = presenter->render_targets.size();
    const auto depths = presenter->depth_targets.size();
    for (size_t i = 0u; targets && i < presenter->render_targets.size();) {
        RenderTargetEntry &entry = presenter->render_targets[i];
        if (!released(entry.desc.data)) { ++i; continue; }
        if (entry.resolve != entry.render) retireTexture(presenter, entry.resolve);
        retireTexture(presenter, entry.render);
        presenter->target_bytes -= entry.bytes;
        entry = presenter->render_targets.back();
        presenter->render_targets.pop_back();
    }
    for (size_t i = 0u; targets && i < presenter->depth_targets.size();) {
        DepthTargetEntry &entry = presenter->depth_targets[i];
        if (!released(entry.desc.data)) { ++i; continue; }
        retireTexture(presenter, entry.texture);
        presenter->target_bytes -= entry.bytes;
        entry = presenter->depth_targets.back();
        presenter->depth_targets.pop_back();
    }
    for (uint32_t i = 0u; i < presenter->texture_count; ++i) {
        TextureEntry &entry = presenter->textures[i];
        if (entry.used && released(entry.data)) {
            unindexTexture(presenter, i);
            retireTexture(presenter, entry.texture);
            entry.used = false;
        }
    }
    // The segment no longer names a released target.
    presenter->segment.color = presenter->segment.depth = nullptr;
    releaseRetiredTextures(presenter);
    if (colors != presenter->render_targets.size() || depths != presenter->depth_targets.size()) {
        std::fprintf(stderr,
            "recomp d3d presenter: released storage base=0x%08X size=%u color=%zu depth=%zu remaining=%zu/%zu\n",
            base, size, colors - presenter->render_targets.size(), depths - presenter->depth_targets.size(),
            presenter->render_targets.size(), presenter->depth_targets.size());
    }
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_destroy(RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const RecompD3dPresenterError owner = checkOwner(*presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    RecompD3dPresenter *destroyed = *presenter;
    active_presenter = nullptr;
    *presenter = nullptr;
    destroyed->precompile_stop = true;
    if (destroyed->precompile.joinable()) destroyed->precompile.join();
    releasePresenter(destroyed);
    delete destroyed;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return RECOMP_D3D_PRESENTER_OK;
}

void d3d_backend_set_immediate_present(bool enabled)
{
    immediate_present = enabled;
    RecompD3dPresenter *presenter = active_presenter;
    if (presenter == nullptr || presenter->window == nullptr) return;
    const SDL_GPUPresentMode mode = preferMailbox(enabled) &&
            SDL_WindowSupportsGPUPresentMode(presenter->device, presenter->window, SDL_GPU_PRESENTMODE_MAILBOX)
        ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
    SDL_SetGPUSwapchainParameters(presenter->device, presenter->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

D3dBackendVisibilityPoll d3d_backend_poll_visibility(RecompD3dPresenter *presenter, uint32_t *index, uint32_t *pixels)
{
    if (checkOwner(presenter) != RECOMP_D3D_PRESENTER_OK) return D3D_BACKEND_VISIBILITY_FAILED;
    if (presenter->visibility_pending.empty()) return D3D_BACKEND_VISIBILITY_IDLE;
    VisibilityQuery &front = presenter->visibility_pending.front();
    double sum = 0.0;
    if (front.fence != nullptr) {
        if (!SDL_QueryGPUFence(presenter->device, front.fence)) return D3D_BACKEND_VISIBILITY_PENDING;
        const auto *counts = static_cast<const float *>(
            SDL_MapGPUTransferBuffer(presenter->device, front.download, false));
        if (counts == nullptr) return D3D_BACKEND_VISIBILITY_FAILED;
        for (uint32_t i = 0u; i < front.level_width * front.level_height; ++i) sum += counts[i];
        SDL_UnmapGPUTransferBuffer(presenter->device, front.download);
        // The reduced level's mean, scaled to the counting target's area.
        sum *= static_cast<double>(front.width) * front.height / (front.level_width * front.level_height);
    }
    *index = front.index;
    // A resolved count is the per-pixel fraction of passing samples.
    *pixels = static_cast<uint32_t>(std::min(
        std::floor(sum / front.host_pixels_per_guest_pixel + 0.5), 4294967295.0));
    releaseQuery(presenter, front);
    presenter->visibility_pending.erase(presenter->visibility_pending.begin());
    return D3D_BACKEND_VISIBILITY_READY;
}

void d3d_backend_report_draw_textures()
{
    for (uint32_t format = 0u; format < 256u; ++format) {
        const uint32_t sampled = draw_texture_tally.textured[format];
        const uint32_t rejected = draw_texture_tally.rejected[format];
        if (sampled == 0u && rejected == 0u) continue;
        std::fprintf(stderr, "recomp d3d presenter: draw texture fmt=0x%02x sampled=%u rejected=%u\n",
            format, sampled, rejected);
    }
    std::fprintf(stderr, "recomp d3d presenter: draw texture untextured=%u cached=%u\n",
        draw_texture_tally.untextured, active_presenter != nullptr ? active_presenter->texture_count : 0u);
}
