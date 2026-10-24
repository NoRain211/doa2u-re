/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d_device_model.h"
#include "d3d_frame_model.h"
#include "d3d_tile_model.h"
#include "d3d_vblank.h"
#include "stop_report.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MEMORY_SIZE = 8u * 1024u * 1024u,
    PARAMETERS = 0x1000u,
    OUTPUT = 0x1800u,
    VIEWPORT_OUTPUT = 0x1900u,
    RECT_SOURCE = 0x1980u,
    TILE_SOURCE = 0x1a00u,
    COLOR_SOURCE = 0x2000u,
    DEPTH_SOURCE = 0x2200u,
    OFFSCREEN = 0x2400u,
    OFFSCREEN_DEPTH = 0x2500u,
    BLOOM_SOURCE = 0x2600u,
    SNAPSHOT = 0x2700u,
    ENTRY_ESP = 0x8000u,
    DEVICE_GLOBAL = 0x3554f8u,
    DEVICE = 0x355500u,
    DEVICE_SIZE = 0x24a0u,
    COLOR_HEADER = DEVICE + 0x1a24u,
    DEPTH_HEADER = DEVICE + 0x1a6cu,
    EAX_SENTINEL = 0xa5a5a5a5u,
};

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram = NULL;

struct RecompD3dPresenter {
    bool active;
};

static uint32_t memory[MEMORY_SIZE / 4u];
static unsigned char saved_device[DEVICE_SIZE];
static struct RecompD3dPresenter mock_presenter;
static RecompD3dPresenterConfig created_config;
static RecompD3dPresenterCommand submitted;
static RecompD3dPresenterError create_error;
static RecompD3dPresenterError submit_error;
static uint32_t create_count, submit_count, destroy_count;
static uint32_t vblank_reset_count, wait_present_count, wait_vblank_count;
void recomp_d3d8_state_reset(void) {}
void recomp_d3d8_state_write_defaults(bool auto_depth) { (void)auto_depth; }
void recomp_d3d8_resource_reset(void) {}
bool recomp_d3d8_resource_describe(uint32_t resource, RecompD3dTextureDesc *out)
{
    const uint32_t *header = recomp_memory_u32(resource);
    const uint32_t format = (header[3] >> 8u) & 255u;
    if ((header[0] & 0x70000u) != 0x50000u) return false;
    *out = (RecompD3dTextureDesc){
        .format_byte = format, .bits_per_pixel = 32u, .render_target = true,
        .depth = format == 0x2au || format == 0x2eu, .linear = header[4] != 0u,
        .width = header[4] ? (header[4] & 0xfffu) + 1u : 1u << ((header[3] >> 20u) & 15u),
        .height = header[4] ? ((header[4] >> 12u) & 0xfffu) + 1u : 1u << ((header[3] >> 24u) & 15u),
        .pitch = header[4] ? ((header[4] >> 24u) + 1u) * 64u : 0u,
        .data = header[1] & 0x03ffffffu, .mip_levels = 1u,
    };
    return true;
}
uint32_t recomp_d3d8_resource_add_ref(uint32_t resource)
{
    return ++*recomp_memory_u32(resource) & RECOMP_D3D_SURFACE_REFERENCE_MASK;
}
void recomp_d3d8_resource_set_render_targets(uint32_t color, uint32_t depth)
{
    const uint32_t old_color = *recomp_memory_u32(DEVICE + 0x1a04u);
    const uint32_t old_depth = *recomp_memory_u32(DEVICE + 0x1a08u);
    *recomp_memory_u32(color) += RECOMP_D3D_SURFACE_BOUND;
    if (depth != 0u) *recomp_memory_u32(depth) += RECOMP_D3D_SURFACE_BOUND;
    *recomp_memory_u32(old_color) -= RECOMP_D3D_SURFACE_BOUND;
    if (old_depth != 0u) *recomp_memory_u32(old_depth) -= RECOMP_D3D_SURFACE_BOUND;
    *recomp_memory_u32(DEVICE + 0x1a04u) = color;
    *recomp_memory_u32(DEVICE + 0x1a08u) = depth;
}
void recomp_d3d_presenter_report_draw_textures(void) {}
static unsigned flush_count;
static bool flush_wait;
static bool coverage = true;
static RecompD3dPresenterError coverage_error = RECOMP_D3D_PRESENTER_OK;
RecompD3dPresenterError recomp_d3d_presenter_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered)
{
    (void)presenter; (void)draw;
    *covered = coverage;
    return coverage_error;
}
RecompD3dPresenterError recomp_d3d_presenter_flush(RecompD3dPresenter *presenter, bool wait)
{
    (void)presenter;
    ++flush_count;
    flush_wait = wait;
    return RECOMP_D3D_PRESENTER_OK;
}
RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    (void)presenter; (void)base; (void)size;
    return RECOMP_D3D_PRESENTER_OK;
}
RecompD3dPresenterError recomp_d3d_presenter_invalidate_textures(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    (void)presenter; (void)base; (void)size;
    return RECOMP_D3D_PRESENTER_OK;
}
void recomp_d3d_wait_vblank(void) { ++wait_vblank_count; }
void recomp_d3d_note_block(uint32_t return_address) { (void)return_address; }
static bool visibility_complete;
static uint32_t visibility_pixels, visibility_queried;
RecompD3dPresenterError recomp_d3d_presenter_visibility_result(
    RecompD3dPresenter *presenter, uint32_t index, bool *complete, uint32_t *pixels)
{
    (void)presenter;
    visibility_queried = index;
    *complete = visibility_complete;
    *pixels = visibility_pixels;
    return RECOMP_D3D_PRESENTER_OK;
}
static uint32_t dispatched, dispatched_context, dispatched_return;
void recomp_dispatch_indirect_site(uint32_t guest_address, uint32_t saved_esp,
    const char *member, int line)
{
    (void)saved_esp; (void)member; (void)line;
    dispatched = guest_address;
    dispatched_return = *recomp_memory_u32(recomp_runtime.registers.esp);
    dispatched_context = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    recomp_runtime.registers.esp += 4u; /* cdecl: the callee pops only its return. */
}
static jmp_buf stop_environment;
static bool stop_armed;
static int stop_code;
static char stop_reason[128];

void recomp_stop(int fallback_code, const char *format, ...)
{
    va_list args;

    stop_code = fallback_code;
    va_start(args, format);
    vsnprintf(stop_reason, sizeof stop_reason, format, args);
    va_end(args);
    if (stop_armed) {
        longjmp(stop_environment, 1);
    }
    fprintf(stderr, "D3D adapter: unexpected stop %s\n", stop_reason);
    abort();
}

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    const uint32_t window = address & ~(RECOMP_XBOX_RAM_SIZE - 1u);
    const uint32_t offset = address & (RECOMP_XBOX_RAM_SIZE - 1u);

    if ((window != 0u && window != RECOMP_XBOX_CACHED_ALIAS &&
         window != RECOMP_XBOX_PHYSICAL_ALIAS) ||
        offset > MEMORY_SIZE || width > MEMORY_SIZE - offset) {
        recomp_stop(2, "memory:0x%08x", address);
    }
    return (uint8_t *)(void *)memory + offset;
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config,
    RecompD3dPresenter **presenter)
{
    ++create_count;
    created_config = *config;
    if (create_error != RECOMP_D3D_PRESENTER_OK) {
        *presenter = NULL;
        return create_error;
    }
    mock_presenter.active = true;
    *presenter = &mock_presenter;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter,
    const RecompD3dPresenterCommand *command)
{
    if (presenter != &mock_presenter || !presenter->active) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    ++submit_count;
    submitted = *command;
    return submit_error;
}

RecompD3dPresenterError recomp_d3d_presenter_destroy(
    RecompD3dPresenter **presenter)
{
    ++destroy_count;
    mock_presenter.active = false;
    *presenter = NULL;
    return RECOMP_D3D_PRESENTER_OK;
}

static uint32_t saved_display, freed_display;
uint32_t recomp_kernel_av_get_saved_data_address(void) { return saved_display; }
void recomp_kernel_av_set_saved_data_address(uint32_t address) { saved_display = address; }
uint32_t recomp_kernel_allocate_contiguous(
    uint32_t size, uint32_t highest_address, uint32_t alignment, uint32_t protect)
{
    return size == 0x40u + 2880u * 480u && highest_address == 0x7ffffffu &&
        alignment == 0x40u && protect == 0x404u ? 0x7f0000u : 0u;
}
void recomp_kernel_free_pool(uint32_t base) { freed_display = base; }

void recomp_d3d_vblank_reset(void)
{
    ++vblank_reset_count;
}

void recomp_d3d_wait_present(void)
{
    ++wait_present_count;
}

static int expect(const char *name, bool condition)
{
    if (!condition) {
        fprintf(stderr, "D3D adapter: %s failed\n", name);
        return 0;
    }
    return 1;
}

static int call(
    uint32_t address,
    const uint32_t *arguments,
    size_t argument_count,
    const char *expected_stop)
{
    RecompFunction function = recomp_d3d8_lookup_manual(address);
    uint32_t *stack = recomp_memory_u32(ENTRY_ESP);

    if (function == NULL) {
        fprintf(stderr, "D3D adapter: lookup 0x%08x failed\n", address);
        return 0;
    }
    stack[0] = 0x10101010u;
    if (argument_count != 0u) {
        memcpy(stack + 1u, arguments, argument_count * 4u);
    }
    recomp_runtime.registers = (RecompRegisters){
        .eax = EAX_SENTINEL, .ebx = 0x12345678u, .esi = 0x87654321u,
        .edi = 0xabcdef01u, .ebp = 0x10203040u, .esp = ENTRY_ESP,
    };
    stop_reason[0] = '\0';
    stop_armed = true;
    if (setjmp(stop_environment) == 0) {
        function();
        stop_armed = false;
        return expect("call returned as expected", expected_stop == NULL);
    }
    stop_armed = false;
    return expect("expected fail-loud boundary",
        expected_stop != NULL && stop_code == 2 &&
        strncmp(stop_reason, expected_stop, strlen(expected_stop)) == 0);
}

static void reset_fixture(void)
{
    const RecompD3dPresentationParameters presentation = {
        .back_buffer_width = 720u, .back_buffer_height = 480u,
        .back_buffer_format = 0x12u, .back_buffer_count = 1u,
        .multi_sample_type = 0x11u, .swap_effect = 1u,
        .enable_auto_depth_stencil = 1u, .auto_depth_stencil_format = 0x2eu,
        .full_screen_presentation_interval = 0x80000001u,
        .buffer_surfaces = {COLOR_SOURCE, COLOR_SOURCE + 0x18u, 0u},
        .depth_stencil_surface = DEPTH_SOURCE,
    };
    const RecompD3dSurfaceHeader colors[2] = {
        {0x50001u, 0x100000u, 0u, 0x11229u, 0x2f1df2cfu, 0u},
        {0x50001u, 0x400000u, 0u, 0x11229u, 0x2f1df2cfu, 0u},
    };
    const RecompD3dSurfaceHeader depth = {
        0x50001u, 0x600000u, 0u, 0x12e29u, 0x2f1df2cfu, 0u,
    };

    recomp_d3d8_shutdown();
    memset(memory, 0, sizeof memory);
    memset(&recomp_runtime, 0, sizeof recomp_runtime);
    create_count = submit_count = destroy_count = 0u;
    vblank_reset_count = wait_present_count = 0u;
    create_error = submit_error = RECOMP_D3D_PRESENTER_OK;
    memcpy(recomp_memory(PARAMETERS, sizeof presentation), &presentation,
        sizeof presentation);
    memcpy(recomp_memory(COLOR_SOURCE, sizeof colors), colors, sizeof colors);
    memcpy(recomp_memory(DEPTH_SOURCE, sizeof depth), &depth, sizeof depth);
    memset(recomp_memory(DEVICE, DEVICE_SIZE), 0xa5, DEVICE_SIZE);
    memcpy(saved_device, recomp_memory(DEVICE, DEVICE_SIZE), DEVICE_SIZE);
    *recomp_memory_u32(OUTPUT) = EAX_SENTINEL;
}

static int create(void)
{
    const uint32_t arguments[] = {0u, 1u, 0u, 0x40u, PARAMETERS, OUTPUT};
    return call(0x34a470u, arguments, 6u, NULL);
}

static int coverage_tests(void)
{
    RecompD3dPresenterDrawCommand draw = {0};
    int passed = 1;
    coverage = false;
    passed &= expect("empty GPU coverage is returned", !recomp_d3d8_draw_has_coverage(&draw));
    coverage = true;
    passed &= expect("visible GPU coverage is returned", recomp_d3d8_draw_has_coverage(&draw));
    coverage = false;
    coverage_error = RECOMP_D3D_PRESENTER_HOST_FAILURE;
    stop_armed = true;
    if (setjmp(stop_environment) == 0) {
        recomp_d3d8_draw_has_coverage(&draw);
        passed &= expect("failed query must not discard the draw", false);
    } else {
        passed &= expect("failed query stops with its error", stop_code == 2 &&
            strncmp(stop_reason, "d3d8:coverage:", strlen("d3d8:coverage:")) == 0);
    }
    stop_armed = false;
    coverage = true;
    coverage_error = RECOMP_D3D_PRESENTER_OK;
    return passed;
}

int main(void)
{
    uint32_t arguments[6] = {0};
    RecompD3dSurfaceHeader before[2], after[2];
    RecompD3dViewport viewport;
    RecompD3dTile tile = {{1u, 0x1000u, 3u, 4u, 5u, 6u}};
    uint32_t prior_submits;
    int passed = 1;

    passed &= expect("unbound SDK and sister addresses stay unbound",
        recomp_d3d8_lookup_manual(0u) == NULL &&
        recomp_d3d8_lookup_manual(0x34a471u) == NULL &&
        recomp_d3d8_lookup_manual(0x1e4930u) == NULL);
    *recomp_memory_u32(ENTRY_ESP) = 0x00346793u;
    recomp_runtime.registers.esp = ENTRY_ESP;
    stop_armed = true;
    if (setjmp(stop_environment) == 0) {
        recomp_d3d8_lookup_manual(0x34a310u)();
        passed &= expect("push-buffer reservation stops", false);
    }
    stop_armed = false;
    passed &= expect("push-buffer stop names its caller",
        strcmp(stop_reason, "d3d8:push-buffer-unbound:ret=00346793") == 0);
    reset_fixture();
    create_error = RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    passed &= create();
    passed &= expect("host allocation failure preserves guest device",
        recomp_runtime.registers.eax == RECOMP_D3D_OUT_OF_MEMORY &&
        recomp_runtime.registers.esp == ENTRY_ESP + 28u &&
        *recomp_memory_u32(OUTPUT) == 0u &&
        *recomp_memory_u32(DEVICE_GLOBAL) == 0u && create_count == 1u &&
        submit_count == 0u && vblank_reset_count == 0u &&
        memcmp(saved_device, recomp_memory(DEVICE, DEVICE_SIZE), DEVICE_SIZE) == 0);
    create_error = RECOMP_D3D_PRESENTER_HOST_FAILURE;
    passed &= create();
    passed &= expect("host failure returns HRESULT",
        recomp_runtime.registers.eax == RECOMP_D3D_FAIL &&
        *recomp_memory_u32(OUTPUT) == 0u &&
        *recomp_memory_u32(DEVICE_GLOBAL) == 0u);
    arguments[1] = 1u;
    arguments[3] = 0x40u;
    arguments[5] = OUTPUT;
    passed &= call(0x34a470u, arguments, 6u, NULL);
    passed &= expect("null presentation rejects with HRESULT",
        recomp_runtime.registers.eax == RECOMP_D3D_INVALID_CALL &&
        recomp_runtime.registers.esp == ENTRY_ESP + 28u &&
        *recomp_memory_u32(OUTPUT) == 0u);
    arguments[4] = PARAMETERS;
    *recomp_memory_u32(PARAMETERS) = 640u;
    passed &= call(0x34a470u, arguments, 6u, "d3d8:create-device-request");
    passed &= expect("unsupported request does not publish or mutate device",
        *recomp_memory_u32(OUTPUT) == 0u &&
        *recomp_memory_u32(DEVICE_GLOBAL) == 0u &&
        memcmp(saved_device, recomp_memory(DEVICE, DEVICE_SIZE), DEVICE_SIZE) == 0);
    reset_fixture();
    *recomp_memory_u32(PARAMETERS + 0x28u) = 0x10u;
    passed &= create();
    passed &= expect("creation ABI and presenter configuration",
        recomp_runtime.registers.eax == RECOMP_D3D_OK &&
        recomp_runtime.registers.esp == ENTRY_ESP + 28u &&
        recomp_runtime.registers.ebx == 0x12345678u &&
        *recomp_memory_u32(OUTPUT) == DEVICE &&
        *recomp_memory_u32(DEVICE_GLOBAL) == DEVICE &&
        created_config.width == 720u && created_config.height == 480u &&
        created_config.color_format == RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM &&
        created_config.depth_format == RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8 &&
        create_count == 1u && submit_count == 1u && vblank_reset_count == 1u &&
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_CLEAR &&
        !submitted.data.clear.clear_color && submitted.data.clear.clear_depth &&
        submitted.data.clear.clear_stencil);
    passed &= expect("verified surface pointer layout",
        *recomp_memory_u32(DEVICE + 0x1a04u) == COLOR_HEADER &&
        *recomp_memory_u32(DEVICE + 0x1a08u) == DEPTH_HEADER &&
        *recomp_memory_u32(DEVICE + 0x1a10u) == 2u &&
        *recomp_memory_u32(DEVICE + 0x1a14u) == COLOR_HEADER &&
        *recomp_memory_u32(DEVICE + 0x1a18u) == COLOR_HEADER + 0x18u &&
        *recomp_memory_u32(DEVICE + 0x1a1cu) == 0u &&
        *recomp_memory_u32(DEVICE + 0x1a20u) == DEPTH_HEADER &&
        *recomp_memory_u32(COLOR_HEADER) == 0xd0002u &&
        *recomp_memory_u32(COLOR_HEADER + 0x18u) == 0x50002u &&
        *recomp_memory_u32(DEPTH_HEADER) == 0xd0002u &&
        *recomp_memory_u32(COLOR_SOURCE) == 0x50001u);
    passed &= expect("default screen-space raster bias",
        *recomp_memory_u32(DEVICE + 0xef8u) == 0x3f080000u &&
        *recomp_memory_u32(DEVICE + 0xefcu) == 0x3f080000u);
    memcpy(&viewport, recomp_memory(DEVICE + 0xee0u, sizeof viewport), sizeof viewport);
    for (uint32_t bank = 0u; bank < 2u; ++bank) {
        const uint8_t *ramp = recomp_memory(DEVICE + 0x1e04u + bank * 0x300u, 0x300u);
        for (uint32_t plane = 0u; plane < 3u; ++plane) {
            for (uint32_t i = 0u; i < 256u; ++i) {
                passed &= expect("initial gamma identity", ramp[plane * 256u + i] == i);
            }
        }
    }
    passed &= expect("initial gamma bank pending and completion",
        *recomp_memory_u32(DEVICE + 0x2404u) == 1u &&
        *recomp_memory_u32(DEVICE + 0x2408u) == 0u &&
        *recomp_memory_u32(DEVICE + 0x240cu) == 0u &&
        *recomp_memory_u32(DEVICE + 0x1de4u) == 0u);
    passed &= expect("initial guest viewport and scales",
        viewport.x == 0u && viewport.y == 0u && viewport.width == 720u &&
        viewport.height == 480u && viewport.min_z == 0.0f && viewport.max_z == 1.0f &&
        *recomp_memory_u32(DEVICE + 0x954u) == 720u &&
        *recomp_memory_u32(DEVICE + 0x958u) == 480u &&
        *recomp_memory_u32(DEVICE + 0x95cu) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x96cu) == 0x3f800000u);
    arguments[0] = 0u;
    passed &= call(0x344a50u, arguments, 1u, NULL);
    passed &= expect("back buffer getter adds reference and retires argument",
        recomp_runtime.registers.eax == COLOR_HEADER &&
        recomp_runtime.registers.esp == ENTRY_ESP + 8u &&
        *recomp_memory_u32(COLOR_HEADER) == 0xd0003u);
    arguments[0] = COLOR_HEADER;
    passed &= call(0x347ec0u, arguments, 1u, NULL);
    passed &= call(0x344dd0u, arguments, 0u, NULL);
    passed &= expect("render target getter returns retained header",
        recomp_runtime.registers.eax == COLOR_HEADER &&
        recomp_runtime.registers.esp == ENTRY_ESP + 4u &&
        *recomp_memory_u32(COLOR_HEADER) == 0xd0003u);
    passed &= call(0x347ec0u, arguments, 1u, NULL);
    passed &= expect("back buffer release stays stable",
        recomp_runtime.registers.eax == 2u &&
        *recomp_memory_u32(COLOR_HEADER) == 0xd0002u);
    arguments[0] = UINT32_MAX;
    passed &= call(0x344a50u, arguments, 1u, NULL);
    passed &= expect("minus one resolves front buffer",
        recomp_runtime.registers.eax == COLOR_HEADER + 0x18u &&
        *recomp_memory_u32(COLOR_HEADER + 0x18u) == 0x50003u);
    arguments[0] = COLOR_HEADER + 0x18u;
    passed &= call(0x347ec0u, arguments, 1u, NULL);
    arguments[0] = COLOR_HEADER;
    passed &= call(0x347e80u, arguments, 1u, NULL);
    passed &= expect("explicit AddRef returns count", recomp_runtime.registers.eax == 3u);
    passed &= call(0x347ec0u, arguments, 1u, NULL);
    passed &= call(0x344df0u, NULL, 0u, NULL);
    passed &= expect("depth getter no-argument ABI",
        recomp_runtime.registers.eax == DEPTH_HEADER &&
        recomp_runtime.registers.esp == ENTRY_ESP + 4u &&
        *recomp_memory_u32(DEPTH_HEADER) == 0xd0003u);
    arguments[0] = DEPTH_HEADER;
    passed &= call(0x347ec0u, arguments, 1u, NULL);
    arguments[0] = VIEWPORT_OUTPUT;
    passed &= call(0x344360u, arguments, 1u, NULL);
    passed &= expect("viewport getter void ABI",
        recomp_runtime.registers.eax == EAX_SENTINEL &&
        recomp_runtime.registers.esp == ENTRY_ESP + 8u &&
        memcmp(recomp_memory(VIEWPORT_OUTPUT, sizeof viewport), &viewport, sizeof viewport) == 0);
    viewport = (RecompD3dViewport){700u, 460u, UINT32_MAX, UINT32_MAX, 0.0f, 1.0f};
    memcpy(recomp_memory(VIEWPORT_OUTPUT, sizeof viewport), &viewport, sizeof viewport);
    *recomp_memory_u32(DEVICE + 0x1c00u) = *recomp_memory_u32(DEVICE + 0x1c04u) = 1u;
    passed &= call(0x344e10u, arguments, 1u, NULL);
    passed &= expect("viewport resets scissors", *recomp_memory_u32(DEVICE + 0x1c00u) == 0u &&
        *recomp_memory_u32(DEVICE + 0x1c04u) == 0u);
    passed &= expect("viewport setter clamps extent",
        *recomp_memory_u32(DEVICE + 0xee0u) == 700u &&
        *recomp_memory_u32(DEVICE + 0xee8u) == 20u &&
        *recomp_memory_u32(DEVICE + 0xeecu) == 20u);
    arguments[0] = 0u; arguments[1] = 0u;
    arguments[2] = 0xf3u; arguments[3] = 0x10203040u;
    arguments[4] = 0x3f800000u; arguments[5] = 0u;
    passed &= call(0x347920u, arguments, 6u, "d3d8:clear-viewport-unbound");
    arguments[0] = COLOR_HEADER; arguments[1] = 0u;
    *recomp_memory_u32(DEVICE + 0x1c00u) = 1u;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    passed &= expect("render target resets scissors", *recomp_memory_u32(DEVICE + 0x1c00u) == 0u);
    passed &= expect("render target removes depth binding and resets viewport",
        *recomp_memory_u32(DEVICE + 0x1a08u) == 0u &&
        *recomp_memory_u32(DEPTH_HEADER) == 0x50002u &&
        *recomp_memory_u32(DEVICE + 0xee8u) == 720u);
    arguments[0] = 0u; arguments[1] = 0u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= expect("clear without depth submits only color",
        submitted.data.clear.clear_color && !submitted.data.clear.clear_depth &&
        !submitted.data.clear.clear_stencil && submitted.data.clear.target.no_depth);
    arguments[0] = 0u; arguments[1] = DEPTH_HEADER;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    /* 0x2756f0 draws its bloom source into a 720x480 texture against main depth. */
    const RecompD3dSurfaceHeader bloom = {0x1050001u, 0x200000u, 0u, 0x11229u, 0x2c1df2cfu, 0u};
    memcpy(recomp_memory(BLOOM_SOURCE, sizeof bloom), &bloom, sizeof bloom);
    arguments[0] = BLOOM_SOURCE; arguments[1] = DEPTH_HEADER;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    arguments[0] = 0u; arguments[1] = 0u; arguments[2] = 0xf0u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= expect("offscreen color shares the main depth",
        submitted.data.clear.target.offscreen && !submitted.data.clear.target.custom_depth &&
        !submitted.data.clear.target.no_depth && submitted.data.clear.target.color.width == 720u &&
        submitted.data.clear.target.depth.data == *recomp_memory_u32(DEPTH_HEADER + 4u) &&
        submitted.data.clear.clear_color && !submitted.data.clear.clear_depth);
    arguments[2] = 0xf3u;
    arguments[0] = COLOR_HEADER; arguments[1] = DEPTH_HEADER;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    passed &= expect("render target restores depth binding",
        *recomp_memory_u32(DEVICE + 0x1a08u) == DEPTH_HEADER &&
        *recomp_memory_u32(DEPTH_HEADER) == 0xd0002u &&
        recomp_runtime.registers.eax == EAX_SENTINEL &&
        recomp_runtime.registers.esp == ENTRY_ESP + 12u);
    arguments[0] = 0u; arguments[1] = 0u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= expect("six-argument clear preserves void return and submits semantic values",
        recomp_runtime.registers.esp == ENTRY_ESP + 28u &&
        recomp_runtime.registers.eax == EAX_SENTINEL &&
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_CLEAR &&
        submitted.data.clear.clear_color && submitted.data.clear.clear_depth &&
        submitted.data.clear.clear_stencil && submitted.data.clear.color == 0x10203040u &&
        submitted.data.clear.z == 1.0f && submitted.data.clear.stencil == 0u);
    const RecompD3dSurfaceHeader offscreen = {
        0x1050001u, 0x200000u, 0u, 0x11229u, 0x0e09f0efu, 0u,
    };
    const RecompD3dSurfaceHeader offscreen_depth = {
        0x50001u, 0x300000u, 0u, 0x12e29u, 0x0e09f0efu, 0u,
    };
    memcpy(recomp_memory(OFFSCREEN, sizeof offscreen), &offscreen, sizeof offscreen);
    memcpy(recomp_memory(OFFSCREEN_DEPTH, sizeof offscreen_depth), &offscreen_depth, sizeof offscreen_depth);
    arguments[0] = OFFSCREEN; arguments[1] = 0u;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    passed &= expect("offscreen binding resets viewport to target dimensions",
        *recomp_memory_u32(DEVICE + 0x1a04u) == OFFSCREEN &&
        *recomp_memory_u32(DEVICE + 0xee8u) == 240u &&
        *recomp_memory_u32(DEVICE + 0xeecu) == 160u &&
        recomp_runtime.registers.eax == EAX_SENTINEL &&
        recomp_runtime.registers.esp == ENTRY_ESP + 12u);
    viewport = (RecompD3dViewport){230u, 150u, UINT32_MAX, UINT32_MAX, 0.0f, 1.0f};
    memcpy(recomp_memory(VIEWPORT_OUTPUT, sizeof viewport), &viewport, sizeof viewport);
    arguments[0] = VIEWPORT_OUTPUT;
    passed &= call(0x344e10u, arguments, 1u, NULL);
    passed &= expect("offscreen viewport clamps against target",
        *recomp_memory_u32(DEVICE + 0xee8u) == 10u && *recomp_memory_u32(DEVICE + 0xeecu) == 10u);
    arguments[0] = 0u; arguments[1] = OFFSCREEN_DEPTH;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    arguments[0] = 0u; arguments[1] = 0u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= expect("offscreen clear snapshots color and custom depth",
        submitted.data.clear.target.offscreen && submitted.data.clear.target.custom_depth &&
        !submitted.data.clear.target.no_depth && submitted.data.clear.target.color.width == 240u &&
        submitted.data.clear.target.color.data == 0x200000u &&
        submitted.data.clear.target.depth.data == 0x300000u &&
        *recomp_memory_u32(DEVICE + 0x948u) == 0x4b7fffffu &&
        *recomp_memory_u32(DEVICE + 0x94cu) == 960u);
    arguments[0] = COLOR_HEADER + 24u; arguments[1] = 0u;
    passed &= call(0x3447d0u, arguments, 2u, "d3d8:target-front-buffer-unbound");
    arguments[0] = COLOR_HEADER; arguments[1] = OFFSCREEN_DEPTH;
    passed &= call(0x3447d0u, arguments, 2u, "d3d8:target-depth-size-unbound");
    *recomp_memory_u32(OFFSCREEN + 12u) = 0x11029u;
    arguments[0] = OFFSCREEN; arguments[1] = 0u;
    passed &= call(0x3447d0u, arguments, 2u, "d3d8:target-surface");
    passed &= expect("unsupported targets preserve bindings and viewport",
        *recomp_memory_u32(DEVICE + 0x1a04u) == OFFSCREEN &&
        *recomp_memory_u32(DEVICE + 0x1a08u) == OFFSCREEN_DEPTH &&
        *recomp_memory_u32(OFFSCREEN_DEPTH) == 0xd0001u &&
        *recomp_memory_u32(DEVICE + 0xee8u) == 240u);
    *recomp_memory_u32(OFFSCREEN + 12u) = offscreen.format;
    arguments[0] = COLOR_HEADER; arguments[1] = 0u;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    passed &= expect("restoring main target preserves creation dimensions and disables depth",
        *recomp_memory_u32(DEVICE + 0xee8u) == 720u &&
        *recomp_memory_u32(DEVICE + 0xeecu) == 480u);
    arguments[0] = 0u; arguments[1] = 0u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= expect("main target with null depth stays disabled",
        !submitted.data.clear.target.offscreen && submitted.data.clear.target.no_depth &&
        !submitted.data.clear.clear_depth && !submitted.data.clear.clear_stencil);
    arguments[0] = 0u; arguments[1] = DEPTH_HEADER;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    memcpy(recomp_memory(TILE_SOURCE, sizeof tile), &tile, sizeof tile);
    arguments[0] = 7u; arguments[1] = TILE_SOURCE;
    passed &= call(0x3459e0u, arguments, 2u, NULL);
    passed &= expect("tile setter normalizes six DWORDs and retires two arguments",
        recomp_runtime.registers.eax == EAX_SENTINEL &&
        recomp_runtime.registers.esp == ENTRY_ESP + 12u &&
        *recomp_memory_u32(DEVICE + 0x1ac0u + 7u * 0x18u) == 1u &&
        *recomp_memory_u32(DEVICE + 0x1ac0u + 7u * 0x18u + 16u) == 0u);
    arguments[1] = 0u;
    passed &= call(0x3459e0u, arguments, 2u, NULL);
    passed &= expect("null tile clears entry",
        *recomp_memory_u32(DEVICE + 0x1ac0u + 7u * 0x18u) == 0u);
    arguments[0] = 8u;
    passed &= call(0x3459e0u, arguments, 2u, "d3d8:tile-index");
    *recomp_memory_u32(COLOR_HEADER + 8u) = 0x11111111u;
    *recomp_memory_u32(COLOR_HEADER + 0x18u + 8u) = 0x22222222u;
    memcpy(before, recomp_memory(COLOR_HEADER, sizeof before), sizeof before);
    *recomp_memory_u32(DEVICE + 0x2408u) = 1u;
    arguments[0] = 0u;
    passed &= call(0x34af80u, arguments, 1u, NULL);
    memcpy(after, recomp_memory(COLOR_HEADER, sizeof after), sizeof after);
    passed &= expect("swap ABI, counter, pacing and physical storage rotation",
        recomp_runtime.registers.eax == 1u &&
        recomp_runtime.registers.esp == ENTRY_ESP + 8u &&
        *recomp_memory_u32(DEVICE + 0x2478u) == 1u && wait_present_count == 1u &&
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_PRESENT &&
        submitted.data.present.effective_flags == 5u &&
        submitted.data.present.swap_counter == 1u &&
        after[0].data == before[1].data && after[1].data == before[0].data &&
        after[0].lock == before[1].lock && after[1].lock == before[0].lock &&
        after[0].common == before[0].common && after[1].common == before[1].common);
    arguments[0] = 1u;
    prior_submits = submit_count;
    passed &= call(0x34af80u, arguments, 1u, "d3d8:swap");
    passed &= expect("unsupported swap has no presenter or guest counter effect",
        submit_count == prior_submits && *recomp_memory_u32(DEVICE + 0x2478u) == 1u);
    passed &= expect("first swap consumes only bank zero",
        *recomp_memory_u32(DEVICE + 0x2404u) == 0u &&
        *recomp_memory_u32(DEVICE + 0x2408u) == 1u &&
        *recomp_memory_u32(DEVICE + 0x1de4u) == 1u);
    arguments[0] = 0u;
    passed &= call(0x34af80u, arguments, 1u, NULL);
    passed &= expect("second swap consumes bank one",
        *recomp_memory_u32(DEVICE + 0x2408u) == 0u &&
        *recomp_memory_u32(DEVICE + 0x1de4u) == 2u &&
        recomp_runtime.registers.eax == 2u);
    passed &= call(0x3444c0u, arguments, 0u, NULL);
    passed &= expect("vblank API waits without retail hardware event",
        wait_vblank_count == 1u && recomp_runtime.registers.esp == ENTRY_ESP + 4u &&
        recomp_runtime.registers.eax == EAX_SENTINEL);
    passed &= expect("vblank tail aliases bind the same API",
        recomp_d3d8_lookup_manual(0x2fc300u) == recomp_d3d8_lookup_manual(0x3444c0u) &&
        recomp_d3d8_lookup_manual(0x2fc5a0u) == recomp_d3d8_lookup_manual(0x3444c0u));
    passed &= call(0x344440u, arguments, 0u, NULL);
    passed &= expect("kick flushes without present", flush_count == 1u && !flush_wait &&
        recomp_runtime.registers.esp == ENTRY_ESP + 4u && recomp_runtime.registers.eax == EAX_SENTINEL);
    passed &= call(0x344420u, arguments, 0u, NULL);
    passed &= expect("idle waits", flush_count == 2u && flush_wait);
    passed &= call(0x345d70u, arguments, 0u, NULL);
    passed &= expect("persist keeps native display after draining", flush_count == 3u &&
        flush_wait && recomp_runtime.registers.eax == 0u);
    passed &= expect("persist publishes the SDK front-buffer copy header",
        saved_display == 0x7f0000u && *recomp_memory_u32(0x7f0000u) == 0x81050001u &&
        *recomp_memory_u32(0x7f0004u) == 0x7f0040u && *recomp_memory_u32(0x7f0008u) == 0u &&
        *recomp_memory_u32(0x7f000cu) == 0x11229u && *recomp_memory_u32(0x7f0010u) == 0x2c1df2cfu);
    passed &= call(0x345d70u, arguments, 0u, NULL);
    passed &= expect("persist frees the previous copy", freed_display == 0x7f0000u &&
        saved_display == 0x7f0000u && flush_count == 4u);
    *recomp_memory_u32(PARAMETERS + 48u) = 1u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x12345678u;
    *recomp_memory_u32(DEVICE + 0xc60u) = 0x3f400000u;
    arguments[0] = PARAMETERS;
    passed &= call(0x344720u, arguments, 1u, NULL);
    passed &= expect("reset retains native display, frame count and non-RT state",
        recomp_runtime.registers.eax == 0u && recomp_runtime.registers.esp == ENTRY_ESP + 8u &&
        create_count == 1u && destroy_count == 0u && flush_count == 5u &&
        *recomp_memory_u32(DEVICE + 0xf88u) == 0x12345678u &&
        *recomp_memory_u32(DEVICE + 0xc60u) == 0x3f400000u &&
        *recomp_memory_u32(DEVICE + 0x2478u) == 2u && *recomp_memory_u32(0x35545cu) == 1u &&
        *recomp_memory_u32(COLOR_HEADER) == 0xd0002u &&
        *recomp_memory_u32(DEPTH_HEADER) == 0xd0002u &&
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_CLEAR &&
        !submitted.data.clear.clear_color && submitted.data.clear.clear_depth && submitted.data.clear.clear_stencil);
    /* 0x2021 Reset: a 1440x480 back/depth pair filtered into a 720 front. */
    const RecompD3dSurfaceHeader wide[3] = {
        {0x50001u, 0x400000u, 0u, 0x11229u, 0x5f1df59fu, 0u},
        {0x50001u, 0x100000u, 0u, 0x11229u, 0x2f1df2cfu, 0u},
        {0x50001u, 0x520000u, 0u, 0x12e29u, 0x5f1df59fu, 0u},
    };
    *recomp_memory_u32(PARAMETERS + 16u) = 0x1021u;
    passed &= call(0x344720u, arguments, 1u, "d3d8:reset-mode");
    *recomp_memory_u32(PARAMETERS + 16u) = 0x2021u;
    passed &= call(0x344720u, arguments, 1u, "d3d8:reset-surfaces");
    memcpy(recomp_memory(COLOR_SOURCE, 2u * sizeof *wide), wide, 2u * sizeof *wide);
    memcpy(recomp_memory(DEPTH_SOURCE, sizeof *wide), &wide[2], sizeof *wide);
    passed &= call(0x344720u, arguments, 1u, NULL);
    passed &= expect("supersampled reset writes SDK mode and back-buffer scale",
        recomp_runtime.registers.eax == 0u &&
        *recomp_memory_u32(DEVICE + 0x196cu) == 0x2021u &&
        *recomp_memory_u32(DEVICE + 0x96cu) == 0x40000000u &&
        *recomp_memory_u32(DEVICE + 0x970u) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x95cu) == 0x40000000u &&
        *recomp_memory_u32(DEVICE + 0x960u) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x954u) == 1440u &&
        *recomp_memory_u32(DEVICE + 0x958u) == 480u &&
        (*recomp_memory_u32(DEVICE + 8u) & 0x4000u) != 0u &&
        *recomp_memory_u32(DEVICE + 0x94cu) == 0x1800u &&
        *recomp_memory_u32(0x3554c8u) == 0u && *recomp_memory_u32(0x355458u) == 2u &&
        *recomp_memory_u32(COLOR_HEADER + 4u) == 0x400000u &&
        *recomp_memory_u32(COLOR_HEADER + 16u) == 0x5f1df59fu);
    viewport = (RecompD3dViewport){0u, 0u, 1440u, 480u, 0.0f, 1.0f};
    memcpy(recomp_memory(VIEWPORT_OUTPUT, sizeof viewport), &viewport, sizeof viewport);
    arguments[0] = VIEWPORT_OUTPUT;
    passed &= call(0x344e10u, arguments, 1u, NULL);
    passed &= expect("supersampled viewport clamps to the front buffer",
        *recomp_memory_u32(DEVICE + 0xee8u) == 720u && *recomp_memory_u32(DEVICE + 0xeecu) == 480u);
    arguments[0] = 0u; arguments[1] = 0u; arguments[2] = 0xf3u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= expect("supersampled clear targets the logical host buffer",
        !submitted.data.clear.target.offscreen && !submitted.data.clear.target.custom_depth &&
        submitted.data.clear.target.color.width == 720u &&
        submitted.data.clear.target.depth.width == 720u &&
        submitted.data.clear.target.color.data == 0x400000u);
    memcpy(before, recomp_memory(COLOR_HEADER, sizeof before), sizeof before);
    arguments[0] = 0u;
    freed_display = 0u;
    passed &= call(0x34af80u, arguments, 1u, NULL);
    passed &= expect("supersampled swap keeps back and front storage",
        memcmp(before, recomp_memory(COLOR_HEADER, sizeof before), sizeof before) == 0);
    passed &= expect("first swap after reset frees the persisted copy (0x34a600)",
        freed_display == 0x7f0000u && saved_display == 0u);
    freed_display = 0u;
    passed &= call(0x34af80u, arguments, 1u, NULL);
    passed &= expect("later swaps leave saved data alone", freed_display == 0u);
    arguments[0] = OFFSCREEN; arguments[1] = 0u;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    passed &= expect("offscreen target drops the supersample scale",
        *recomp_memory_u32(DEVICE + 0x95cu) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x954u) == 240u);
    arguments[0] = COLOR_HEADER; arguments[1] = DEPTH_HEADER;
    passed &= call(0x3447d0u, arguments, 2u, NULL);
    passed &= expect("back buffer restores the supersample scale and logical viewport",
        *recomp_memory_u32(DEVICE + 0x95cu) == 0x40000000u &&
        *recomp_memory_u32(DEVICE + 0x954u) == 1440u &&
        *recomp_memory_u32(DEVICE + 0xee8u) == 720u);
    arguments[0] = PARAMETERS;
    *recomp_memory_u32(DEVICE + 0x2cu) = 2u;
    passed &= call(0x3445f0u, arguments, 0u, NULL);
    passed &= expect("insert returns a fence without draining", flush_count == 6u &&
        recomp_runtime.registers.eax == 2u &&
        recomp_runtime.registers.esp == ENTRY_ESP + 4u &&
        *recomp_memory_u32(DEVICE + 0x2cu) == 4u);
    passed &= call(0x3445f0u, arguments, 0u, NULL);
    passed &= expect("fences advance by two", recomp_runtime.registers.eax == 4u &&
        *recomp_memory_u32(DEVICE + 0x2cu) == 6u);
    arguments[0] = 2u;
    passed &= call(0x344430u, arguments, 1u, NULL);
    passed &= expect("fence wait returns at once and preserves void ABI", flush_count == 6u &&
        recomp_runtime.registers.eax == EAX_SENTINEL &&
        recomp_runtime.registers.esp == ENTRY_ESP + 8u);
    /* 0x276d40 brackets a draw with Begin/End(index), inserts a WRITE
       callback (0x276b50) and polls the result on a later frame. */
    arguments[0] = 0x11u;
    passed &= call(0x345860u, arguments, 1u, "d3d8:visibility-end-without-begin");
    passed &= call(0x3457c0u, arguments, 0u, NULL);
    passed &= expect("begin submits counting with the void ABI",
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN &&
        recomp_runtime.registers.esp == ENTRY_ESP + 4u && recomp_runtime.registers.eax == EAX_SENTINEL);
    arguments[0] = 0x4000u;
    passed &= call(0x345860u, arguments, 1u, "d3d8:visibility-index:00004000");
    arguments[0] = 0x11u;
    passed &= call(0x345860u, arguments, 1u, NULL);
    passed &= expect("end submits its index and pops it",
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END &&
        submitted.data.visibility_index == 0x11u &&
        recomp_runtime.registers.esp == ENTRY_ESP + 8u && recomp_runtime.registers.eax == 0u);
    arguments[0] = 1u; arguments[1] = 0x276b50u; arguments[2] = 0x11u;
    passed &= call(0x349e10u, arguments, 3u, NULL);
    passed &= expect("callback runs as cdecl with its context",
        dispatched == 0x276b50u && dispatched_context == 0x11u && dispatched_return == 0u &&
        recomp_runtime.registers.esp == ENTRY_ESP + 16u && recomp_runtime.registers.eax == EAX_SENTINEL);
    arguments[0] = 2u;
    passed &= call(0x349e10u, arguments, 3u, "d3d8:insert-callback:2");
    arguments[0] = 0x11u; arguments[1] = OUTPUT; arguments[2] = 0u;
    *recomp_memory_u32(OUTPUT) = EAX_SENTINEL;
    prior_submits = flush_count;
    passed &= call(0x344450u, arguments, 3u, NULL);
    passed &= expect("pending result kicks queued work and keeps the output",
        recomp_runtime.registers.eax == 0x88760828u && recomp_runtime.registers.esp == ENTRY_ESP + 16u &&
        visibility_queried == 0x11u && flush_count == prior_submits + 1u && !flush_wait &&
        *recomp_memory_u32(OUTPUT) == EAX_SENTINEL);
    visibility_complete = true;
    visibility_pixels = 300u;
    passed &= call(0x344450u, arguments, 3u, NULL);
    passed &= expect("complete result counts supersampled back-buffer samples",
        recomp_runtime.registers.eax == 0u && recomp_runtime.registers.esp == ENTRY_ESP + 16u &&
        *recomp_memory_u32(OUTPUT) == 600u);
    arguments[2] = OUTPUT + 4u;
    passed &= call(0x344450u, arguments, 3u, "d3d8:visibility-timestamp-unbound");
    arguments[0] = 0x12u; arguments[2] = 0u;
    passed &= call(0x344450u, arguments, 3u, "d3d8:visibility-result-not-ended:00000012");
    /* Return Reset 0x2021 -> 0x11 (0x282770): the game widens the old
       headers (0x2821a0) and persists the display before Reset. */
    passed &= call(0x345d70u, arguments, 0u, NULL);
    *recomp_memory_u32(COLOR_HEADER + 16u) = 0x5f1df5ffu;
    *recomp_memory_u32(COLOR_HEADER + 0x18u + 16u) = 0x2f1df2ffu;
    *recomp_memory_u32(DEPTH_HEADER + 16u) = 0x5f1df5ffu;
    const RecompD3dSurfaceHeader narrow[3] = {
        {0x50001u, 0x100000u, 0u, 0x11229u, 0x2f1df2cfu, 0u},
        {0x50001u, 0x400000u, 0u, 0x11229u, 0x2f1df2cfu, 0u},
        {0x50001u, 0x600000u, 0u, 0x12e29u, 0x2f1df2cfu, 0u},
    };
    memcpy(recomp_memory(COLOR_SOURCE, 2u * sizeof *narrow), narrow, 2u * sizeof *narrow);
    memcpy(recomp_memory(DEPTH_SOURCE, sizeof *narrow), &narrow[2], sizeof *narrow);
    *recomp_memory_u32(PARAMETERS + 16u) = 0x11u;
    arguments[0] = PARAMETERS;
    passed &= call(0x344720u, arguments, 1u, NULL);
    passed &= expect("return reset restores the SDK 0x11 mode and fresh headers",
        recomp_runtime.registers.eax == 0u &&
        *recomp_memory_u32(DEVICE + 0x196cu) == 0x11u &&
        *recomp_memory_u32(DEVICE + 0x96cu) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x970u) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x95cu) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x960u) == 0x3f800000u &&
        *recomp_memory_u32(DEVICE + 0x954u) == 720u &&
        *recomp_memory_u32(DEVICE + 0x958u) == 480u &&
        (*recomp_memory_u32(DEVICE + 8u) & 0x4000u) == 0u &&
        *recomp_memory_u32(DEVICE + 0x94cu) == 0xc00u &&
        *recomp_memory_u32(0x3554c8u) == 0u && *recomp_memory_u32(0x355458u) == 2u &&
        *recomp_memory_u32(DEVICE + 0x1a04u) == COLOR_HEADER &&
        *recomp_memory_u32(COLOR_HEADER + 4u) == 0x100000u &&
        *recomp_memory_u32(COLOR_HEADER + 16u) == 0x2f1df2cfu &&
        *recomp_memory_u32(COLOR_HEADER + 0x18u + 16u) == 0x2f1df2cfu &&
        *recomp_memory_u32(DEPTH_HEADER + 16u) == 0x2f1df2cfu &&
        *recomp_memory_u32(DEVICE + 0xee8u) == 720u);
    /* 0x2823f0 restores the saved display into the 720-wide back buffer. */
    const uint32_t rect[4] = {0u, 0u, 0x2cfu, 0x1dfu};
    memcpy(recomp_memory(RECT_SOURCE, sizeof rect), rect, sizeof rect);
    const uint32_t copy[5] = {saved_display, RECT_SOURCE, 1u, COLOR_HEADER, 0u};
    prior_submits = submit_count;
    passed &= call(0x344aa0u, copy, 5u, NULL);
    passed &= expect("saved-display copy is already in the host back buffer",
        recomp_runtime.registers.esp == ENTRY_ESP + 24u &&
        recomp_runtime.registers.eax == EAX_SENTINEL && submit_count == prior_submits);
    const uint32_t front_copy[5] = {saved_display, RECT_SOURCE, 1u, COLOR_HEADER + 0x18u, 0u};
    passed &= call(0x344aa0u, front_copy, 5u, "d3d8:copy-rects-unbound");
    arguments[0] = 0u; arguments[1] = 0u; arguments[2] = 0xf3u;
    passed &= call(0x347920u, arguments, 6u, NULL);
    passed &= call(0x344aa0u, copy, 5u, "d3d8:copy-rects-unbound");
    /* A shattering mirror (0x277670) copies the whole back buffer into a texture. */
    const RecompD3dSurfaceHeader shards = {0x50001u, 0x280000u, 0u, 0x11229u, 0x2f1df2cfu, 0u};
    memcpy(recomp_memory(SNAPSHOT, sizeof shards), &shards, sizeof shards);
    const uint32_t snapshot[5] = {COLOR_HEADER, 0u, 1u, SNAPSHOT, 0u};
    passed &= call(0x344aa0u, snapshot, 5u, NULL);
    passed &= expect("back buffer snapshot copies the main target into the texture",
        recomp_runtime.registers.esp == ENTRY_ESP + 24u &&
        submitted.type == RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR &&
        submitted.data.copy_destination.data == 0x280000u &&
        submitted.data.copy_destination.width == 720u);
    const uint32_t partial[5] = {COLOR_HEADER, RECT_SOURCE, 1u, SNAPSHOT, 0u};
    passed &= call(0x344aa0u, partial, 5u, "d3d8:copy-rects-unbound");
    passed &= coverage_tests();
    recomp_d3d8_shutdown();
    passed &= expect("shutdown destroys presenter once", destroy_count == 1u);
    recomp_d3d8_shutdown();
    passed &= expect("repeated shutdown is safe", destroy_count == 1u);
    return passed ? 0 : 1;
}
