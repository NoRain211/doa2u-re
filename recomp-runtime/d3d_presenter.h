#ifndef DOAXBV_RECOMP_D3D_PRESENTER_H
#define DOAXBV_RECOMP_D3D_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "d3d_render_state_model.h"
#include "d3d_texture_model.h"

typedef struct RecompD3dPresenter RecompD3dPresenter;

typedef enum RecompD3dPresenterColorFormat {
    RECOMP_D3D_PRESENTER_COLOR_FORMAT_UNKNOWN,
    /* Xbox linear A8R8G8B8 (0x12) uses BGRA8 host storage. Clear colors
       remain logical RGBA values at the presenter seam. */
    RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
} RecompD3dPresenterColorFormat;

typedef enum RecompD3dPresenterDepthFormat {
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_UNKNOWN,
    /* Xbox linear D24S8 (0x2e). */
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8,
} RecompD3dPresenterDepthFormat;

typedef struct RecompD3dPresenterConfig {
    uint32_t width;
    uint32_t height;
    RecompD3dPresenterColorFormat color_format;
    RecompD3dPresenterDepthFormat depth_format;
} RecompD3dPresenterConfig;

typedef enum RecompD3dPresenterCommandType {
    RECOMP_D3D_PRESENTER_COMMAND_CLEAR,
    RECOMP_D3D_PRESENTER_COMMAND_PRESENT,
    RECOMP_D3D_PRESENTER_COMMAND_DRAW,
    RECOMP_D3D_PRESENTER_COMMAND_GAMMA,
    /* Occlusion counting brackets later draws; END assigns the count to an index. */
    RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN,
    RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END,
    RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE,
    /* Copies the main color target into a main-sized offscreen target, so
       later draws sampling that texture see the frame (a CopyRects snapshot). */
    RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR,
} RecompD3dPresenterCommandType;

/* Resolved by the adapter. Surface wrappers sharing pixel storage have the
   same color descriptor; the backend never reads a guest device object. */
typedef struct RecompD3dPresenterTarget {
    bool offscreen;
    bool no_depth;
    bool custom_depth;
    RecompD3dTextureDesc color;
    /* Otherwise use the default host depth surface (unless no_depth). */
    RecompD3dTextureDesc depth;
} RecompD3dPresenterTarget;

typedef struct RecompD3dPresenterClearCommand {
    bool clear_color;
    bool clear_depth;
    bool clear_stencil;
    uint32_t color;
    float z;
    uint32_t stencil;
    RecompD3dPresenterTarget target;
} RecompD3dPresenterClearCommand;

typedef struct RecompD3dPresenterPresentCommand {
    uint32_t effective_flags;
    uint32_t swap_counter;
} RecompD3dPresenterPresentCommand;

/* Fixed-function vertex lighting: directional and point lights. DOA3 also
   fills specular and a stage-0 combine of texture and lit diffuse (stage_mode). */
typedef struct RecompD3dDirectionalLighting {
    bool enabled;
    bool normalize;
    bool stage_mode;
    uint32_t count;
    float normal_transforms[4][16];
    float ambient_emissive[4];
    float material_diffuse[4];
    float directions[8][4];
    float colors[8][4];
    float world_transforms[4][16];
    /* World position xyz; w is 1 for point lights, 0 for directional. */
    float positions[8][4];
    /* Constant, linear, quadratic attenuation, and range. */
    float attenuation[8][4];
    /* Point-light ambient multiplied by material ambient, before attenuation. */
    float ambient[8][4];
    /* Light specular times material specular. */
    float specular[8][4];
    /* World vertex-to-viewer direction; w is the power, 0 for no specular. */
    float view[4];
    /* Stage 0: RGB texture, RGB source, alpha texture, alpha source.
       Sources: 0 = one, 1 = diffuse, 2 = TFACTOR. Texture selectors are 0/1. */
    float stage[4];
} RecompD3dDirectionalLighting;

typedef enum RecompD3dCullMode {
    RECOMP_D3D_CULL_NONE,
    RECOMP_D3D_CULL_CLOCKWISE,
    RECOMP_D3D_CULL_COUNTER_CLOCKWISE,
} RecompD3dCullMode;

/* One indexed draw. Buffer contents stay in guest memory: the adapter passes
   host pointers and byte counts it has already bounds-checked, so the
   presenter never decodes guest addresses itself. */
typedef struct RecompD3dPresenterDrawCommand {
    RecompD3dCullMode cull_mode;
    /* D24 depth-buffer units and slope factor, resolved by the adapter. */
    int32_t depth_bias;
    float slope_scaled_depth_bias;
    /* Pixel width and alpha coverage for untextured XYZ|DIFFUSE line lists. */
    float line_width;
    bool line_antialias;
    uint32_t primitive_type;
    uint32_t index_count;
    uint32_t triangle_count;
    uint32_t vertex_count;
    uint32_t vertex_stride;
    uint32_t fvf;
    /* Bounded API-level vertex program; zero count selects fixed function. */
    uint32_t program_count;
    uint32_t program[136][4];
    float program_constants[192][4];
    bool program_alpha_mask;
    float program_mask_lod_bias;
    /* Capture copies all but enabled/normalize/stage_mode/count/stage only when enabled. */
    RecompD3dDirectionalLighting directional;
    const void *vertex_bytes;
    const void *index_bytes;
    /* World-view-projection rows, already composed by the adapter. */
    float transform[16];
    /* Guest viewport for transformed draws: X, Y, width, height in target
       pixels, then MinZ, MaxZ. Zero width keeps the whole target. */
    float viewport[6];
    /* Fixed-function raster displacement in logical guest pixels. */
    float screen_space_offset[2];
    /* One allowed rectangle with exclusive right/bottom guest target pixel edges. */
    bool has_scissor;
    int32_t scissor[4];
    /* Additional world-view-projection matrices for 1..3 explicit weights. */
    float blend_transforms[3][16];
    uint32_t blend_weight_count;
    bool has_transform;
    /* Depth, stencil, and alpha-test state for this draw, already decoded by
       the render-state model so the presenter never sees a method number. */
    RecompD3dDepthState depth;
    RecompD3dBlendState blend;
    /* Stage 0 selects this ARGB factor for both color and alpha. */
    bool use_texture_factor;
    /* Stage 1 multiplies the sampled result by this factor. */
    bool modulate_texture_factor;
    /* Stage 0 color is saturate(scale * texture * factor), scale 2 or 4; zero disables. */
    uint32_t texture_factor_color_scale;
    uint32_t texture_factor;
    /* Measured stage-0 selection or modulation of material alpha. */
    RecompD3dMaterialAlphaMode material_alpha_mode;
    float material_alpha;
    /* ponytail: exact zero illumination; full lighting needs light evaluation. */
    bool zero_diffuse_rgb;
    /* The sampled texel contributes alpha only, as for SELECTARG1(DIFFUSE) color. */
    bool texture_alpha_only;
    /* Four-source register-combiner filter over all four UV sets:
       sum_scale * (sat(scale * (f0*t0 + f1*t1)) + sat(scale * (f2*t2 + f3*t3))),
       with packed ARGB factors. */
    bool four_tap_filter;
    uint32_t four_tap_factors[4];
    float four_tap_scale, four_tap_sum_scale;
    /* Exact two-stage bright pass: saturate(2 * (gaussian3x3(t0) - c0) - 1).
       The threshold is stage-0 C0 in packed ARGB; UV0 is the only coordinate. */
    bool bright_pass;
    uint32_t bright_pass_threshold;
    /* Two-stage tint: l = dot(t0.rgb, c0.rgb), output saturate(c1 * l) for RGB
       and alpha. Factors are stage-0 C0 and stage-1 C0 in packed ARGB. */
    bool luminance_tint;
    uint32_t luminance_tint_factors[2];
    /* Stage 0 texture for this draw. `texture_bytes` is a host-readable view
       of guest pixel memory, valid only for the duration of the submit. */
    RecompD3dTextureDesc texture;
    bool has_texture;
    /* Guest D3DTADDRESS U/V/W for the texture's stage; 0 (unknown) wraps. */
    uint32_t address_u, address_v, address_w;
    /* Stage-0 mip selection; zero initialization keeps linear mips and no bias. */
    bool point_mip_filter;
    float mip_lod_bias;
    /* Stage-0 D3DTSS_ALPHAKILL: discard fragments whose texel alpha is zero. */
    bool alpha_kill;
    /* Stage-0 TCI_CAMERASPACEREFLECTIONVECTOR with COUNT2: UV0 is generated from
       reflection_world_view/normal and reflection_transform (TEXTURE0). */
    bool texgen_reflection;
    /* Stage-0 COUNT2 over mesh UV0: UV0 becomes (u, v, 1, 0) times
       reflection_transform (TEXTURE0), so the third row translates. */
    bool texture_transform_uv;
    /* FVF 0x404 dependent read: n = signed reflection_texture texel at UV0,
       then the texture is sampled at (dot((uv1, uv2.x), n), dot((uv2.y, uv3), n)). */
    bool distortion;
    /* Stage-effect bump pass (pixel shader built by FUN_0027ffc0): n is the
       signed V8U8 alpha_mask texel at oT0 / q, t2 = texture at oT2.xy + M2 n,
       t3 = reflection_texture at oT3.xy + M3 n and r0 = t2 (1 - t3.a^4) oD0.
       Output is lerp(fog_color, r0, f) with oD0 alpha, where f is
       saturate(bump_fog[0] + bump_fog[1] * oFog) when bump_fog[2] is set and
       1 otherwise. bump_matrices hold BUMPENVMAT00, 01, 10, 11 of stages 2-3. */
    bool bump_env;
    /* Water variant (FUN_0026bd20): texture is t1 at oT1.xy + M1 n and
       reflection_texture t3 at oT3.xy + M3 n; bump_matrices hold stages 1
       and 3. s = saturate(dot(2 oD0 - 1, 2 t3 - 1))^8, r0 = clamp(s + oD1 t1.a
       - 1) then clamp(r0 + t3.a t1), alpha t3.a oD0.a, with the same fog. */
    bool bump_water;
    float bump_matrices[2][4];
    float bump_fog[3];
    uint32_t fog_color;
    uint32_t bump_address_u, bump_address_v;
    /* Stage-0 MODULATE(TEXTURE, TFACTOR) color over white vertices. */
    bool texture_factor_modulate;
    /* Stage-0 TCI_CAMERASPACEPOSITION with COUNT3|PROJECTED: (s, t, q) =
       (world-view position, 1) times reflection_transform columns 0-2, with
       the sample at (s, t) / q per pixel. COUNT2 uses the same path with Q=1. */
    bool texgen_projected;
    /* Stages 1-3 ADD(TEXTURE, CURRENT) under texgen_projected (0x257ee0). Each of
       the first projected_stage_count stages samples slot projected_slots[i] (0
       texture, 1 reflection_texture, 2 reflection_stage2_texture, each with its
       slot's sampler; -1 passes CURRENT) at (s, t) / q, where (s, t, q) is the
       world-view position times projected_transforms[i] (TEXTURE1-3) columns 0-2.
       Each sum clamps to 1. */
    uint32_t projected_stage_count;
    int32_t projected_slots[3];
    float projected_transforms[3][16];
    /* A later stage's MODULATE(TFACTOR, CURRENT) on alpha alone. */
    bool modulate_texture_factor_alpha;
    /* Stage 1 MODULATE(TEXTURE, CURRENT) alpha: reflection_texture at UV1
       multiplies alpha; with ALPHAKILL a zero texel alpha discards. */
    bool stage1_alpha_mask;
    bool stage1_alpha_kill;
    /* The stage-1 texture only applies its ALPHAKILL, at UV0; alpha is not multiplied. */
    bool stage1_kill_only;
    /* Storage aliases the current guest backbuffer, whose pixels are host-owned. */
    bool texture_is_backbuffer;
    /* GetBackBuffer(-1): the frame Present last showed, not the one in progress. */
    bool texture_is_front_buffer;
    const void *texture_bytes;
    uint32_t texture_byte_count;
    /* Bound P8 palette in guest ARGB32 order, valid during the submit. */
    const void *palette_bytes;
    uint32_t palette_byte_count;
    /* Separate stage-0 alpha mask; stage-1 color uses the second UV set. */
    bool has_alpha_mask;
    RecompD3dTextureDesc alpha_mask;
    const void *alpha_mask_bytes;
    uint32_t alpha_mask_byte_count;
    const void *alpha_mask_palette;
    uint32_t alpha_mask_palette_byte_count;
    /* Fixed-function texture-alpha reflection blend, followed by diffuse modulation. */
    bool has_reflection;
    bool reflection_normalize;
    bool reflection_mesh_uv;
    bool reflection_premultiplied;
    /* Stage-1 alpha is SELECTARG1(CURRENT): the blend keeps stage-0 alpha. */
    bool reflection_color_only;
    /* Discard a zero-alpha stage-1 texel before its reflection blend. */
    bool reflection_alpha_kill;
    /* Stage 2 MODULATE(TEXTURE, CURRENT) without a stage-2 texture: the 5849
       compiler (0x352e46) passes CURRENT through, so no final diffuse. */
    bool reflection_skip_diffuse;
    RecompD3dTextureDesc reflection_texture;
    /* Stage-1 D3DTADDRESS U/V for reflection_texture; 0 (unknown) wraps. */
    uint32_t reflection_address_u, reflection_address_v;
    /* reflection_texture's own stage mip filter and LOD bias. */
    bool reflection_point_mip_filter;
    float reflection_mip_lod_bias;
    const void *reflection_bytes;
    uint32_t reflection_byte_count;
    /* Capture copies through reflection_diffuse only with has_reflection,
       texgen_reflection, texture_transform_uv or texgen_projected. */
    float reflection_world_view[16];
    float reflection_normal[16];
    float reflection_blend_world_view[3][16];
    float reflection_blend_normal[3][16];
    float reflection_transform[16];
    /* TEXTURE1 for stage-1 generated coordinates when stage 0 also generates
       (texgen_reflection with has_reflection and !reflection_mesh_uv). */
    float reflection_stage1_transform[16];
    float reflection_diffuse[4];
    /* Second BLENDTEXTUREALPHA at stage 2, before stage-3 diffuse modulation.
       Coordinates use the shared reflection vector and TEXTURE2, or mesh UV0. */
    bool reflection_stage2;
    bool reflection_stage2_mesh_uv;
    RecompD3dTextureDesc reflection_stage2_texture;
    const void *reflection_stage2_bytes;
    uint32_t reflection_stage2_byte_count;
    uint32_t reflection_stage2_address_u, reflection_stage2_address_v;
    bool reflection_stage2_point_mip_filter;
    float reflection_stage2_mip_lod_bias;
    float reflection_stage2_transform[16];
    RecompD3dPresenterTarget target;
} RecompD3dPresenterDrawCommand;

typedef struct RecompD3dPresenterCommand {
    RecompD3dPresenterCommandType type;
    union {
        RecompD3dPresenterClearCommand clear;
        RecompD3dPresenterPresentCommand present;
        RecompD3dPresenterDrawCommand draw;
        /* Xbox gamma ramp: separate 256-entry 8-bit red, green, blue planes. */
        uint8_t gamma[3][256];
        uint32_t visibility_index;
        RecompD3dTextureDesc copy_destination;
    } data;
} RecompD3dPresenterCommand;

typedef enum RecompD3dPresenterError {
    RECOMP_D3D_PRESENTER_OK,
    RECOMP_D3D_PRESENTER_INVALID_ARGUMENT,
    RECOMP_D3D_PRESENTER_NOT_INITIALIZED,
    RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED,
    RECOMP_D3D_PRESENTER_OUT_OF_MEMORY,
    RECOMP_D3D_PRESENTER_HOST_FAILURE,
    RECOMP_D3D_PRESENTER_WRONG_THREAD,
    RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND,
    RECOMP_D3D_PRESENTER_COMMAND_LIMIT,
    RECOMP_D3D_PRESENTER_CLOSED,
} RecompD3dPresenterError;

/* Lifecycle calls and submissions occur on one owning thread. Create requires
   a null output handle, and destroy releases all adapter state and nulls the
   handle. The D3D11 build copies each submit and renders it on a worker thread
   at the next PRESENT. A failed draw is counted as a decline, as in the
   synchronous path; other errors surface on a later call or on destroy. */
#ifdef __cplusplus
extern "C" {
#endif

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config,
    RecompD3dPresenter **presenter);
RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command);
RecompD3dPresenterError recomp_d3d_presenter_destroy(
    RecompD3dPresenter **presenter);
/* Publish queued work without swapping; optionally wait for its completion. */
RecompD3dPresenterError recomp_d3d_presenter_flush(
    RecompD3dPresenter *presenter, bool wait);

/* Synchronous, write-free raster coverage of fixed-function triangles. Ignores
   shading and depth/stencil rejection; zero means no fragment can consume them. */
RecompD3dPresenterError recomp_d3d_presenter_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered);

/* Result of the latest VISIBILITY_END submitted for index: samples that passed
   depth/stencil, in pixels of the bound target's guest size. complete stays
   false while any END for index is unresolved. Never waits; flush publishes a
   queued END. INVALID_ARGUMENT when no END for index was submitted. */
RecompD3dPresenterError recomp_d3d_presenter_visibility_result(
    RecompD3dPresenter *presenter, uint32_t index, bool *complete, uint32_t *pixels);

/* Retire pixels only when their backing allocation is released, not when a
   nonowning surface wrapper is destroyed. Accepts guest RAM aliases. */
RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size);

/* Drop cached CPU texture uploads in a range but keep rendered color and depth
   targets: re-registering a header over rendered memory leaves its pixels. */
RecompD3dPresenterError recomp_d3d_presenter_invalidate_textures(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size);

/* Process-wide host pacing toggle. When enabled, presents use
   DXGI_PRESENT_DO_NOT_WAIT instead of vsync so the guest frame loop runs at
   CPU speed. The guest does not depend on wall-clock time (kernel waits are
   already immediate), so this only removes unintended host pacing; it does
   not force guest state. Intended for the full-program runner; the default
   (vsync) remains for windowed tests. */
void recomp_d3d_presenter_set_immediate_present(bool enabled);

/* Observation only: reports which guest texture formats draws actually
   sampled, and which ones a draw asked for but the presenter could not
   upload. Bind counts alone cannot answer that. */
void recomp_d3d_presenter_report_draw_textures(void);

#ifdef __cplusplus
}
#endif

#endif
