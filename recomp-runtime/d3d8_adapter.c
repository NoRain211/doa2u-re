/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d_card_model.h"
#include "d3d_device_model.h"
#include "d3d_vertex_shader_model.h"
#include "d3d_frame_model.h"
#include "d3d_tile_model.h"
#include "d3d_vblank.h"
#include "kernel_abi.h"
#include "stop_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Verified against the selected D3D8 5849 library. No sister-title bindings. */
enum {
    DEVICE_GLOBAL = 0x003554f8u,
    DEVICE = 0x00355500u,
    DEVICE_SIZE = 0x24a0u,
    TARGET = 0x1a04u,
    DEPTH = 0x1a08u,
    BUFFER_COUNT = 0x1a10u,
    BUFFER_ARRAY = 0x1a14u,
    DEFAULT_DEPTH = 0x1a20u,
    COLOR_HEADERS = 0x1a24u,
    DEPTH_HEADER = 0x1a6cu,
    VIEWPORT = 0x0ee0u,
    FRAME_COUNTER = 0x2478u,
};

static RecompD3dDeviceState device_state;
static RecompD3dFrameState frame_state;
static RecompD3dPresenter *presenter;
void (*recomp_d3d_present_hook)(uint32_t swap_counter);
static uint32_t submitted_draws;
/* The host keeps one main color target across Swap and Reset (which clears
   only depth). It still holds the frame Present last showed, and the pixels
   PersistDisplay saved, until a color clear or draw writes it. */
static bool back_buffer_displayed, back_buffer_saved;
/* Reset's mode set (0x34d58d) raises device+0x1de0; the next Swap (0x34af40)
   programs the mode and frees the persisted front-buffer copy (0x34a600). */
static bool display_mode_pending;
/* Device teardown (0x34ca1f) frees 64 report pages of 256 visibility slots. */
enum { VISIBILITY_TESTS = 64u * 256u, D3DERR_TESTINCOMPLETE = 0x88760828u };
static bool visibility_open;
/* Guest samples per counted host pixel for each ended index; zero until ended. */
static uint32_t visibility_scale[VISIBILITY_TESTS];

static uint32_t argument(uint32_t entry, uint32_t index)
{
    return *recomp_memory_u32(entry + 4u + index * 4u);
}

static void load(void *destination, uint32_t address, size_t size)
{
    memcpy(destination, recomp_memory(address, size), size);
}

static void store(uint32_t address, const void *source, size_t size)
{
    memcpy(recomp_memory(address, size), source, size);
}

uint32_t recomp_d3d8_require_device(void)
{
    if (!device_state.created || *recomp_memory_u32(DEVICE_GLOBAL) != DEVICE) {
        recomp_stop(2, "d3d8:device-not-created");
    }
    return DEVICE;
}

void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    if ((command->type == RECOMP_D3D_PRESENTER_COMMAND_CLEAR && command->data.clear.clear_color &&
         !command->data.clear.target.offscreen) ||
        (command->type == RECOMP_D3D_PRESENTER_COMMAND_DRAW && !command->data.draw.target.offscreen)) {
        back_buffer_displayed = back_buffer_saved = false;
    }
    const RecompD3dPresenterError error = recomp_d3d_presenter_submit(presenter, command);
    if (error == RECOMP_D3D_PRESENTER_CLOSED) recomp_stop(0, "host-window-closed");
    if (error != RECOMP_D3D_PRESENTER_OK) {
        recomp_stop(2, "d3d8:presenter:%u", (unsigned)error);
    }
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_DRAW && ++submitted_draws <= 4u) {
        fprintf(stderr, "recomp d3d8: Draw count=%u fvf=%08x vertices=%u texture=%u format=%02x\n",
            submitted_draws, command->data.draw.fvf, command->data.draw.vertex_count,
            command->data.draw.has_texture, command->data.draw.texture.format_byte);
    }
}

bool recomp_d3d8_draw_has_coverage(const RecompD3dPresenterDrawCommand *draw)
{
    bool covered = true;
    const RecompD3dPresenterError error = recomp_d3d_presenter_draw_coverage(presenter, draw, &covered);
    if (error == RECOMP_D3D_PRESENTER_CLOSED) recomp_stop(0, "host-window-closed");
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:coverage:%u", (unsigned)error);
    return covered;
}

void recomp_d3d8_release_memory(uint32_t base, uint32_t size)
{
    if (presenter == NULL) return;
    const RecompD3dPresenterError error = recomp_d3d_presenter_release_memory(presenter, base, size);
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:release-memory:%u", (unsigned)error);
}

void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size)
{
    if (presenter == NULL) return;
    const RecompD3dPresenterError error = recomp_d3d_presenter_invalidate_textures(presenter, base, size);
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:invalidate-textures:%u", (unsigned)error);
}

void recomp_d3d8_shutdown(void)
{
    if (presenter != NULL) {
        recomp_d3d_presenter_report_draw_textures();
        const RecompD3dPresenterError error = recomp_d3d_presenter_destroy(&presenter);
        fprintf(stderr, "recomp d3d8: presenter shutdown=%u\n", (unsigned)error);
    }
    recomp_d3d8_resource_reset();
    recomp_d3d_device_reset(&device_state);
    recomp_d3d_frame_reset(&frame_state);
    recomp_d3d8_state_reset();
    submitted_draws = 0u;
    back_buffer_displayed = back_buffer_saved = false;
    display_mode_pending = false;
    visibility_open = false;
    memset(visibility_scale, 0, sizeof visibility_scale);
}

static uint32_t color_header(uint32_t index)
{
    return DEVICE + COLOR_HEADERS + index * sizeof(RecompD3dSurfaceHeader);
}

static void target_surface(uint32_t surface, bool depth, RecompD3dTextureDesc *desc)
{
    if (!recomp_d3d8_resource_describe(surface, desc) || desc->depth != depth ||
        (!depth && !desc->render_target) || desc->bits_per_pixel != 32u || desc->data == 0u ||
        desc->width == 0u || desc->height == 0u || desc->width > 16384u || desc->height > 16384u ||
        (depth ? (desc->format_byte != 0x2eu && desc->format_byte != 0x2au) :
            (desc->format_byte != 0x12u && desc->format_byte != 0x06u)) ||
        (desc->linear && desc->pitch < desc->width * 4u)) {
        recomp_stop(2, "d3d8:target-surface:%08x", surface);
    }
    const uint64_t bytes = desc->linear ? (uint64_t)desc->pitch * desc->height :
        (uint64_t)desc->width * desc->height * 4u;
    if (bytes > RECOMP_XBOX_RAM_SIZE) recomp_stop(2, "d3d8:target-span");
    (void)recomp_memory(desc->data, (size_t)bytes);
}

static void resolve_target(uint32_t color, uint32_t depth, RecompD3dPresenterTarget *target)
{
    *target = (RecompD3dPresenterTarget){0};
    target_surface(color, false, &target->color);
    RecompD3dTextureDesc main_color, front_color;
    target_surface(color_header(0u), false, &main_color);
    target_surface(color_header(1u), false, &front_color);
    if (target->color.data == front_color.data) recomp_stop(2, "d3d8:target-front-buffer-unbound");
    target->offscreen = target->color.data != main_color.data;
    if (!target->offscreen && (target->color.width != main_color.width ||
        target->color.height != main_color.height || target->color.pitch != main_color.pitch ||
        target->color.linear != main_color.linear || target->color.format_byte != main_color.format_byte)) {
        recomp_stop(2, "d3d8:target-alias-unbound");
    }
    target->no_depth = depth == 0u;
    if (depth != 0u) {
        target_surface(depth, true, &target->depth);
        if (target->depth.width != target->color.width || target->depth.height != target->color.height)
            recomp_stop(2, "d3d8:target-depth-size-unbound");
        RecompD3dTextureDesc main_depth;
        target_surface(DEVICE + DEPTH_HEADER, true, &main_depth);
        target->custom_depth = target->depth.data != main_depth.data;
        /* SetRenderTarget (0x3447d0) addresses depth only through its own header, so an
           offscreen color may test against the main depth, as the bloom source pass
           (0x2756f0) does. shortcut: a supersampled main depth has physical units the
           host depth lacks; bind a scaled offscreen target if a caller needs one. */
        if (!target->custom_depth && ((target->offscreen &&
                (device_state.scale[0] != 1u || device_state.scale[1] != 1u)) ||
            target->depth.width != main_depth.width ||
            target->depth.height != main_depth.height || target->depth.pitch != main_depth.pitch ||
            target->depth.linear != main_depth.linear || target->depth.format_byte != main_depth.format_byte))
            recomp_stop(2, "d3d8:target-depth-alias-unbound");
    }
    if (!target->offscreen) {
        /* The host renders a supersampled back buffer at its logical (front
           buffer) size; guest viewports and scissors use the same units. */
        target->color.width /= device_state.scale[0];
        target->color.height /= device_state.scale[1];
        target->depth.width /= device_state.scale[0];
        target->depth.height /= device_state.scale[1];
    }
}

void recomp_d3d8_target(RecompD3dPresenterTarget *target)
{
    recomp_d3d8_require_device();
    resolve_target(*recomp_memory_u32(DEVICE + TARGET), *recomp_memory_u32(DEVICE + DEPTH), target);
}

/* D3D_CommonSetMultiSampleModeAndScale (0x347130): only the back buffer
   carries the supersample scale; the extent is in physical pixels. */
static void write_target_scale(bool main, uint32_t width, uint32_t height)
{
    const uint32_t x = main ? device_state.scale[0] : 1u, y = main ? device_state.scale[1] : 1u;
    const float scale[2] = {(float)x, (float)y};
    *recomp_memory_u32(DEVICE + 0x954u) = width * x;
    *recomp_memory_u32(DEVICE + 0x958u) = height * y;
    store(DEVICE + 0x95cu, scale, sizeof scale);
}

/* CDevice_InitializeFrameBuffers (0x34be80) mode fields. Flag 0x4000 selects
   Swap's filtered copy from the supersampled back buffer to the front buffer. */
static void write_frame_buffer_mode(const RecompD3dPresentationParameters *presentation)
{
    const float scale[2] = {(float)device_state.scale[0], (float)device_state.scale[1]};
    const bool supersampled = device_state.scale[0] != 1u || device_state.scale[1] != 1u;
    store(DEVICE + 0x96cu, scale, sizeof scale);
    *recomp_memory_u32(DEVICE + 0x196cu) = presentation->multi_sample_type;
    *recomp_memory_u32(DEVICE + 0x1970u) = presentation->swap_effect;
    *recomp_memory_u32(DEVICE + 0x8u) =
        (*recomp_memory_u32(DEVICE + 0x8u) & ~0x4000u) | (supersampled ? 0x4000u : 0u);
    write_target_scale(true, device_state.width, device_state.height);
}

static void write_initial_device(const RecompD3dPresentationParameters *presentation)
{
    recomp_d3d8_state_reset();
    memset(recomp_memory(DEVICE, DEVICE_SIZE), 0, DEVICE_SIZE);
    for (uint32_t i = 0; i < 2u; ++i) {
        store(color_header(i), &device_state.color[i], sizeof device_state.color[i]);
        *recomp_memory_u32(DEVICE + BUFFER_ARRAY + i * 4u) = color_header(i);
    }
    store(DEVICE + DEPTH_HEADER, &device_state.depth, sizeof device_state.depth);
    *recomp_memory_u32(DEVICE + TARGET) = color_header(0u);
    *recomp_memory_u32(DEVICE + DEPTH) = DEVICE + DEPTH_HEADER;
    *recomp_memory_u32(DEVICE + DEFAULT_DEPTH) = DEVICE + DEPTH_HEADER;
    *recomp_memory_u32(DEVICE + BUFFER_COUNT) = 2u;
    store(DEVICE + VIEWPORT, &device_state.viewport, sizeof device_state.viewport);
    *recomp_memory_u32(DEVICE + 0xef8u) = *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f080000u;
    for (uint32_t slot = 0u; slot < 10u; ++slot) {
        for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal) {
            *recomp_memory_u32(DEVICE + 0xc60u + slot * 64u + diagonal * 20u) = 0x3f800000u;
        }
    }
    *recomp_memory_u32(DEVICE + 0x938u) = 1u;
    *recomp_memory_u32(DEVICE + 0x940u) = 0x3f800000u;
    *recomp_memory_u32(DEVICE + 0x944u) = 0x3f800000u;
    *recomp_memory_u32(DEVICE + 0x8u) = 2u;
    write_frame_buffer_mode(presentation);
    for (uint32_t bank = 0u; bank < 2u; ++bank) {
        uint8_t *ramp = recomp_memory(DEVICE + 0x1e04u + bank * 0x300u, 0x300u);
        for (uint32_t plane = 0u; plane < 3u; ++plane) {
            for (uint32_t i = 0u; i < 256u; ++i) ramp[plane * 256u + i] = (uint8_t)i;
        }
    }
    *recomp_memory_u32(DEVICE + 0x2404u) = 1u;
    /* SDK creation selects FVF 4 then FVF 2 through the public setter; replay
       only the resulting shadows so later binds never read page zero. */
    uint32_t *declaration = (uint32_t *)(void *)recomp_memory(
        RECOMP_D3D_DEFAULT_DECLARATION, RECOMP_D3D_VERTEX_DECLARATION_SIZE);
    if (!recomp_d3d_build_fixed_function_declaration(2u, declaration)) {
        recomp_stop(2, "d3d8:default-declaration");
    }
    *recomp_memory_u32(DEVICE + 0x794u) = RECOMP_D3D_DEFAULT_DECLARATION;
    *recomp_memory_u32(DEVICE + 0x798u) = 2u;
    *recomp_memory_u32(DEVICE + 0x79cu) = 0u;
    *recomp_memory_u32(0x00355058u) |= 0x70u;
    recomp_d3d8_state_write_defaults(*recomp_memory_u32(DEVICE + DEFAULT_DEPTH) != 0u);
}

static void validate_storage(const RecompD3dSurfaceHeader *surface)
{
    const uint32_t pitch = ((surface->size >> 24u) + 1u) * 64u;
    const uint32_t height = ((surface->size >> 12u) & 0xfffu) + 1u;
    (void)recomp_memory(surface->data, (size_t)pitch * height);
}

static void create_device(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t parameters = argument(entry, 4u);
    const uint32_t output = argument(entry, 5u);
    RecompD3dCreateRequest request = {
        .adapter = argument(entry, 0u), .device_type = argument(entry, 1u),
        .focus_window = argument(entry, 2u), .behavior_flags = argument(entry, 3u),
    };
    RecompD3dDeviceState candidate = {0};
    RecompD3dSurfaceHeader colors[2], depth;
    uint32_t result = RECOMP_D3D_INVALID_CALL;

    if (output != 0u) *recomp_memory_u32(output) = 0u;
    if (parameters == 0u || device_state.created) goto returned;
    load(&request.presentation, parameters, sizeof request.presentation);
    if (!recomp_d3d_create_request_supported(&request)) {
        const uint32_t *words = (const uint32_t *)(const void *)&request.presentation;
        for (uint32_t i = 0; i < sizeof request.presentation / 4u; ++i) {
            fprintf(stderr, "recomp d3d8: presentation[%u]=%08" PRIx32 "\n", i, words[i]);
        }
        recomp_stop(2, "d3d8:create-device-request");
    }
    for (uint32_t i = 0; i < 2u; ++i) {
        load(&colors[i], request.presentation.buffer_surfaces[i], sizeof colors[i]);
    }
    load(&depth, request.presentation.depth_stencil_surface, sizeof depth);
    const RecompD3dDeviceError error = recomp_d3d_device_create(&candidate, &request, colors, &depth);
    if (error != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:create-device-model:%u", (unsigned)error);
    for (uint32_t i = 0; i < 2u; ++i) validate_storage(&colors[i]);
    validate_storage(&depth);
    const RecompD3dPresenterError host_error = recomp_d3d_presenter_create(&candidate.presenter_config, &presenter);
    if (host_error != RECOMP_D3D_PRESENTER_OK) {
        result = host_error == RECOMP_D3D_PRESENTER_OUT_OF_MEMORY ? RECOMP_D3D_OUT_OF_MEMORY : RECOMP_D3D_FAIL;
        goto returned;
    }
    device_state = candidate;
    write_initial_device(&request.presentation);
    if (recomp_d3d_frame_initialize(&frame_state, candidate.width, candidate.height) != RECOMP_D3D_FRAME_OK) {
        recomp_stop(2, "d3d8:frame-initialize");
    }
    recomp_d3d_vblank_reset();
    *recomp_memory_u32(DEVICE_GLOBAL) = DEVICE;
    if (output != 0u) *recomp_memory_u32(output) = DEVICE;
    result = RECOMP_D3D_OK;
    /* The API owns initialization of the native depth/stencil target too. */
    RecompD3dFrameResult clear = recomp_d3d_frame_clear(&frame_state, 0u, 0u,
        RECOMP_D3D_CLEAR_DEPTH | RECOMP_D3D_CLEAR_STENCIL, 0u, 0x3f800000u, 0u);
    recomp_d3d8_submit(&clear.command);
returned:
    fprintf(stderr, "recomp d3d8: CreateDevice result=%08" PRIx32 " device=%08" PRIx32 "\n",
        result, result == RECOMP_D3D_OK ? DEVICE : 0u);
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp = entry + 28u;
}

static uint32_t reference_surface(uint32_t address, bool release)
{
    RecompD3dSurfaceHeader surface;
    uint32_t count = 0u;
    bool destroy = false;
    RecompD3dDeviceError error;
    if (address == 0u) recomp_stop(2, "d3d8:resource-null");
    load(&surface, address, sizeof surface);
    error = release ? recomp_d3d_surface_release(&surface, &count, &destroy)
                    : recomp_d3d_surface_add_ref(&surface, &count);
    if (error != RECOMP_D3D_DEVICE_OK || destroy) {
        recomp_stop(2, "d3d8:resource-reference:%u:%u", (unsigned)error, (unsigned)destroy);
    }
    store(address, &surface, sizeof surface);
    return count;
}

static void free_saved_display(void)
{
    const uint32_t previous = recomp_kernel_av_get_saved_data_address();
    if (previous != 0u) {
        recomp_kernel_free_pool(previous);
        recomp_kernel_av_set_saved_data_address(0u);
    }
}

static void add_ref(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = reference_surface(argument(entry, 0u), false);
    recomp_runtime.registers.esp = entry + 8u;
}

static void release(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_runtime.registers.eax = reference_surface(argument(entry, 0u), true);
    recomp_runtime.registers.esp = entry + 8u;
}

static void get_back_buffer(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t index = argument(entry, 0u);
    recomp_d3d8_require_device();
    if (index != 0u && index != UINT32_MAX) recomp_stop(2, "d3d8:back-buffer-index:%u", index);
    const uint32_t address = color_header(index == UINT32_MAX ? 1u : 0u);
    recomp_d3d8_resource_add_ref(address);
    recomp_runtime.registers.eax = address;
    recomp_runtime.registers.esp = entry + 8u;
}

static void get_depth_surface(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_d3d8_require_device();
    const uint32_t address = *recomp_memory_u32(DEVICE + DEPTH);
    if (address != 0u) recomp_d3d8_resource_add_ref(address);
    recomp_runtime.registers.eax = address;
    recomp_runtime.registers.esp = entry + 4u;
}

static void get_render_target(void)
{
    recomp_d3d8_require_device();
    const uint32_t address = *recomp_memory_u32(DEVICE + TARGET);
    if (address != 0u) recomp_d3d8_resource_add_ref(address);
    recomp_runtime.registers.eax = address;
    recomp_runtime.registers.esp += 4u;
}

static void get_viewport(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t output = argument(entry, 0u);
    recomp_d3d8_require_device();
    if (output == 0u) recomp_stop(2, "d3d8:viewport-null");
    load(&device_state.viewport, DEVICE + VIEWPORT, sizeof device_state.viewport);
    store(output, &device_state.viewport, sizeof device_state.viewport);
    recomp_runtime.registers.esp = entry + 8u;
}

static void set_viewport(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t source = argument(entry, 0u);
    RecompD3dViewport viewport;
    recomp_d3d8_require_device();
    if (source != 0u) {
        RecompD3dPresenterTarget target;
        recomp_d3d8_target(&target);
        RecompD3dDeviceState candidate = device_state;
        candidate.width = target.color.width;
        candidate.height = target.color.height;
        load(&viewport, source, sizeof viewport);
        const RecompD3dDeviceError error = recomp_d3d_device_set_viewport(&candidate, &viewport);
        if (error != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:viewport:%u", (unsigned)error);
        device_state.viewport = candidate.viewport;
        store(DEVICE + VIEWPORT, &device_state.viewport, sizeof device_state.viewport);
    }
    *recomp_memory_u32(DEVICE + 0x1c00u) = *recomp_memory_u32(DEVICE + 0x1c04u) = 0u;
    recomp_runtime.registers.esp = entry + 8u;
}

static void set_tile(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t index = argument(entry, 0u), source = argument(entry, 1u);
    uint32_t destination;
    RecompD3dTile tile, normalized;
    recomp_d3d8_require_device();
    if (!recomp_d3d_tile_entry_address(DEVICE, index, &destination)) recomp_stop(2, "d3d8:tile-index:%u", index);
    if (source != 0u) load(&tile, source, sizeof tile);
    recomp_d3d_set_tile(&normalized, source == 0u ? NULL : &tile);
    store(destination, &normalized, sizeof normalized);
    recomp_runtime.registers.esp = entry + 12u;
}

static void set_render_target(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    uint32_t color = argument(entry, 0u), depth = argument(entry, 1u);
    recomp_d3d8_require_device();
    if (color == 0u) color = *recomp_memory_u32(DEVICE + TARGET);
    RecompD3dPresenterTarget target;
    resolve_target(color, depth, &target);
    recomp_d3d8_resource_set_render_targets(color, depth);
    if (depth != 0u) {
        *recomp_memory_u32(DEVICE + 0x948u) = 0x4b7fffffu;
        *recomp_memory_u32(DEVICE + 0x94cu) = target.depth.linear ? target.depth.pitch : target.depth.width * 4u;
    }
    write_target_scale(!target.offscreen, target.color.width, target.color.height);
    device_state.viewport = (RecompD3dViewport){0u, 0u, target.color.width, target.color.height, 0.0f, 1.0f};
    store(DEVICE + VIEWPORT, &device_state.viewport, sizeof device_state.viewport);
    *recomp_memory_u32(DEVICE + 0x1c00u) = *recomp_memory_u32(DEVICE + 0x1c04u) = 0u;
    recomp_runtime.registers.esp = entry + 12u;
}

static void clear(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_d3d8_require_device();
    RecompD3dPresenterTarget target;
    recomp_d3d8_target(&target);
    load(&device_state.viewport, DEVICE + VIEWPORT, sizeof device_state.viewport);
    if (device_state.viewport.x != 0u || device_state.viewport.y != 0u ||
        device_state.viewport.width != target.color.width || device_state.viewport.height != target.color.height) {
        recomp_stop(2, "d3d8:clear-viewport-unbound");
    }
    RecompD3dFrameResult result = recomp_d3d_frame_clear(&frame_state,
        argument(entry, 0u), argument(entry, 1u), argument(entry, 2u),
        argument(entry, 3u), argument(entry, 4u), argument(entry, 5u));
    if (result.error != RECOMP_D3D_FRAME_OK) recomp_stop(2, "d3d8:clear:%u", (unsigned)result.error);
    result.command.data.clear.target = target;
    if (target.no_depth) {
        result.command.data.clear.clear_depth = false;
        result.command.data.clear.clear_stencil = false;
    }
    recomp_d3d8_submit(&result.command);
    fprintf(stderr, "recomp d3d8: Clear flags=%08" PRIx32 "\n", argument(entry, 2u));
    recomp_runtime.registers.esp = entry + 28u;
}

static void swap(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    recomp_d3d8_require_device();
    const uint32_t completed_bank = frame_state.swap_counter & 1u;
    const RecompD3dFrameResult result = recomp_d3d_frame_swap(&frame_state, argument(entry, 0u));
    if (result.error != RECOMP_D3D_FRAME_OK) recomp_stop(2, "d3d8:swap:%u", (unsigned)result.error);
    for (uint32_t i = 0; i < 2u; ++i) load(&device_state.color[i], color_header(i), sizeof device_state.color[i]);
    if (recomp_d3d_device_swap_buffers(&device_state) != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:swap-headers");
    recomp_d3d_wait_present();
    recomp_d3d8_submit(&result.command);
    back_buffer_displayed = true;
    back_buffer_saved = false;
    if (display_mode_pending) {
        free_saved_display();
        display_mode_pending = false;
    }
    *recomp_memory_u32(DEVICE + 0x2404u + completed_bank * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x1de4u) = result.command.data.present.swap_counter;
    for (uint32_t i = 0; i < 2u; ++i) store(color_header(i), &device_state.color[i], sizeof device_state.color[i]);
    *recomp_memory_u32(DEVICE + FRAME_COUNTER) = result.command.data.present.swap_counter;
    if (recomp_d3d_present_hook != NULL) recomp_d3d_present_hook(result.command.data.present.swap_counter);
    recomp_runtime.registers.eax = result.command.data.present.swap_counter;
    recomp_runtime.registers.esp = entry + 8u;
    fprintf(stderr, "recomp d3d8: Swap frame=%u\n", result.command.data.present.swap_counter);
}

void recomp_d3d8_present_card(const uint32_t *pixels, uint32_t width, uint32_t height,
    float display_aspect, uint8_t level)
{
    recomp_d3d8_require_device();
    RecompD3dPresenterCommand command = {.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR};
    resolve_target(color_header(0u), 0u, &command.data.clear.target);
    const RecompD3dPresenterTarget target = command.data.clear.target;
    command.data.clear.clear_color = true;
    command.data.clear.color = 0xff000000u;
    recomp_d3d8_submit(&command);
    RecompD3dCardVertex vertices[4];
    command = (RecompD3dPresenterCommand){.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    recomp_d3d_card_draw(pixels, width, height, (float)target.color.width,
        (float)target.color.height, display_aspect, level, vertices, &command.data.draw);
    command.data.draw.target = target;
    recomp_d3d8_submit(&command);
    command = (RecompD3dPresenterCommand){.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
    command.data.present.effective_flags = 5u;
    command.data.present.swap_counter = frame_state.swap_counter;
    recomp_d3d8_submit(&command);
    back_buffer_displayed = true;
    back_buffer_saved = false;
}

static void block_until_vertical_blank(void)
{
    recomp_d3d8_require_device();
    recomp_d3d_note_block(*recomp_memory_u32(recomp_runtime.registers.esp));
    recomp_d3d_wait_vblank();
    recomp_runtime.registers.esp += 4u;
}

static void flush(bool wait)
{
    recomp_d3d8_require_device();
    const RecompD3dPresenterError error = recomp_d3d_presenter_flush(presenter, wait);
    if (error == RECOMP_D3D_PRESENTER_CLOSED) recomp_stop(0, "host-window-closed");
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:flush:%u", (unsigned)error);
}

static void kick(void) { flush(false); recomp_runtime.registers.esp += 4u; }
static void wait_idle(void) { flush(true); recomp_runtime.registers.esp += 4u; }

static void insert_fence(void)
{
    /* Queued commands own copies of their bytes, so a fence is complete at once.
       Draining here stalled the guest on the presenter's vsync every frame. */
    recomp_d3d8_require_device();
    const uint32_t fence = *recomp_memory_u32(DEVICE + 0x2cu);
    *recomp_memory_u32(DEVICE + 0x2cu) = fence + 2u;
    recomp_runtime.registers.eax = fence;
    recomp_runtime.registers.esp += 4u;
}

static void block_on_fence(void)
{
    /* All returned fences have already completed; the queue keeps work ordered. */
    recomp_d3d8_require_device();
    recomp_runtime.registers.esp += 8u;
}

/* D3DDevice_InsertCallback (0x349e10, stdcall type, callback, context).
   Queued commands own their bytes and the host GPU never writes guest memory,
   so the READ (0) and WRITE (1) conditions already hold here. The cdecl
   callback runs at once on the calling thread instead of the GPU interrupt. */
static void insert_callback(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t type = argument(entry, 0u), callback = argument(entry, 1u);
    uint32_t context = argument(entry, 2u);
    recomp_d3d8_require_device();
    if (type > 1u || callback == 0u) recomp_stop(2, "d3d8:insert-callback:%u", type);
    kernel_call_guest(callback, &context, 1u);
    recomp_runtime.registers.esp = entry + 16u;
}

/* D3DDevice_BeginVisibilityTest (0x3457c0, no arguments) clears the report
   counter and enables Z-pass counting for later draws. */
static void begin_visibility_test(void)
{
    recomp_d3d8_require_device();
    const RecompD3dPresenterCommand command = {.type = RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN};
    recomp_d3d8_submit(&command);
    visibility_open = true;
    recomp_runtime.registers.esp += 4u;
}

/* D3DDevice_EndVisibilityTest (0x345860, stdcall index) reports the count into
   the index's 16-byte slot (0x3457f0). The presenter keeps it instead, so no
   guest slot page is allocated. */
static void end_visibility_test(void)
{
    const uint32_t entry = recomp_runtime.registers.esp, index = argument(entry, 0u);
    recomp_d3d8_require_device();
    if (index >= VISIBILITY_TESTS) recomp_stop(2, "d3d8:visibility-index:%08" PRIx32, index);
    /* The SDK would report a stale counter here; no caller has needed it. */
    if (!visibility_open) recomp_stop(2, "d3d8:visibility-end-without-begin");
    RecompD3dPresenterTarget target;
    recomp_d3d8_target(&target);
    RecompD3dPresenterCommand command = {.type = RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END};
    command.data.visibility_index = index;
    recomp_d3d8_submit(&command);
    visibility_open = false;
    /* The host counts logical pixels; a supersampled back buffer has more samples. */
    visibility_scale[index] = target.offscreen ? 1u : device_state.scale[0] * device_state.scale[1];
    recomp_runtime.registers.eax = RECOMP_D3D_OK;
    recomp_runtime.registers.esp = entry + 8u;
}

/* D3DDevice_GetVisibilityTestResult (0x344450, stdcall index, result,
   timestamp) returns D3DERR_TESTINCOMPLETE while the slot is pending and
   kicks the push buffer when the test is still queued; callers poll. */
static void get_visibility_test_result(void)
{
    const uint32_t entry = recomp_runtime.registers.esp, index = argument(entry, 0u);
    const uint32_t result = argument(entry, 1u), timestamp = argument(entry, 2u);
    recomp_d3d8_require_device();
    if (index >= VISIBILITY_TESTS || visibility_scale[index] == 0u)
        recomp_stop(2, "d3d8:visibility-result-not-ended:%08" PRIx32, index);
    if (result == 0u) recomp_stop(2, "d3d8:visibility-result-null");
    /* shortcut: no host clock maps the GPU report timestamp; bind one when a caller asks for it. */
    if (timestamp != 0u) recomp_stop(2, "d3d8:visibility-timestamp-unbound");
    bool complete = false;
    uint32_t pixels = 0u;
    const RecompD3dPresenterError error =
        recomp_d3d_presenter_visibility_result(presenter, index, &complete, &pixels);
    if (error == RECOMP_D3D_PRESENTER_CLOSED) recomp_stop(0, "host-window-closed");
    if (error != RECOMP_D3D_PRESENTER_OK) recomp_stop(2, "d3d8:visibility-result:%u", (unsigned)error);
    if (complete) {
        *recomp_memory_u32(result) = pixels * visibility_scale[index];
        recomp_runtime.registers.eax = RECOMP_D3D_OK;
    } else {
        flush(false);
        recomp_runtime.registers.eax = D3DERR_TESTINCOMPLETE;
    }
    recomp_runtime.registers.esp = entry + 16u;
}

static void persist_display(void)
{
    /* The native window retains its displayed front buffer across reset. The
       SDK (0x345d70) also publishes a contiguous front-buffer copy (header
       built by 0x345cf0) that the game locks after Reset (0x2823f0). */
    flush(true);
    free_saved_display();
    const uint32_t pitch = (device_state.width * 4u + 63u) & ~63u;
    const uint32_t surface = recomp_kernel_allocate_contiguous(
        0x40u + pitch * device_state.height, 0x7ffffffu, 0x40u, 0x404u);
    if (surface != 0u) {
        /* shortcut: host pixels are not copied; the window keeps showing them.
           Read them back if the game ever displays this surface itself. */
        const RecompD3dSurfaceHeader header = {
            0x81050001u, (surface + 0x40u) & 0x0fffffffu, 0u, device_state.color[1].format,
            ((pitch / 64u - 1u) << 24u) | ((device_state.height - 1u) << 12u) | (device_state.width - 1u), 0u,
        };
        store(surface, &header, sizeof header);
        recomp_kernel_av_set_saved_data_address(surface);
        back_buffer_saved = back_buffer_displayed;
    }
    recomp_runtime.registers.eax = surface != 0u ? RECOMP_D3D_OK : RECOMP_D3D_FAIL;
    recomp_runtime.registers.esp += 4u;
}

/* D3DDevice_CopyRects (0x344aa0, stdcall, 5 arguments). After a mode Reset
   the game (0x2823f0) copies the saved display back into a 720-wide back
   buffer so the next Swap repeats the last frame. The host back buffer already
   holds those pixels, so that copy is complete without host work. A shattering
   stage mirror (0x277670) copies the whole back buffer into a render-target
   texture of its size and format, then draws the shards with that texture. */
static void copy_rects(void)
{
    const uint32_t entry = recomp_runtime.registers.esp;
    const uint32_t source = argument(entry, 0u), rects = argument(entry, 1u),
        count = argument(entry, 2u), destination = argument(entry, 3u), points = argument(entry, 4u);
    recomp_d3d8_require_device();
    RecompD3dTextureDesc target, main_color;
    target_surface(destination, false, &target);
    target_surface(color_header(0u), false, &main_color);
    bool bound = source != 0u && source == recomp_kernel_av_get_saved_data_address() &&
        back_buffer_saved && target.data == main_color.data && points == 0u &&
        device_state.scale[0] == 1u && device_state.scale[1] == 1u;
    if (bound && rects != 0u) {
        const uint32_t *rect = recomp_memory_u32(rects);
        bound = count == 1u && rect[0] == 0u && rect[1] == 0u && rect[2] != 0u && rect[3] != 0u &&
            rect[2] <= target.width && rect[3] <= target.height;
    }
    if (!bound && source != 0u && rects == 0u && points == 0u &&
        device_state.scale[0] == 1u && device_state.scale[1] == 1u) {
        RecompD3dTextureDesc copied;
        target_surface(source, false, &copied);
        bound = copied.data == main_color.data && target.data != main_color.data &&
            target.width == main_color.width && target.height == main_color.height &&
            target.format_byte == main_color.format_byte;
        if (bound) {
            RecompD3dPresenterCommand command = {.type = RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR};
            command.data.copy_destination = target;
            recomp_d3d8_submit(&command);
        }
    }
    /* shortcut: only the restore and whole main-target snapshots are bound;
       partial or scaled copies need a presenter rectangle copy. */
    if (!bound) recomp_stop(2, "d3d8:copy-rects-unbound:%08" PRIx32 "->%08" PRIx32, source, destination);
    recomp_runtime.registers.esp = entry + 24u;
}

static void reset_device(void)
{
    recomp_d3d8_require_device();
    const uint32_t parameters = argument(recomp_runtime.registers.esp, 0u);
    RecompD3dCreateRequest request = {.device_type = 1u, .behavior_flags = 0x40u};
    RecompD3dDeviceState candidate = {0};
    RecompD3dSurfaceHeader colors[2], depth;
    if (parameters == 0u) recomp_stop(2, "d3d8:reset-pointer");
    load(&request.presentation, parameters, sizeof request.presentation);
    if (!recomp_d3d_create_request_supported(&request)) {
        const uint32_t *words = (const uint32_t *)(const void *)&request.presentation;
        for (uint32_t i = 0u; i < sizeof request.presentation / sizeof *words; ++i)
            fprintf(stderr, "recomp d3d8: reset-presentation[%u]=%08x\n", i, words[i]);
        recomp_stop(2, "d3d8:reset-mode");
    }
    for (uint32_t i = 0u; i < 2u; ++i)
        load(&colors[i], request.presentation.buffer_surfaces[i], sizeof colors[i]);
    load(&depth, request.presentation.depth_stencil_surface, sizeof depth);
    const RecompD3dDeviceError error = recomp_d3d_device_create(&candidate, &request, colors, &depth);
    if (error != RECOMP_D3D_DEVICE_OK) recomp_stop(2, "d3d8:reset-surfaces:%u", (unsigned)error);
    if (candidate.width != device_state.width || candidate.height != device_state.height)
        recomp_stop(2, "d3d8:reset-resize-unbound");
    for (uint32_t i = 0u; i < 2u; ++i) validate_storage(&colors[i]);
    validate_storage(&depth);
    flush(true);
    /* Same native attachments: retain displayed pixels and all non-RT state. */
    device_state = candidate;
    for (uint32_t i = 0u; i < 2u; ++i)
        store(color_header(i), &candidate.color[i], sizeof candidate.color[i]);
    store(DEVICE + DEPTH_HEADER, &candidate.depth, sizeof candidate.depth);
    *recomp_memory_u32(DEVICE + TARGET) = color_header(0u);
    *recomp_memory_u32(DEVICE + DEPTH) = DEVICE + DEPTH_HEADER;
    *recomp_memory_u32(DEVICE + DEFAULT_DEPTH) = DEVICE + DEPTH_HEADER;
    store(DEVICE + VIEWPORT, &candidate.viewport, sizeof candidate.viewport);
    *recomp_memory_u32(DEVICE + 0x1c00u) = *recomp_memory_u32(DEVICE + 0x1c04u) = 0u;
    *recomp_memory_u32(0x35545cu) = request.presentation.full_screen_presentation_interval;
    write_frame_buffer_mode(&request.presentation);
    /* Reset rebinds the default depth buffer, whose pitch follows the mode. */
    *recomp_memory_u32(DEVICE + 0x94cu) = ((candidate.depth.size >> 24u) + 1u) * 64u;
    /* InitializeFrameBuffers also selects D3DRS_MULTISAMPLEMODE (0 without the
       0x1000 multisample bit) and the linear swap filter (2). */
    *recomp_memory_u32(0x3554c8u) = 0u;
    *recomp_memory_u32(0x355458u) = 2u;
    RecompD3dFrameResult clear = recomp_d3d_frame_clear(&frame_state, 0u, 0u,
        RECOMP_D3D_CLEAR_DEPTH | RECOMP_D3D_CLEAR_STENCIL, 0u, 0x3f800000u, 0u);
    if (clear.error != RECOMP_D3D_FRAME_OK) recomp_stop(2, "d3d8:reset-clear");
    recomp_d3d8_submit(&clear.command);
    display_mode_pending = true;
    recomp_runtime.registers.eax = RECOMP_D3D_OK;
    recomp_runtime.registers.esp += 8u;
    fprintf(stderr, "recomp d3d8: Reset %ux%u multisample=%04x interval=%08x frame=%u\n",
        candidate.width, candidate.height, request.presentation.multi_sample_type,
        request.presentation.full_screen_presentation_interval, frame_state.swap_counter);
}

/* Every SDK setter that is still unreplaced reserves console push-buffer
   space here, then waits forever for GPU hardware this host lacks. Stop with
   the caller's return address so the missing API is named immediately. */
static void push_buffer_unbound(void)
{
    recomp_stop(2, "d3d8:push-buffer-unbound:ret=%08" PRIx32,
        *recomp_memory_u32(recomp_runtime.registers.esp));
}

RecompFunction recomp_d3d8_lookup_manual(uint32_t address)
{
    switch (address) {
    case 0x0034a470u: return create_device;
    case 0x00347e80u: return add_ref;
    case 0x00347ec0u: return release;
    case 0x00344a50u: return get_back_buffer;
    case 0x00344df0u: return get_depth_surface;
    case 0x00344dd0u: return get_render_target;
    case 0x00344360u: return get_viewport;
    case 0x00344e10u: return set_viewport;
    case 0x003447d0u: return set_render_target;
    case 0x003459e0u: return set_tile;
    case 0x00347920u: return clear;
    case 0x0034af80u: return swap;
    case 0x0034a310u: return push_buffer_unbound;
    case 0x00344440u: return kick;
    case 0x00344420u: case 0x0034a370u: return wait_idle;
    case 0x003445f0u: return insert_fence;
    case 0x00344430u: return block_on_fence;
    case 0x00349e10u: return insert_callback;
    case 0x003457c0u: return begin_visibility_test;
    case 0x00345860u: return end_visibility_test;
    case 0x00344450u: return get_visibility_test_result;
    case 0x00345d70u: return persist_display;
    case 0x00344aa0u: return copy_rects;
    case 0x00344720u: return reset_device;
    /* CRI's no-argument tail aliases bypass generated callee lookup. */
    case 0x002fc300u: case 0x002fc5a0u: case 0x003444c0u: return block_until_vertical_blank;
    default: return NULL;
    }
}
