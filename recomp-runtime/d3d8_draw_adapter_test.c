/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_draw_adapter.h"
#include "d3d8_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d_light_model.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL draw line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

enum { DEVICE = 0x355500u, STACK = 0x1000u, INPUT = 0x2000u,
    OUTPUT = 0x3000u, VERTICES = 0x4000u, INDICES = 0x5000u, BUFFER = 0x6000u,
    STAGES = 0x355060u };
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[RECOMP_XBOX_RAM_SIZE];
static RecompD3dRenderStateModel state;
static RecompD3dPresenterCommand submitted;
static uint16_t captured_indices[131070];
static float captured_vertices[64];
static uint32_t allocated = 0x360000u, freed, submits;
static bool fail_allocation;
static bool covered = true;
static uint32_t coverage_queries;
static jmp_buf stopped;
static char stop_reason[128];
static RecompD3dTextureDesc texture;
static bool inline_texture_active;
static RecompD3dPresenterTarget target;
void recomp_d3d8_target(RecompD3dPresenterTarget *out) { *out = target; }

void recomp_stop(int code, const char *format, ...)
{
    va_list arguments;
    (void)code;
    va_start(arguments, format);
    vsnprintf(stop_reason, sizeof stop_reason, format, arguments);
    va_end(arguments);
    longjmp(stopped, 1);
}
uint8_t *recomp_memory(uint32_t address, size_t size)
{
    address &= 0x7fffffffu;
    if (address >= sizeof ram || size > sizeof ram - address)
        recomp_stop(2, "test:guest-span");
    return ram + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void recomp_guest_load(void *to, uint32_t from, size_t size) { memcpy(to, recomp_memory(from, size), size); }
void recomp_guest_store(uint32_t to, const void *from, size_t size) { memcpy(recomp_memory(to, size), from, size); }
void recomp_guest_memcpy(uint32_t to, uint32_t from, size_t size)
{
    memcpy(recomp_memory(to, size), recomp_memory(from, size), size);
}
uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    if (fail_allocation) return 0u;
    const uint32_t result = allocated;
    allocated += size;
    return result;
}
void recomp_kernel_free_pool(uint32_t address) { freed = address; }
uint32_t recomp_d3d8_require_device(void) { return DEVICE; }
const RecompD3dRenderStateModel *recomp_d3d8_state_model(void) { return &state; }
bool recomp_d3d8_state_set_simple(uint32_t method, uint32_t value)
{
    return recomp_d3d_set_simple_render_state(&state, method, value);
}
bool recomp_d3d8_resource_describe(uint32_t resource, RecompD3dTextureDesc *out)
{
    if (resource != 0x7000u) return false;
    *out = texture;
    return true;
}
bool recomp_d3d8_resource_inline_texture(uint32_t stage, RecompD3dTextureDesc *out)
{
    if (stage != 3u || !inline_texture_active || *recomp_memory_u32(DEVICE + 0xf94u) == 0u) return false;
    *out = texture;
    return true;
}
bool recomp_d3d8_draw_has_coverage(const RecompD3dPresenterDrawCommand *draw)
{
    ++coverage_queries;
    if (draw->vertex_bytes == NULL || !draw->has_transform || draw->index_count != 3u)
        recomp_stop(2, "test:coverage-geometry");
    return covered;
}
void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    submitted = *command;
    memcpy(captured_indices, command->data.draw.index_bytes, command->data.draw.index_count * 2u);
    const size_t vertex_bytes = (size_t)command->data.draw.vertex_stride * command->data.draw.vertex_count;
    memcpy(captured_vertices, command->data.draw.vertex_bytes,
        vertex_bytes < sizeof captured_vertices ? vertex_bytes : sizeof captured_vertices);
    ++submits;
}

static uint32_t invoke(uint32_t address, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    RecompFunction function = recomp_d3d8_draw_lookup_manual(address);
    if (function == NULL) abort();
    recomp_runtime.registers.esp = STACK;
    const uint32_t args[5] = {0x12345678u, a, b, c, d};
    recomp_guest_store(STACK, args, sizeof args);
    recomp_runtime.registers.eax = 0x12345678u;
    function();
    return recomp_runtime.registers.eax;
}

static void initialize(void)
{
    memset(ram, 0, sizeof ram);
    recomp_d3d_render_state_reset(&state);
    inline_texture_active = false;
    submits = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    *recomp_memory_u32(DEVICE + 0x1a04u) = DEVICE + 0x1a24u;
    *recomp_memory_u32(DEVICE + 0x1a08u) = DEVICE + 0x1a6cu;
    *recomp_memory_u32(DEVICE + 0xee8u) = 720u;
    *recomp_memory_u32(DEVICE + 0xeecu) = 480u;
    *recomp_memory_u32(DEVICE + 0xef4u) = 0x3f800000u;
    *recomp_memory_u32(DEVICE + 0xef8u) = *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f080000u;
    for (uint32_t slot = 0u; slot < 10u; ++slot) {
        for (uint32_t i = 0u; i < 4u; ++i)
            *recomp_memory_u32(DEVICE + 0xc60u + slot * 64u + i * 20u) = 0x3f800000u;
    }
    *recomp_memory_u32(0x3579a8u) = 28u;
    *recomp_memory_u32(0x3579b0u) = BUFFER;
    *recomp_memory_u32(BUFFER + 4u) = VERTICES;
    for (uint32_t i = 0u; i < 4u; ++i) {
        const uint32_t base = STAGES + i * 128u;
        *recomp_memory_u32(base) = 1u;
        *recomp_memory_u32(base + 4u) = 1u;
        *recomp_memory_u32(base + 12u) = 2u;
        *recomp_memory_u32(base + 16u) = 2u;
        *recomp_memory_u32(base + 48u) = i == 0u ? 4u : 1u;
        *recomp_memory_u32(base + 56u) = 2u;
        *recomp_memory_u32(base + 60u) = 0u;
        *recomp_memory_u32(base + 64u) = 4u;
        *recomp_memory_u32(base + 72u) = 2u;
        *recomp_memory_u32(base + 76u) = 0u;
    }
    texture = (RecompD3dTextureDesc){0};
    target = (RecompD3dPresenterTarget){0};
    texture.format_byte = 6u;
    texture.bits_per_pixel = 32u;
    texture.width = texture.height = 4u;
    texture.data = 0x8000u;
    texture.mip_levels = 1u;
    covered = true;
    coverage_queries = 0u;
}

static int pixel_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    CHECK(recomp_d3d8_draw_lookup_manual(0u) == NULL);
    memset(ram + INPUT, 0, 0xf0u);
    *recomp_memory_u32(INPUT + 0xe4u) = UINT32_MAX;
    *recomp_memory_u32(INPUT + 0xe8u) = UINT32_MAX;
    *recomp_memory_u32(INPUT + 0xecu) = UINT32_MAX;
    *recomp_memory_u32(INPUT + 0xd8u) = 0x1234u;
    fail_allocation = true;
    *recomp_memory_u32(OUTPUT) = 0xdeadbeefu;
    CHECK(invoke(0x349490u, INPUT, OUTPUT, 0u, 0u) == 0x8007000eu);
    CHECK(*recomp_memory_u32(OUTPUT) == 0xdeadbeefu);
    fail_allocation = false;
    CHECK(invoke(0x349490u, INPUT, OUTPUT, 0u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    const uint32_t handle = *recomp_memory_u32(OUTPUT);
    CHECK(*recomp_memory_u32(handle) == 1u && *recomp_memory_u32(handle + 4u) == 1u);
    CHECK(*recomp_memory_u32(handle + 8u) == handle + 12u);
    CHECK(memcmp(ram + handle + 12u, ram + INPUT, 0xf0u) == 0);
    invoke(0x349520u, handle, 0u, 0u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(*recomp_memory_u32(DEVICE + 0x784u) == handle);
    CHECK(*recomp_memory_u32(DEVICE + 0x790u) == 0x1234u);
    CHECK(*recomp_memory_u32(0x355480u) == 0x1234u);
    CHECK(memcmp(ram + 0x355260u, ram + INPUT, 0xe4u) == 0);
    invoke(0x349500u, OUTPUT, 0u, 0u, 0u);
    CHECK(*recomp_memory_u32(OUTPUT) == handle && recomp_runtime.registers.esp == STACK + 8u);
    *recomp_memory_u32(handle + 12u + 0xe4u) = 0xfffffff2u;
    *recomp_memory_u32(handle + 12u + 0xe8u) = 0xffffff2fu;
    *recomp_memory_u32(handle + 12u + 0xecu) = 0x12fu;
    const float color[4] = {-1.0f, 0.5f, 2.0f, 1.0f};
    recomp_guest_store(INPUT, color, sizeof color);
    invoke(0x349720u, 2u, INPUT, 1u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u);
    CHECK(*recomp_memory_u32(DEVICE + 0x8ecu) == 0xff0080ffu);
    CHECK(*recomp_memory_u32(0x355288u) == 0xff0080ffu);
    CHECK(*recomp_memory_u32(0x3552acu) == 0xff0080ffu);
    CHECK(*recomp_memory_u32(0x355310u) == 0xff0080ffu);
    uint32_t value;
    CHECK(recomp_d3d_get_simple_render_state(&state, 0x40a60u, &value) && value == 0xff0080ffu);
    if (setjmp(stopped) == 0) { invoke(0x349720u, 7u, INPUT, 2u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:pixel-constant-range") == 0);
    invoke(0x349520u, 0u, 0u, 0u, 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x784u) == 0u);
    invoke(0x3494e0u, handle, 0u, 0u, 0u);
    CHECK(freed == handle && recomp_runtime.registers.esp == STACK + 8u);
    return 0;
}

static int draw_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    CHECK(invoke(0x348240u, 5u, 3u, VERTICES, 28u) == 0x12345678u);
    CHECK(recomp_runtime.registers.esp == STACK + 20u && submits == 1u);
    CHECK(submitted.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW);
    CHECK(submitted.data.draw.fvf == 0x144u && submitted.data.draw.vertex_count == 3u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES);
    CHECK(captured_indices[0] == 0u && captured_indices[2] == 2u);
    CHECK(!submitted.data.draw.has_transform && submitted.data.draw.viewport[2] == 720.0f);
    CHECK(submitted.data.draw.screen_space_offset[0] == 0.0f && submitted.data.draw.screen_space_offset[1] == 0.0f);
    CHECK(submitted.data.draw.has_scissor && submitted.data.draw.scissor[2] == 720 &&
        submitted.data.draw.scissor[3] == 480);
    const uint32_t binding = *recomp_memory_u32(DEVICE + 0xf88u);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7100u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-texture-description:stage=0:resource=00007100:common=00000000:format=00000000:fvf=144:ret=12345678") == 0);
    *recomp_memory_u32(0x7100u) = 0x40001u;
    *recomp_memory_u32(0x710cu) = 0x02210624u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strstr(stop_reason, ":common=00040001:format=02210624:") != NULL);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    texture.depth = true;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strstr(stop_reason, "d3d8:draw-texture-description:depth:stage=0:") == stop_reason);
    texture.depth = false;
    *recomp_memory_u32(DEVICE + 0xf88u) = binding;
    const int32_t scissor[4] = {8, 12, 80, 120};
    recomp_guest_store(DEVICE + 0x1b80u, scissor, sizeof scissor);
    *recomp_memory_u32(DEVICE + 0x1c00u) = 1u;
    *recomp_memory_u32(DEVICE + 0xef8u) = 0x3d000000u;
    *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f480000u;
    target.offscreen = true;
    target.no_depth = true;
    target.color = texture;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.target.offscreen && submitted.data.draw.target.no_depth &&
        submitted.data.draw.target.color.data == texture.data);
    CHECK(submitted.data.draw.screen_space_offset[0] == -0.5f && submitted.data.draw.screen_space_offset[1] == 0.25f);
    CHECK(submitted.data.draw.has_scissor &&
        memcmp(submitted.data.draw.scissor, scissor, sizeof scissor) == 0);
    *recomp_memory_u32(DEVICE + 0x1c00u) = 0u;
    *recomp_memory_u32(DEVICE + 0xee0u) = 20u;
    *recomp_memory_u32(DEVICE + 0xee8u) = 700u;
    *recomp_memory_u32(DEVICE + 0xef8u) = *recomp_memory_u32(DEVICE + 0xefcu) = 0x3f080000u;
    target = (RecompD3dPresenterTarget){0};
    invoke(0x3483b0u, 6u, 2u, 4u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u && submits == 3u);
    CHECK(submitted.data.draw.has_scissor && submitted.data.draw.scissor[0] == 20 &&
        submitted.data.draw.scissor[2] == 720);
    *recomp_memory_u32(DEVICE + 0xee0u) = 0u;
    *recomp_memory_u32(DEVICE + 0xee8u) = 720u;
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES + 56u);
    CHECK(submitted.data.draw.triangle_count == 2u);
    const uint16_t indices[3] = {4u, 0u, 2u};
    recomp_guest_store(INDICES, indices, sizeof indices);
    *recomp_memory_u32(DEVICE + 0x1cu) = 2u;
    invoke(0x348450u, 5u, 3u, INDICES, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u && submits == 4u);
    CHECK(submitted.data.draw.vertex_count == 5u && captured_indices[0] == 4u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES + 56u);
    const uint16_t deep[3] = {6u, 4u, 5u};
    recomp_guest_store(INDICES, deep, sizeof deep);
    invoke(0x348450u, 5u, 3u, INDICES, 0u);
    CHECK(submitted.data.draw.vertex_count == 3u && captured_indices[0] == 2u &&
        captured_indices[1] == 0u && captured_indices[2] == 1u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES + 56u + 4u * submitted.data.draw.vertex_stride);
    CHECK(memcmp(ram + INDICES, deep, sizeof deep) == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 2u;
    const float translate = 3.5f;
    recomp_guest_store(DEVICE + 0xde0u + 48u, &translate, sizeof translate);
    invoke(0x348240u, 5u, 3u, VERTICES, 12u);
    CHECK(submitted.data.draw.has_transform && submitted.data.draw.transform[12] == 3.5f);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.has_texture && submitted.data.draw.texture_bytes == ram + 0x8000u);
    CHECK(submitted.data.draw.texture_byte_count == 64u);
    CHECK(submitted.data.draw.address_u == 1u && submitted.data.draw.address_v == 1u);
    texture.mip_levels = 3u;
    *recomp_memory_u32(STAGES + 20u) = 1u;
    *recomp_memory_u32(STAGES + 24u) = 0xc0000000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    /* -2.0 * 256 + 0.5 truncates toward zero to -511. */
    CHECK(submitted.data.draw.point_mip_filter && submitted.data.draw.mip_lod_bias == -511.0f / 256.0f);
    CHECK(!submitted.data.draw.alpha_kill);
    *recomp_memory_u32(STAGES + 44u) = 4u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.alpha_kill);
    *recomp_memory_u32(STAGES + 44u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-sampler:", 26) == 0);
    *recomp_memory_u32(STAGES + 44u) = 0u;
    *recomp_memory_u32(STAGES + 64u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE);
    *recomp_memory_u32(STAGES + 72u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE);
    *recomp_memory_u32(STAGES + 72u) = 2u;
    *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(STAGES + 24u) = 0x7f800000u;
    /* 0x34f400 clamps the 13-bit fixed-point bias; infinity converts to INT_MIN. */
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.mip_lod_bias == -16.0f);
    *recomp_memory_u32(STAGES + 24u) = 0x42c80000u; /* 100.0 */
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.mip_lod_bias == 15.99f);
    *recomp_memory_u32(STAGES + 24u) = 0xbb800000u; /* -1/256 truncates to 0 after +0.5 */
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.mip_lod_bias == 0.0f);
    texture.mip_levels = 1u;
    *recomp_memory_u32(STAGES + 24u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    *recomp_memory_u32(STAGES + 64u) = 3u;
    invoke(0x348240u, 6u, 4u, VERTICES, 24u);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE &&
        submitted.data.draw.material_alpha == 1.0f);
    *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    const uint32_t before = submits;
    invoke(0x348240u, 5u, 0u, VERTICES, 28u);
    CHECK(submits == before);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 3u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-primitive", 19u) == 0);
    const uint32_t quads = submits;
    invoke(0x348240u, 9u, 5u, VERTICES, 28u);
    CHECK(submits == quads + 1u && submitted.data.draw.primitive_type == 6u &&
        submitted.data.draw.index_count == 4u && submitted.data.draw.triangle_count == 2u);
    invoke(0x348240u, 2u, 5u, VERTICES, 28u);
    CHECK(submits == quads + 2u && submitted.data.draw.primitive_type == 2u &&
        submitted.data.draw.index_count == 4u && submitted.data.draw.triangle_count == 2u);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected overlapping line stop: %s\n", stop_reason); return 1; }
    invoke(0x348240u, 2u, 2u, VERTICES, 20u);
    CHECK(submits == quads + 3u && submitted.data.draw.fvf == 0x144u &&
        submitted.data.draw.vertex_stride == 28u && submitted.data.draw.has_texture);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0u;
    invoke(0x348240u, 2u, 2u, VERTICES, 20u);
    CHECK(submits == quads + 4u && submitted.data.draw.fvf == 0x44u && !submitted.data.draw.has_texture);
    /* Width 2 widens an x-major line by one pixel above and below. */
    const float line[10] = {10.0f, 20.0f, 0.0f, 1.0f, 0.0f, 50.0f, 24.0f, 0.0f, 1.0f, 0.0f};
    const float wide = 2.0f;
    recomp_guest_store(0x4800u, line, sizeof line);
    recomp_guest_store(0x3554d4u, &wide, sizeof wide);
    invoke(0x348240u, 2u, 2u, 0x4800u, 20u);
    CHECK(submitted.data.draw.primitive_type == 5u && submitted.data.draw.index_count == 6u &&
        submitted.data.draw.vertex_count == 4u && submitted.data.draw.triangle_count == 2u &&
        submitted.data.draw.cull_mode == RECOMP_D3D_CULL_NONE);
    CHECK(captured_indices[2] == 3u && captured_indices[5] == 2u);
    CHECK(captured_vertices[1] == 19.0f && captured_vertices[6] == 21.0f &&
        captured_vertices[10] == 50.0f && captured_vertices[11] == 23.0f && captured_vertices[16] == 25.0f);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 2u, 2u, VERTICES, 24u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-wide-lines:1") == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    recomp_guest_store(0x3554d4u, &(float){1.0f}, sizeof(float));
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 65537u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-span") == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x1002u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-vertex-layout", 23u) == 0);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    *recomp_memory_u32(DEVICE + 0x784u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    return 0;
}

static int line_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected line stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0x798u) = 0x42u;
    CHECK(recomp_d3d_set_edge_antialias(&state, 1u));
    const float widths[4] = {0.3f, 0.6f, 0.9f, 1.2f};
    const float quantized[4] = {0.25f, 0.625f, 0.875f, 1.25f};
    for (uint32_t i = 0u; i < 4u; ++i) {
        recomp_guest_store(0x3554d4u, &widths[i], sizeof(float));
        invoke(0x348240u, 2u, 2u, VERTICES, 16u);
        CHECK(submitted.data.draw.primitive_type == 2u && submitted.data.draw.line_antialias &&
            submitted.data.draw.line_width == quantized[i] && submitted.data.draw.has_transform &&
            submitted.data.draw.fvf == 0x42u && captured_indices[1] == 1u);
    }
    const uint16_t strip[6] = {0u, 1u, 1u, 2u, 2u, 3u};
    invoke(0x348240u, 4u, 4u, VERTICES, 16u);
    CHECK(submitted.data.draw.primitive_type == 2u && submitted.data.draw.index_count == 6u &&
        submitted.data.draw.vertex_count == 4u && submitted.data.draw.triangle_count == 3u &&
        submitted.data.draw.line_antialias && submitted.data.draw.line_width == 1.25f);
    CHECK(memcmp(captured_indices, strip, sizeof strip) == 0);
    CHECK(recomp_runtime.registers.esp == STACK + 20u);
    const uint16_t source[4] = {3u, 0u, 2u, 1u}, indexed[6] = {3u, 0u, 0u, 2u, 2u, 1u};
    recomp_guest_store(INDICES, source, sizeof source);
    *recomp_memory_u32(0x3579a8u) = 16u;
    invoke(0x348450u, 4u, 4u, INDICES, 0u);
    CHECK(submitted.data.draw.vertex_count == 4u && submitted.data.draw.index_count == 6u &&
        memcmp(captured_indices, indexed, sizeof indexed) == 0);
    CHECK(memcmp(ram + INDICES, source, sizeof source) == 0);
    invoke(0x3483b0u, 4u, 2u, 4u, 0u);
    CHECK(submitted.data.draw.vertex_bytes == ram + VERTICES + 32u &&
        memcmp(captured_indices, strip, sizeof strip) == 0);
    const uint32_t before = submits;
    invoke(0x348240u, 4u, 0u, 0u, 0u);
    invoke(0x348240u, 4u, 1u, VERTICES, 16u);
    CHECK(submits == before);
    invoke(0x348240u, 4u, 2u, VERTICES, 16u);
    CHECK(submits == before + 1u && submitted.data.draw.index_count == 2u && captured_indices[1] == 1u);
    invoke(0x348240u, 4u, 65536u, VERTICES, 16u);
    CHECK(submitted.data.draw.vertex_count == 65536u && submitted.data.draw.index_count == 131070u &&
        captured_indices[131068] == 65534u && captured_indices[131069] == UINT16_MAX);
    CHECK(recomp_d3d_set_edge_antialias(&state, 0u));
    invoke(0x348240u, 2u, 2u, VERTICES, 16u);
    CHECK(!submitted.data.draw.line_antialias && submitted.data.draw.line_width == 1.25f);
    CHECK(recomp_d3d_set_edge_antialias(&state, 1u));
    invoke(0x348240u, 5u, 3u, VERTICES, 16u);
    CHECK(submitted.data.draw.primitive_type == 5u && !submitted.data.draw.line_antialias);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 3u, VERTICES, 16u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-raster-state:", 22u) == 0);
    return 0;
}

static int quad_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected quad stop: %s\n", stop_reason); return 1; }
    initialize();
    const uint16_t triangles[12] = {0,1,2, 0,2,3, 4,5,6, 4,6,7};
    invoke(0x348240u, 8u, 9u, VERTICES, 28u);
    CHECK(submits == 1u && submitted.data.draw.primitive_type == 5u &&
        submitted.data.draw.vertex_count == 8u && submitted.data.draw.index_count == 12u &&
        submitted.data.draw.triangle_count == 4u);
    CHECK(memcmp(captured_indices, triangles, sizeof triangles) == 0);
    const uint16_t source[9] = {6,2,5,1, 7,3,4,0, UINT16_MAX};
    const uint16_t indexed[12] = {6,2,5, 6,5,1, 7,3,4, 7,4,0};
    recomp_guest_store(INDICES, source, sizeof source);
    invoke(0x348450u, 8u, 9u, INDICES, 0u);
    CHECK(submits == 2u && submitted.data.draw.vertex_count == 8u &&
        submitted.data.draw.index_count == 12u && submitted.data.draw.triangle_count == 4u);
    CHECK(memcmp(captured_indices, indexed, sizeof indexed) == 0);
    CHECK(memcmp(ram + INDICES, source, sizeof source) == 0);
    invoke(0x3483b0u, 8u, 2u, 4u, 0u);
    CHECK(submits == 3u && submitted.data.draw.vertex_bytes == ram + VERTICES + 56u &&
        submitted.data.draw.index_count == 6u && submitted.data.draw.vertex_count == 4u);
    invoke(0x348240u, 8u, 3u, 0u, 0u);
    CHECK(submits == 3u);
    invoke(0x348240u, 8u, 65536u, VERTICES, 28u);
    CHECK(submits == 4u && submitted.data.draw.index_count == 98304u &&
        submitted.data.draw.vertex_count == 65536u && captured_indices[98303] == UINT16_MAX);
    return 0;
}

static int point_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected point stop: %s\n", stop_reason); return 1; }
    initialize();
    /* 0x2345c0's particles: XYZ|DIFFUSE points with the sprite texture on stage 3. */
    *recomp_memory_u32(DEVICE + 0x798u) = 0x42u;
    *recomp_memory_u32(DEVICE + 0xf94u) = 0x7000u;
    *recomp_memory_u32(STAGES + 3u * 128u + 48u) = 2u;
    const float projection[16] = {2,0,0,0, 0,2,0,0, 0,0,1,1, 0,0,-1,0};
    recomp_guest_store(DEVICE + 0xca0u, projection, sizeof projection);
    const float size[2] = {0.1f, 1.0f}, scale[4] = {1.0f, 0.0f, 0.0f, 32.0f};
    recomp_guest_store(0x355430u, size, sizeof size);
    recomp_guest_store(0x355440u, scale, sizeof scale);
    *recomp_memory_u32(0x355438u) = *recomp_memory_u32(0x35543cu) = 1u;
    const struct { float x, y, z; uint32_t diffuse; } vertex = {0.0f, 0.0f, 10.0f, 0x80123456u};
    recomp_guest_store(VERTICES, &vertex, sizeof vertex);
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(draw->primitive_type == 5u && draw->index_count == 6u && draw->vertex_count == 4u &&
        draw->fvf == 0x142u && draw->vertex_stride == 24u && draw->has_texture &&
        draw->cull_mode == RECOMP_D3D_CULL_NONE && draw->transform[11] == 1.0f);
    CHECK(captured_indices[0] == 0u && captured_indices[4] == 1u && captured_indices[5] == 3u);
    /* 480 * 0.1 clamps to MAX 32 pixels: +-32*10/(2*720) eye units across, +-32*10/(2*480) up. */
    uint32_t diffuse;
    memcpy(&diffuse, &captured_vertices[3], sizeof diffuse);
    CHECK(fabsf(captured_vertices[0] + 2.0f / 9.0f) < 1e-6f && fabsf(captured_vertices[1] - 1.0f / 3.0f) < 1e-6f &&
        captured_vertices[2] == 10.0f && captured_vertices[4] == 0.0f && captured_vertices[5] == 0.0f);
    CHECK(fabsf(captured_vertices[18] - 2.0f / 9.0f) < 1e-6f && fabsf(captured_vertices[19] + 1.0f / 3.0f) < 1e-6f &&
        captured_vertices[22] == 1.0f && captured_vertices[23] == 1.0f);
    CHECK(diffuse == 0x80ffffffu);
    /* Direct stage-3 writes replace sampling data without mutating the API resource. */
    const uint32_t saved_data = texture.data;
    texture.data = 0x11000u;
    inline_texture_active = true;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    CHECK(draw->has_texture && draw->texture.data == 0x11000u && draw->texture_bytes == ram + 0x11000u);
    CHECK(*recomp_memory_u32(DEVICE + 0xf94u) == 0x7000u);
    inline_texture_active = false;
    texture.data = saved_data;
    /* 0x1f5030 leaves LIGHTING on for texture-RGB/material-alpha point sprites. */
    *recomp_memory_u32(0x3553f8u) = *recomp_memory_u32(0x355404u) = 1u;
    *recomp_memory_u32(0x355408u) = *recomp_memory_u32(0x355418u) = 2u;
    *recomp_memory_u32(0x35540cu) = *recomp_memory_u32(0x35541cu) = 1u;
    const float material_alpha = 0.25f;
    recomp_guest_store(DEVICE + 0xf0cu, &material_alpha, sizeof material_alpha);
    /* Unused lighting RGB data must not affect this material or trigger guards. */
    *recomp_memory_u32(DEVICE + 0xf00u) = 0x7fc00000u;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    memcpy(&diffuse, &captured_vertices[3], sizeof diffuse);
    CHECK(!draw->directional.enabled && diffuse == 0xffffffffu &&
        draw->material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE &&
        draw->material_alpha == 0.25f);
    CHECK(recomp_d3d_set_texture_factor(&state, 0x80402010u));
    for (uint32_t channels = 3u; channels != 0u; --channels) {
        const uint32_t sprite = STAGES + 3u * 128u;
        *recomp_memory_u32(sprite + 48u) = (channels & 1u) ? 4u : 2u;
        *recomp_memory_u32(sprite + 60u) = (channels & 1u) ? 3u : 0u;
        *recomp_memory_u32(sprite + 76u) = (channels & 2u) ? 3u : 0u;
        invoke(0x348240u, 1u, 1u, VERTICES, 16u);
        CHECK(draw->directional.stage_mode && !draw->directional.enabled &&
            draw->texture_factor == 0x80402010u && draw->directional.stage[0] == 1.0f &&
            draw->directional.stage[1] == ((channels & 1u) ? 2.0f : 0.0f) &&
            draw->directional.stage[3] == ((channels & 2u) ? 2.0f : 1.0f));
        CHECK(draw->material_alpha_mode == ((channels & 2u) ? RECOMP_D3D_MATERIAL_ALPHA_NONE
            : RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE));
    }
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) =
        *recomp_memory_u32(STAGES + 3u * 128u + 72u) = 3u;
    *recomp_memory_u32(STAGES + 3u * 128u + 60u) =
        *recomp_memory_u32(STAGES + 3u * 128u + 76u) = 2u;
    *recomp_memory_u32(DEVICE + 0xf0cu) = 0x7f800000u;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    CHECK(draw->directional.stage[1] == 2.0f && draw->directional.stage[3] == 2.0f);
    *recomp_memory_u32(0x3553f8u) = 0u;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    CHECK(draw->directional.stage_mode && draw->material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE);
    *recomp_memory_u32(0x3553f8u) = 1u;
    *recomp_memory_u32(STAGES + 3u * 128u + 48u) = 2u;
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) =
        *recomp_memory_u32(STAGES + 3u * 128u + 72u) = 2u;
    *recomp_memory_u32(STAGES + 3u * 128u + 60u) =
        *recomp_memory_u32(STAGES + 3u * 128u + 76u) = 0u;
    const uint32_t nan_alpha[2] = {0x7fc00000u, 0xffc00000u};
    for (uint32_t i = 0u; i < 2u; ++i) {
        *recomp_memory_u32(DEVICE + 0xf0cu) = nan_alpha[i];
        invoke(0x348240u, 1u, 1u, VERTICES, 16u);
        CHECK(draw->material_alpha == (i == 0u ? 1.0f : 0.0f));
    }
    *recomp_memory_u32(STAGES + 3u * 128u + 64u) = 2u;
    *recomp_memory_u32(DEVICE + 0xf0cu) = 0x7f800000u;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    CHECK(draw->material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE);
    *recomp_memory_u32(STAGES + 3u * 128u + 64u) = 4u;
    recomp_guest_store(DEVICE + 0xf0cu, &material_alpha, sizeof material_alpha);
    *recomp_memory_u32(0x355494u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:point-sprite-lighting") == 0);
    *recomp_memory_u32(0x355494u) = 0u;
    *recomp_memory_u32(DEVICE + 0xf0cu) = 0x7f800000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:point-sprite-material-alpha") == 0);
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:point-sprite-lighting") == 0);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected unlit point stop: %s\n", stop_reason); return 1; }
    *recomp_memory_u32(0x3553f8u) = 0u;
    *recomp_memory_u32(STAGES + 3u * 128u + 64u) = 4u;
    *recomp_memory_u32(STAGES + 3u * 128u + 72u) = 2u;
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) = 0u;
    invoke(0x348240u, 1u, 1u, VERTICES, 16u);
    memcpy(&diffuse, &captured_vertices[3], sizeof diffuse);
    CHECK(draw->texture_alpha_only && diffuse == 0x80123456u);
    *recomp_memory_u32(STAGES + 3u * 128u + 56u) = 2u;
    *recomp_memory_u32(0x35543cu) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:draw-points-unbound") == 0);
    *recomp_memory_u32(0x35543cu) = 1u;
    *recomp_memory_u32(STAGES + 3u * 128u + 48u) = 7u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 1u, 1u, VERTICES, 16u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:point-sprite-stage:", 24u) == 0);
    return 0;
}

static int lighting_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected lighting stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(0x355368u) = 0xdeadbeefu; /* Inline Simple calls do not maintain this shadow. */
    *recomp_memory_u32(0x3553f8u) = 1u;
    *recomp_memory_u32(0x3553fcu) = 1u;
    *recomp_memory_u32(0x355404u) = 1u;
    *recomp_memory_u32(0x35541cu) = 1u; /* No FVF color: source falls back to material. */
    *recomp_memory_u32(0x35542cu) = 0xff804020u;
    *recomp_memory_u32(0x355498u) = 1u;
    const float material[17] = {0.5f, 0.75f, 1.0f, 0.25f, 0.2f, 0.4f, 0.6f, 1.0f,
        0.1f, 0.2f, 0.3f, 1.0f, 0.05f, 0.1f, 0.15f, 0.0f, 16.0f};
    recomp_guest_store(DEVICE + 0xf00u, material, sizeof material);
    const uint32_t lights = 0x9000u;
    *recomp_memory_u32(DEVICE + 0x7a4u) = lights;
    *recomp_memory_u32(DEVICE + 0x7a8u) = 4u;
    *recomp_memory_u32(DEVICE + 0x7acu) = lights;
    *recomp_memory_u32(DEVICE + 0x7acu + 12u) = lights + 3u * sizeof(RecompD3dLightRecord);
    RecompD3dLightRecord directional = {0}, point = {0};
    directional.desc.type = 3u;
    directional.desc.diffuse[0] = 1.0f;
    directional.desc.specular[1] = 0.5f;
    directional.desc.ambient[2] = 0.25f;
    directional.normalized_direction[2] = 1.0f;
    directional.slot = 0u;
    point.desc.type = 1u;
    point.desc.diffuse[1] = 0.5f;
    point.desc.position[0] = 2.0f;
    point.desc.position[1] = 3.0f;
    point.desc.position[2] = 4.0f;
    point.desc.range = 20.0f;
    point.desc.attenuation[0] = 1.0f;
    point.desc.attenuation[1] = 0.5f;
    point.slot = 3u;
    recomp_guest_store(lights, &directional, sizeof directional);
    recomp_guest_store(lights + 3u * sizeof point, &point, sizeof point);
    const float world[16] = {2,0,0,0, 0,3,0,0, 0,0,4,0, 5,6,7,1};
    recomp_guest_store(DEVICE + 0xde0u, world, sizeof world);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    const RecompD3dDirectionalLighting *lit = &submitted.data.draw.directional;
    CHECK(lit->enabled && lit->normalize && lit->stage_mode && lit->count == 2u);
    CHECK(lit->directions[0][2] == 1.0f && lit->positions[0][3] == 0.0f);
    CHECK(lit->positions[1][0] == 2.0f && lit->positions[1][3] == 1.0f);
    CHECK(lit->attenuation[1][1] == 0.5f && lit->attenuation[1][3] == 20.0f);
    CHECK(lit->material_diffuse[3] == 0.25f && lit->specular[0][1] == 0.1f);
    CHECK(lit->ambient[0][2] == 0.15f && lit->ambient_emissive[0] > 0.15f &&
        lit->ambient_emissive[0] < 0.151f);
    CHECK(lit->normal_transforms[0][0] == 0.5f && lit->normal_transforms[0][10] == 0.25f);
    CHECK(lit->world_transforms[0][12] == 5.0f && lit->view[2] == -1.0f && lit->view[3] == 16.0f);
    CHECK(lit->stage[0] == 1.0f && lit->stage[1] == 1.0f && lit->stage[2] == 1.0f && lit->stage[3] == 1.0f);
    /* SDK 0x34ff80 uploads direction, not position, for a directional light. */
    directional.desc.position[0] = NAN;
    recomp_guest_store(lights, &directional, sizeof directional);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->positions[0][0] == 0.0f);
    directional.desc.position[0] = 0.0f;
    recomp_guest_store(lights, &directional, sizeof directional);
    const uint32_t ignored[2][16] = {
        {3,7,11,12,13,14,15,16,17,18,19,20,21,22,23,24},
        {3,7,11,15,16,17,19,23,24}
    };
    for (uint32_t kind = 0u; kind < 2u; ++kind) {
        const uint32_t address = lights + (kind == 0u ? 0u : 3u * sizeof point);
        const uint32_t count = kind == 0u ? 16u : 9u;
        for (uint32_t k = 0u; k < count; ++k) {
            const uint32_t field = address + 4u + ignored[kind][k] * 4u;
            const uint32_t saved = *recomp_memory_u32(field);
            *recomp_memory_u32(field) = 0x7fc00000u;
            invoke(0x348240u, 5u, 3u, VERTICES, 32u);
            CHECK(lit->enabled && lit->colors[0][0] == 1.0f && lit->positions[1][0] == 2.0f &&
                lit->directions[0][2] == 1.0f && lit->attenuation[0][3] == 0.0f);
            *recomp_memory_u32(field) = saved;
        }
    }
    point.normalized_direction[0] = NAN;
    recomp_guest_store(lights + 3u * sizeof point, &point, sizeof point);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->directions[1][0] == 0.0f);
    point.normalized_direction[0] = 0.0f;
    recomp_guest_store(lights + 3u * sizeof point, &point, sizeof point);
    *recomp_memory_u32(0x3553fcu) = 0u;
    directional.desc.specular[1] = NAN;
    recomp_guest_store(lights, &directional, sizeof directional);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->specular[0][1] == 0.0f);
    *recomp_memory_u32(0x3553fcu) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strstr(stop_reason, ":slot=0:type=3:field=5:bits=7fc00000:") != NULL);
    directional.desc.specular[1] = 0.5f;
    recomp_guest_store(lights, &directional, sizeof directional);
    point.desc.position[0] = NAN;
    recomp_guest_store(lights + 3u * sizeof point, &point, sizeof point);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strstr(stop_reason, ":slot=3:type=1:field=12:bits=7fc00000:") != NULL);
    point.desc.position[0] = 2.0f;
    recomp_guest_store(lights + 3u * sizeof point, &point, sizeof point);
    directional.normalized_direction[2] = NAN;
    recomp_guest_store(lights, &directional, sizeof directional);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-direction") == 0);
    directional.normalized_direction[2] = 1.0f;
    recomp_guest_store(lights, &directional, sizeof directional);
    /* A disabled first color op ends both channels, even with a texture bound. */
    *recomp_memory_u32(STAGES + 48u) = *recomp_memory_u32(STAGES + 64u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && submitted.data.draw.has_texture && !submitted.data.draw.has_reflection);
    CHECK(lit->stage[0] == 0.0f && lit->stage[1] == 1.0f &&
        lit->stage[2] == 0.0f && lit->stage[3] == 1.0f);
    CHECK(submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE);
    /* ALPHAOP, its arguments and later combiners are not read; sampling still runs. */
    *recomp_memory_u32(STAGES + 64u) = 2u;
    *recomp_memory_u32(STAGES + 72u) = 3u;
    *recomp_memory_u32(STAGES + 128u + 48u) = 99u;
    *recomp_memory_u32(STAGES + 44u) = 4u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && submitted.data.draw.has_texture && submitted.data.draw.alpha_kill &&
        !submitted.data.draw.use_texture_factor && !submitted.data.draw.modulate_texture_factor_alpha);
    *recomp_memory_u32(0x3553f8u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(!lit->enabled && lit->stage_mode && lit->stage[1] == 1.0f && lit->stage[3] == 1.0f);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(0x3553f8u) = 1u;
    *recomp_memory_u32(STAGES + 44u) = 0u;
    *recomp_memory_u32(STAGES + 48u) = *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(STAGES + 72u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 48u) = 1u;
    /* Stage-0 CURRENT reads diffuse (MODULATE(TEXTURE, CURRENT) in a fight). */
    *recomp_memory_u32(STAGES + 15u * 4u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->stage[0] == 1.0f && lit->stage[1] == 1.0f);
    *recomp_memory_u32(STAGES + 15u * 4u) = 0u;
    /* Lit stage 0 can multiply texture by TFACTOR independently in RGB/alpha. */
    CHECK(recomp_d3d_set_texture_factor(&state, 0x804080c0u));
    for (uint32_t channels = 1u; channels < 4u; ++channels) {
        *recomp_memory_u32(STAGES + 15u * 4u) = (channels & 1u) ? 3u : 0u;
        *recomp_memory_u32(STAGES + 19u * 4u) = (channels & 2u) ? 3u : 0u;
        invoke(0x348240u, 5u, 3u, VERTICES, 32u);
        CHECK(lit->enabled && lit->stage[0] == 1.0f && lit->stage[2] == 1.0f &&
            lit->stage[1] == ((channels & 1u) ? 2.0f : 1.0f) &&
            lit->stage[3] == ((channels & 2u) ? 2.0f : 1.0f));
        CHECK(submitted.data.draw.texture_factor == 0x804080c0u &&
            !submitted.data.draw.texture_factor_modulate && !submitted.data.draw.modulate_texture_factor_alpha);
    }
    /* 4,2,3/3,2,3 selects TFACTOR alpha without texture or lit alpha. */
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected factor select stop: %s\n", stop_reason); return 1; }
    for (uint32_t reverse = 0u; reverse < 2u; ++reverse) {
        *recomp_memory_u32(STAGES + 14u * 4u) = reverse ? 3u : 2u;
        *recomp_memory_u32(STAGES + 15u * 4u) = reverse ? 2u : 3u;
        *recomp_memory_u32(STAGES + 16u * 4u) = reverse ? 2u : 3u;
        *recomp_memory_u32(STAGES + 18u * 4u) = reverse ? 3u : 2u;
        *recomp_memory_u32(STAGES + 19u * 4u) = reverse ? 2u : 3u;
        invoke(0x348240u, 5u, 3u, VERTICES, 32u);
        CHECK(lit->enabled && lit->stage_mode && lit->stage[0] == 1.0f && lit->stage[1] == 2.0f &&
            lit->stage[2] == 0.0f && lit->stage[3] == 2.0f);
        CHECK(submitted.data.draw.texture_factor == 0x804080c0u &&
            submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE &&
            !submitted.data.draw.texture_factor_modulate && !submitted.data.draw.modulate_texture_factor_alpha);
    }
    /* TFACTOR times diffuse needs another product input and remains unsupported. */
    *recomp_memory_u32(STAGES + 16u * 4u) = 4u;
    *recomp_memory_u32(STAGES + 19u * 4u) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:lighting-stage:", 19u) == 0);
    /* Multiplication accepts either argument order. */
    *recomp_memory_u32(STAGES + 14u * 4u) = *recomp_memory_u32(STAGES + 18u * 4u) = 3u;
    *recomp_memory_u32(STAGES + 15u * 4u) = *recomp_memory_u32(STAGES + 19u * 4u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->stage[1] == 2.0f && lit->stage[3] == 2.0f);
    *recomp_memory_u32(STAGES + 14u * 4u) = *recomp_memory_u32(STAGES + 18u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 15u * 4u) = *recomp_memory_u32(STAGES + 19u * 4u) = 0u;
    CHECK(recomp_d3d8_state_set_simple(0x4037cu, 0x1d00u));
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-shade-mode") == 0);
    CHECK(recomp_d3d8_state_set_simple(0x4037cu, 0x1d01u));
    const float flat[16] = {0,0,0,0, 0,-2,0,0, 0,0,0,0, 5,6,7,1};
    recomp_guest_store(DEVICE + 0xde0u, flat, sizeof flat);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->world_transforms[0][5] == -2.0f && lit->normal_transforms[0][5] == 0.0f);
    recomp_guest_store(DEVICE + 0xde0u, world, sizeof world);
    uint32_t saved_stages[4][32];
    recomp_guest_load(saved_stages, STAGES, sizeof saved_stages);
    const uint32_t reflection[3][6] = {{2,2,1,4,2,0}, {14,2,1,14,2,1}, {4,0,1,4,0,1}};
    const uint32_t offsets[6] = {12,14,15,16,18,19};
    for (uint32_t stage = 0u; stage < 3u; ++stage)
        for (uint32_t i = 0u; i < 6u; ++i)
            *recomp_memory_u32(STAGES + stage * 128u + offsets[i] * 4u) = reflection[stage][i];
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0x30000u;
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && !submitted.data.draw.reflection_mesh_uv &&
        !submitted.data.draw.reflection_premultiplied);
    CHECK(submitted.data.draw.reflection_bytes == ram + texture.data &&
        submitted.data.draw.reflection_byte_count == 64u);
    CHECK(!submitted.data.draw.reflection_skip_diffuse);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected reflection kill stop: %s\n", stop_reason); return 1; }
    for (uint32_t kills = 1u; kills <= 3u; ++kills) {
        *recomp_memory_u32(STAGES + 11u * 4u) = (kills & 1u) ? 4u : 0u;
        *recomp_memory_u32(STAGES + 128u + 11u * 4u) = (kills & 2u) ? 4u : 0u;
        invoke(0x348240u, 5u, 3u, VERTICES, 32u);
        CHECK(submitted.data.draw.has_reflection && submitted.data.draw.alpha_kill == ((kills & 1u) != 0u) &&
            submitted.data.draw.reflection_alpha_kill == ((kills & 2u) != 0u));
    }
    *recomp_memory_u32(STAGES + 11u * 4u) = *recomp_memory_u32(STAGES + 128u + 11u * 4u) = 0u;
    /* A second environment blend precedes the final diffuse stage. */
    uint32_t third_stages[4][32];
    recomp_guest_load(third_stages, STAGES, sizeof third_stages);
    memcpy(third_stages[3], third_stages[2], sizeof third_stages[3]);
    memcpy(third_stages[2], third_stages[1], sizeof third_stages[2]);
    third_stages[2][0] = third_stages[2][1] = 3u;
    third_stages[2][5] = 1u;
    third_stages[2][6] = 0xc0000000u;
    recomp_guest_store(STAGES, third_stages, sizeof third_stages);
    *recomp_memory_u32(DEVICE + 0xf90u) = 0x7000u;
    const float texture2[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 11,0,0,1};
    recomp_guest_store(DEVICE + 0xd60u, texture2, sizeof texture2);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected third blend stop: %s\n", stop_reason); return 1; }
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_stage2 &&
        !submitted.data.draw.reflection_skip_diffuse && submitted.data.draw.reflection_stage2_bytes == ram + texture.data &&
        submitted.data.draw.reflection_stage2_byte_count == 64u &&
        submitted.data.draw.reflection_stage2_address_u == 3u && submitted.data.draw.reflection_stage2_address_v == 3u &&
        submitted.data.draw.reflection_stage2_point_mip_filter && submitted.data.draw.reflection_stage2_mip_lod_bias == -511.0f / 256.0f &&
        submitted.data.draw.reflection_stage2_transform[12] == 11.0f);
    CHECK(!submitted.data.draw.reflection_stage2_mesh_uv);
    /* Coordinate mode 0 on both blends reads mesh UV0 and never TEXTURE2. */
    for (uint32_t stage = 1u; stage <= 2u; ++stage)
        *recomp_memory_u32(STAGES + stage * 128u + 21u * 4u) =
            *recomp_memory_u32(STAGES + stage * 128u + 28u * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0xd60u) = 0x7fc00000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.reflection_stage2 && submitted.data.draw.reflection_stage2_mesh_uv &&
        submitted.data.draw.reflection_mesh_uv);
    recomp_guest_store(STAGES, third_stages, sizeof third_stages);
    recomp_guest_store(DEVICE + 0xd60u, texture2, sizeof texture2);
    *recomp_memory_u32(STAGES + 256u + 7u * 4u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:reflection-stage2-sampler") == 0);
    *recomp_memory_u32(STAGES + 256u + 7u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 256u + 28u * 4u) = 0x20000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected restored reflection stop: %s\n", stop_reason); return 1; }
    memcpy(third_stages[2], third_stages[3], sizeof third_stages[2]);
    third_stages[3][12] = 1u;
    recomp_guest_store(STAGES, third_stages, sizeof third_stages);
    *recomp_memory_u32(DEVICE + 0xf90u) = 0u;
    /* FVF XYZB1|NORMAL|TEX1 also reaches the generated environment stage. */
    *recomp_memory_u32(DEVICE + 0x798u) = 0x116u;
    *recomp_memory_u32(0x355484u) = 1u;
    *recomp_memory_u32(0x355498u) = 0u;
    const float reflection_world1[16] = {1,0,0,0, 0,2,0,0, 0,0,1,0, 9,3,0,1};
    recomp_guest_store(DEVICE + 0xe20u, reflection_world1, sizeof reflection_world1);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected skinned reflection stop: %s\n", stop_reason); return 1; }
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.blend_weight_count == 1u);
    CHECK(submitted.data.draw.reflection_blend_world_view[0][12] == 9.0f &&
        submitted.data.draw.reflection_blend_normal[0][5] == 0.5f);
    *recomp_memory_u32(0x355498u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(submitted.data.draw.reflection_normalize &&
        submitted.data.draw.reflection_blend_normal[0][0] == 2.0f &&
        submitted.data.draw.reflection_blend_normal[0][5] == 1.0f);
    *recomp_memory_u32(0x355498u) = 0u;
    for (uint32_t weights = 2u; weights <= 3u; ++weights) {
        recomp_guest_store(DEVICE + 0xde0u + weights * 64u, reflection_world1, sizeof reflection_world1);
        *recomp_memory_u32(DEVICE + 0x798u) = 0x114u + weights * 2u;
        *recomp_memory_u32(0x355484u) = weights * 2u - 1u;
        invoke(0x348240u, 5u, 3u, VERTICES, 32u + weights * 4u);
        CHECK(submitted.data.draw.blend_weight_count == weights &&
            submitted.data.draw.reflection_blend_world_view[weights - 1u][12] == 9.0f);
    }
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(0x355484u) = 0u;
    *recomp_memory_u32(0x355498u) = 1u;
    /* Material modes 9 and 11 end with MODULATE(TEXTURE, CURRENT); without a
       stage-2 texture 0x352e46 compiles it to a CURRENT pass-through. */
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stage-2 stop: %s\n", stop_reason); return 1; }
    *recomp_memory_u32(STAGES + 256u + 14u * 4u) = *recomp_memory_u32(STAGES + 256u + 18u * 4u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_skip_diffuse);
    *recomp_memory_u32(DEVICE + 0xf90u) = 0x7000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:reflection-stage2-texture") == 0);
    *recomp_memory_u32(DEVICE + 0xf90u) = 0u;
    *recomp_memory_u32(STAGES + 256u + 14u * 4u) = *recomp_memory_u32(STAGES + 256u + 18u * 4u) = 0u;
    /* Normalized (0x355498): the adjugate of diag(2, 3, 4), undivided by det. */
    CHECK(submitted.data.draw.reflection_world_view[12] == 5.0f &&
        submitted.data.draw.reflection_normal[0] == 12.0f &&
        submitted.data.draw.reflection_transform[15] == 1.0f &&
        submitted.data.draw.reflection_diffuse[3] == 0.25f);
    /* The fight's environment stage clamps (U/V 3); its sampler follows stage 1. */
    *recomp_memory_u32(STAGES + 128u) = *recomp_memory_u32(STAGES + 132u) = 3u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_address_u == 3u &&
        submitted.data.draw.reflection_address_v == 3u);
    *recomp_memory_u32(STAGES + 128u) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    *recomp_memory_u32(STAGES + 128u) = 3u;
    *recomp_memory_u32(STAGES + 128u + 48u) = *recomp_memory_u32(STAGES + 128u + 64u) = 16u;
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected premultiplied stop: %s\n", stop_reason); return 1; }
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_premultiplied);
    *recomp_memory_u32(STAGES + 128u + 64u) = 14u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    *recomp_memory_u32(STAGES + 128u + 48u) = *recomp_memory_u32(STAGES + 128u + 64u) = 14u;
    /* Costume material mode 21 keeps CURRENT alpha at stage 1 (issue #60). */
    *recomp_memory_u32(STAGES + 128u + 64u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 72u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_color_only);
    *recomp_memory_u32(STAGES + 128u + 64u) = 14u;
    *recomp_memory_u32(STAGES + 128u + 72u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && !submitted.data.draw.reflection_color_only);
    *recomp_memory_u32(STAGES + 128u + 6u * 4u) = 0x3f800000u;
    *recomp_memory_u32(STAGES + 128u + 5u * 4u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    /* Stage 1 keeps its own mip filter and bias; stage 0 keeps its own. */
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_mip_lod_bias == 1.0f &&
        submitted.data.draw.reflection_point_mip_filter && submitted.data.draw.mip_lod_bias == 0.0f);
    *recomp_memory_u32(STAGES + 128u + 7u * 4u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:reflection-sampler:maxmip=0,1") == 0);
    *recomp_memory_u32(STAGES + 128u + 7u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 5u * 4u) = *recomp_memory_u32(STAGES + 5u * 4u);
    *recomp_memory_u32(STAGES + 128u + 6u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    *recomp_memory_u32(0x3553f8u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_mesh_uv && !lit->enabled);
    /* Generated stage-1 coordinates without a normal stream (fight FVF 0x142). */
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0x30000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.has_reflection && !submitted.data.draw.reflection_mesh_uv);
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(0x3553f8u) = 1u;
    /* Fight chrome: stage 0 generates the environment coordinates (TEXTURE0
       matrix) and stage 1 blends the mesh-UV base by its own alpha. */
    const float chrome_matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 7,0,0,1};
    recomp_guest_store(DEVICE + 0xce0u, chrome_matrix, sizeof chrome_matrix);
    *recomp_memory_u32(STAGES + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 0x30000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && submitted.data.draw.reflection_mesh_uv &&
        submitted.data.draw.texgen_reflection && submitted.data.draw.reflection_transform[12] == 7.0f);
    /* Both stages generating (fight materials 3 and 4 with coordinate mode 1):
       stage 0 reads TEXTURE0, stage 1 keeps TEXTURE1. */
    const float texture1[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 3,0,0,1};
    recomp_guest_store(DEVICE + 0xd20u, texture1, sizeof texture1);
    *recomp_memory_u32(STAGES + 128u + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0x30000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.has_reflection && !submitted.data.draw.reflection_mesh_uv &&
        submitted.data.draw.texgen_reflection && submitted.data.draw.reflection_transform[12] == 7.0f &&
        submitted.data.draw.reflection_stage1_transform[12] == 3.0f);
    /* Other stage-1 coordinate sources stay guarded. */
    *recomp_memory_u32(STAGES + 128u + 28u * 4u) = 0x10000u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-stages:", 25) == 0);
    recomp_guest_store(STAGES, saved_stages, sizeof saved_stages);
    /* Stage-0 camera-space reflection texgen (COUNT2) reads TEXTURE0. */
    const float texture0[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 9,0,0,1};
    recomp_guest_store(DEVICE + 0xce0u, texture0, sizeof texture0);
    *recomp_memory_u32(STAGES + 21u * 4u) = 2u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 0x30000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    *recomp_memory_u32(0x3553f8u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.texgen_reflection && !submitted.data.draw.has_reflection &&
        submitted.data.draw.reflection_transform[12] == 9.0f &&
        submitted.data.draw.reflection_world_view[12] == 5.0f);
    /* COUNT2 never uploads column 2, so a stale all-ones _43 is unused. */
    *recomp_memory_u32(DEVICE + 0xce0u + 14u * 4u) = 0xffffffffu;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.texgen_reflection && submitted.data.draw.reflection_transform[12] == 9.0f);
    *recomp_memory_u32(DEVICE + 0xce0u + 13u * 4u) = 0xffffffffu;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strstr(stop_reason, "d3d8:reflection-transform:fvf=142:matrix=ce0:field=13:") != NULL);
    recomp_guest_store(DEVICE + 0xce0u, texture0, sizeof texture0);
    *recomp_memory_u32(STAGES + 21u * 4u) = 3u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-coordinates:", 29u) == 0 &&
        strstr(stop_reason, ":fvf=142:flags=0,3:tci=30000:") != NULL);
    *recomp_memory_u32(STAGES + 21u * 4u) = 0u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 0u;
    /* COUNT2 over mesh UV0 animates coordinates through TEXTURE0 (0x34f7d0). */
    *recomp_memory_u32(STAGES + 21u * 4u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submitted.data.draw.texture_transform_uv && !submitted.data.draw.texgen_reflection &&
        submitted.data.draw.reflection_transform[12] == 9.0f);
    *recomp_memory_u32(STAGES + 21u * 4u) = 0x102u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-coordinates:", 29u) == 0 &&
        strstr(stop_reason, ":fvf=142:flags=0,102:tci=0:") != NULL);
    *recomp_memory_u32(STAGES + 21u * 4u) = 0u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(0x3553f8u) = 1u;
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0u;
    *recomp_memory_u32(STAGES + 48u) = *recomp_memory_u32(STAGES + 64u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(lit->enabled && lit->stage_mode && !submitted.data.draw.has_texture && lit->count == 2u);
    recomp_guest_store(STAGES, saved_stages, sizeof saved_stages);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x116u;
    *recomp_memory_u32(0x355484u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(submitted.data.draw.blend_weight_count == 1u &&
        lit->normal_transforms[1][0] == 1.0f && lit->world_transforms[1][15] == 1.0f);
    *recomp_memory_u32(0x3553fcu) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(lit->view[3] == 0.0f);
    /* Disabled specular ignores a NaN specular color; enabled, it stops. */
    const uint32_t specular_green = *recomp_memory_u32(DEVICE + 0xf24u);
    *recomp_memory_u32(DEVICE + 0xf24u) = 0x7fc00000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(lit->enabled && lit->specular[0][1] == 0.0f);
    *recomp_memory_u32(0x3553fcu) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-material:9:7fc00000") == 0);
    *recomp_memory_u32(DEVICE + 0xf24u) = specular_green;
    /* A NaN diffuse alpha saturates by sign instead of stopping. */
    const uint32_t diffuse_alpha = *recomp_memory_u32(DEVICE + 0xf0cu);
    *recomp_memory_u32(DEVICE + 0xf0cu) = 0x7fc08080u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(lit->enabled && lit->material_diffuse[3] == 1.0f);
    *recomp_memory_u32(DEVICE + 0xf0cu) = 0xffc00000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 36u);
    CHECK(lit->enabled && lit->material_diffuse[3] == 0.0f);
    *recomp_memory_u32(DEVICE + 0xf0cu) = diffuse_alpha;
    *recomp_memory_u32(0x3553fcu) = 1u;
    *recomp_memory_u32(0x355400u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-local-viewer") == 0);
    *recomp_memory_u32(0x355400u) = 0u;
    *recomp_memory_u32(DEVICE + 0x7acu) = lights + 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:lighting-slot") == 0);
    return 0;
}

static int filter_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    static const uint32_t shader[][2] = {
        {0x08u, 0xdc30dd30u}, {0x20u, 0xcu}, {0x24u, 0x1c80u},
        {0x68u, 0x10c00u}, {0x6cu, 0x10d00u}, {0x70u, 0x30c00u},
        {0x90u, 0xcc20cd20u}, {0xb4u, 0x10c00u}, {0xb8u, 0x10d00u},
        {0xbcu, 0x30c00u}, {0xd4u, 0x11103u}, {0xd8u, 0x8421u},
    };
    static const uint32_t overrides[][2] = {
        {0x40260u, 0xd1d8d2d9u}, {0x40264u, 0xd1dad2dbu},
        {0x40a60u, 0x40404040u}, {0x40a64u, 0x40404040u},
        {0x40a80u, 0x40404040u}, {0x40a84u, 0x40404040u},
        {0x40ac0u, 0xc1c8c2c9u}, {0x40ac4u, 0xc1cac2cbu},
    };
    for (uint32_t i = 0u; i < sizeof shader / sizeof shader[0]; ++i)
        *recomp_memory_u32(INPUT + shader[i][0]) = shader[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    for (uint32_t i = 0u; i < sizeof overrides / sizeof overrides[0]; ++i)
        CHECK(recomp_d3d8_state_set_simple(overrides[i][0], overrides[i][1]));
    for (uint32_t stage = 0u; stage < 4u; ++stage) {
        *recomp_memory_u32(DEVICE + 0xf88u + stage * 4u) = 0x7000u;
        *recomp_memory_u32(STAGES + stage * 128u) = 3u;
        *recomp_memory_u32(STAGES + stage * 128u + 4u) = 3u;
        *recomp_memory_u32(STAGES + stage * 128u + 20u) = 1u;
        *recomp_memory_u32(STAGES + stage * 128u + 112u) = stage;
    }
    *recomp_memory_u32(DEVICE + 0x798u) = 0x404u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(submits == 1u && submitted.data.draw.four_tap_filter);
    CHECK(submitted.data.draw.four_tap_factors[0] == 0x40404040u &&
        submitted.data.draw.four_tap_factors[3] == 0x40404040u &&
        submitted.data.draw.four_tap_scale == 2.0f && submitted.data.draw.four_tap_sum_scale == 0.5f);
    /* Identity-scale variant with unsigned stage-2 inputs, disabled mips and live factors. */
    static const uint32_t identity[][2] = {
        {0x08u, 0x1c301d30u}, {0x68u, 0xc00u}, {0x6cu, 0xd00u}, {0x70u, 0xc00u},
        {0x90u, 0x0c200d20u}, {0xb4u, 0xc00u}, {0xb8u, 0xd00u}, {0xbcu, 0xc00u},
    };
    for (uint32_t i = 0u; i < sizeof identity / sizeof identity[0]; ++i)
        *recomp_memory_u32(0x355260u + identity[i][0]) = identity[i][1];
    CHECK(recomp_d3d8_state_set_simple(0x40a60u, 0x55555555u));
    CHECK(recomp_d3d8_state_set_simple(0x40a84u, 0x2b2b2b2bu));
    for (uint32_t stage = 0u; stage < 4u; ++stage) *recomp_memory_u32(STAGES + stage * 128u + 20u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(submits == 2u && submitted.data.draw.four_tap_filter &&
        submitted.data.draw.four_tap_factors[0] == 0x55555555u &&
        submitted.data.draw.four_tap_factors[3] == 0x2b2b2b2bu &&
        submitted.data.draw.four_tap_scale == 1.0f && submitted.data.draw.four_tap_sum_scale == 1.0f);
    *recomp_memory_u32(STAGES + 20u) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    *recomp_memory_u32(STAGES + 20u) = 0u;
    *recomp_memory_u32(0x355268u) ^= 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    return 0;
}

static int bright_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    static const uint32_t shader[][2] = {
        {0x00u, 0xd830d150u}, {0x04u, 0x5d301010u}, {0x20u, 0xcu}, {0x24u, 0x1c80u},
        {0x28u, 0x1a1a1a1au}, {0x68u, 0xd00u}, {0x6cu, 0xc0u}, {0x88u, 0xc820c140u},
        {0x8cu, 0x4d200000u}, {0xb4u, 0xd00u}, {0xb8u, 0xc0u}, {0xd4u, 0x11102u}, {0xd8u, 1u},
    };
    for (uint32_t i = 0u; i < sizeof shader / sizeof shader[0]; ++i)
        *recomp_memory_u32(INPUT + shader[i][0]) = shader[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(STAGES + 12u) = *recomp_memory_u32(STAGES + 16u) = 5u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 1u && submitted.data.draw.bright_pass && !submitted.data.draw.four_tap_filter);
    CHECK(submitted.data.draw.bright_pass_threshold == 0x1a1a1a1au);
    /* SetPixelShaderConstant(0) reaches stage-0 C0 through the zero C0 mapping. */
    const float threshold[4] = {0.2f, 0.2f, 0.2f, 0.2f};
    recomp_guest_store(INPUT + 0x100u, threshold, sizeof threshold);
    invoke(0x349720u, 0u, INPUT + 0x100u, 1u, 0u);
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 2u && submitted.data.draw.bright_pass_threshold == 0x33333333u);
    *recomp_memory_u32(STAGES + 16u) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    *recomp_memory_u32(STAGES + 16u) = 5u;
    CHECK(recomp_d3d8_state_set_simple(0x40ac4u, 0x4d200001u));
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    /* 0xa74f0's screen tint, with 0xa75d0's SetPixelShaderConstant(1) reaching stage-1 C0. */
    initialize();
    memset(ram + INPUT, 0, 0xf0u);
    static const uint32_t tint[][2] = {
        {0x04u, 0xd1cd1010u}, {0x28u, 0x004c961du}, {0x6cu, 0xc0u}, {0x88u, 0xc8c10000u},
        {0x8cu, 0xc1cd0000u}, {0xb4u, 0x820d0u}, {0xb8u, 0xc0u}, {0xd4u, 0x11102u}, {0xd8u, 1u},
        {0xe4u, 0xffffff10u}, {0xe8u, 0xffffffffu}, {0xecu, 0x1ffu},
    };
    for (uint32_t i = 0u; i < sizeof tint / sizeof tint[0]; ++i)
        *recomp_memory_u32(INPUT + tint[i][0]) = tint[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    const float sepia[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    recomp_guest_store(INPUT + 0x100u, sepia, sizeof sepia);
    invoke(0x349720u, 1u, INPUT + 0x100u, 1u, 0u);
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 1u && submitted.data.draw.luminance_tint && !submitted.data.draw.bright_pass &&
        submitted.data.draw.luminance_tint_factors[0] == 0x004c961du &&
        submitted.data.draw.luminance_tint_factors[1] == 0xffff8000u);
    /* Fog replaces the default final combiner. */
    *recomp_memory_u32(0x3553d0u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 1u && strcmp(stop_reason, "d3d8:draw-pixel-shader-unsupported") == 0);
    *recomp_memory_u32(0x3553d0u) = 0u;
    /* Bloom composite: MODULATE2X(TEXTURE, TFACTOR) with stage-0 alpha disabled. */
    initialize();
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x104u;
    *recomp_memory_u32(STAGES + 48u) = 5u;
    *recomp_memory_u32(STAGES + 60u) = 3u;
    *recomp_memory_u32(STAGES + 64u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 1u && submitted.data.draw.texture_factor_color_scale == 2u &&
        submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE &&
        submitted.data.draw.material_alpha == 1.0f);
    *recomp_memory_u32(STAGES + 48u) = 6u;
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected 4x composite stop: %s\n", stop_reason); return 1; }
    invoke(0x348240u, 9u, 4u, VERTICES, 24u);
    CHECK(submits == 2u && submitted.data.draw.texture_factor_color_scale == 4u &&
        submitted.data.draw.material_alpha == 1.0f);
    /* Trail strips (setup 0x23a42d): 1x MODULATE(TEXTURE, TFACTOR), alpha DISABLE. */
    *recomp_memory_u32(STAGES + 48u) = 4u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x102u;
    invoke(0x348240u, 5u, 3u, VERTICES, 20u);
    CHECK(submits == 3u && submitted.data.draw.texture_factor_modulate &&
        !submitted.data.draw.texture_factor_color_scale &&
        submitted.data.draw.material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE);
    *recomp_memory_u32(STAGES + 48u) = 5u;
    *recomp_memory_u32(DEVICE + 0x798u) = 0x144u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(submits == 3u && strncmp(stop_reason, "d3d8:draw-texture-combine:", 26) == 0);
    return 0;
}

static int program_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    enum { PROGRAM = 0x9000u };
    for (uint32_t i = 0u; i < 16u; ++i) *recomp_memory_u32(PROGRAM + 0x1cu + i * 16u) = 2u;
    *recomp_memory_u32(PROGRAM + 4u) = 0x10u;
    *recomp_memory_u32(PROGRAM + 8u) = 1u;
    *recomp_memory_u32(PROGRAM + 0xcu) = 5u;
    *recomp_memory_u32(PROGRAM + 0x1cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0x38u) = 12u;
    *recomp_memory_u32(PROGRAM + 0x3cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0xa8u) = 24u;
    *recomp_memory_u32(PROGRAM + 0xacu) = 0x22u;
    const uint32_t upload[5] = {(4u << 18u) | 0xb00u, 1u, 2u, 3u, 4u};
    recomp_guest_store(PROGRAM + 0x114u, upload, sizeof upload);
    const float one = 1.0f, two = 2.0f, constant = 7.0f;
    recomp_guest_store(DEVICE + 0x95cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x960u, &one, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x970u, &one, 4u);
    recomp_guest_store(DEVICE + 0x948u, &two, 4u);
    recomp_guest_store(0x354440u + 96u * 16u, &constant, 4u);
    *recomp_memory_u32(DEVICE + 0x798u) = PROGRAM + 1u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(STAGES + 20u) = 1u;
    *recomp_memory_u32(STAGES + 48u) = *recomp_memory_u32(STAGES + 64u) = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(submits == 1u && draw->fvf == 0x112u && draw->program_count == 1u);
    CHECK(draw->program[0][3] == 4u && draw->program_constants[96][0] == 7.0f);
    CHECK(draw->program_constants[58][0] == 360.0f && draw->program_constants[58][1] == -240.0f);
    CHECK(draw->program_constants[58][2] == 2.0f && draw->program_constants[59][0] == 360.53125f);
    CHECK(draw->program_constants[59][1] == 240.53125f && draw->program_constants[59][2] == 0.0f);
    CHECK(!draw->has_transform && draw->screen_space_offset[0] == 0.0f && !draw->program_alpha_mask);
    /* A 2x supersampled back buffer keeps the host at its logical 720 pixels. */
    recomp_guest_store(DEVICE + 0x95cu, &two, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &two, 4u);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(draw->program_constants[58][0] == 360.0f && draw->program_constants[59][0] == 360.53125f);
    recomp_guest_store(DEVICE + 0x95cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &one, 4u);
    /* Multisampled back-buffer rendering shifts the sample grid by half a pixel. */
    *recomp_memory_u32(DEVICE + 8u) = 0x8000u;
    *recomp_memory_u32(0x3554c0u) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(draw->program_constants[59][0] == 360.03125f);
    /* Stage 1 masks alpha with its own texture. */
    const uint32_t mask_stage[6][2] = {{48u, 2u}, {56u, 1u}, {60u, 0u}, {64u, 4u}, {72u, 1u}, {76u, 2u}};
    for (uint32_t i = 0u; i < 6u; ++i) *recomp_memory_u32(STAGES + 128u + mask_stage[i][0]) = mask_stage[i][1];
    *recomp_memory_u32(STAGES + 128u + 20u) = 1u;
    *recomp_memory_u32(STAGES + 128u + 24u) = 0xc0000000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 4u && !draw->program_alpha_mask);
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 5u && draw->program_alpha_mask && draw->reflection_bytes == ram + 0x8000u);
    CHECK(draw->program_mask_lod_bias == -2.0f);
    *recomp_memory_u32(STAGES + 48u) = 5u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(submits == 5u && strncmp(stop_reason, "d3d8:vertex-program-stages", 26) == 0);
    *recomp_memory_u32(STAGES + 48u) = 2u;
    /* The projected shadow (0x7af4d0, FUN_0027a760): position and normal only,
       stage-0 alpha MODULATE(TEXTURE, TFACTOR). */
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected shadow stop: %s\n", stop_reason); return 1; }
    *recomp_memory_u32(PROGRAM + 0xacu) = 2u;
    *recomp_memory_u32(STAGES + 64u) = 4u;
    *recomp_memory_u32(STAGES + 76u) = 3u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 6u && draw->fvf == 0x012u && draw->modulate_texture_factor_alpha);
    *recomp_memory_u32(STAGES + 76u) = 4u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 6u && strncmp(stop_reason, "d3d8:vertex-program-stages", 26) == 0);
    /* FUN_002718e0's program (0x7ae238): position, color and UV; stage 0
       MODULATE(TEXTURE, oD0) color, MODULATE(TEXTURE, TFACTOR) alpha, mipped. */
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected color-program stop: %s\n", stop_reason); return 1; }
    *recomp_memory_u32(PROGRAM + 0x3cu) = 2u;
    *recomp_memory_u32(PROGRAM + 0x4cu) = 0x40u;
    *recomp_memory_u32(PROGRAM + 0x48u) = 12u;
    *recomp_memory_u32(PROGRAM + 0xacu) = 0x22u;
    *recomp_memory_u32(PROGRAM + 0xa8u) = 16u;
    *recomp_memory_u32(STAGES + 48u) = 4u;
    *recomp_memory_u32(STAGES + 76u) = 3u;
    *recomp_memory_u32(STAGES + 20u) = 2u;
    texture.mip_levels = 2u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 7u && draw->fvf == 0x142u && draw->modulate_texture_factor_alpha &&
        draw->directional.stage_mode && !draw->point_mip_filter);
    CHECK(draw->directional.stage[0] == 1.0f && draw->directional.stage[1] == 1.0f &&
        draw->directional.stage[2] == 1.0f && draw->directional.stage[3] == 0.0f);
    texture.mip_levels = 1u;
    *recomp_memory_u32(STAGES + 20u) = 1u;
    *recomp_memory_u32(STAGES + 48u) = 2u;
    *recomp_memory_u32(STAGES + 76u) = 0u;
    *recomp_memory_u32(PROGRAM + 0x3cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0x4cu) = 2u;
    *recomp_memory_u32(PROGRAM + 0x48u) = 0u;
    *recomp_memory_u32(PROGRAM + 0xa8u) = 24u;
    *recomp_memory_u32(PROGRAM + 0xacu) = 0x22u;
    *recomp_memory_u32(STAGES + 64u) = 2u;
    *recomp_memory_u32(PROGRAM + 0x38u) = 16u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:vertex-program-declaration:00009001:2:", 43) == 0);
    /* The stage effect's declaration (0x7afd0c): position, D3DCOLOR v3 at 12
       and four UV sets v9-v12 at 16-40 draw as FVF 0x442. */
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected four-UV stop: %s\n", stop_reason); return 1; }
    *recomp_memory_u32(PROGRAM + 0x38u) = 0u;
    *recomp_memory_u32(PROGRAM + 0x3cu) = 2u;
    *recomp_memory_u32(PROGRAM + 0x48u) = 12u;
    *recomp_memory_u32(PROGRAM + 0x4cu) = 0x40u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        *recomp_memory_u32(PROGRAM + 0xa8u + i * 16u) = 16u + i * 8u;
        *recomp_memory_u32(PROGRAM + 0xacu + i * 16u) = 0x22u;
    }
    const uint32_t before = submits;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(submits == before + 1u && draw->fvf == 0x442u && draw->vertex_stride == 48u);
    for (uint32_t u = 3u; u > 0u; --u) {
        for (uint32_t v = 1u; v <= 3u; ++v) {
            *recomp_memory_u32(STAGES) = u;
            *recomp_memory_u32(STAGES + 4u) = v;
            invoke(0x348240u, 5u, 3u, VERTICES, 48u);
            CHECK(draw->address_u == u && draw->address_v == v);
        }
    }
    CHECK(submits == before + 10u);
    /* Disabled combiner chains on programs remain outside this binding. */
    *recomp_memory_u32(STAGES + 48u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:vertex-program-stages:", 26u) == 0);
    *recomp_memory_u32(STAGES + 48u) = 2u;
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected restored stages stop: %s\n", stop_reason); return 1; }
    *recomp_memory_u32(STAGES) = *recomp_memory_u32(STAGES + 4u) = 3u;
    *recomp_memory_u32(STAGES + 44u) = 4u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(draw->alpha_kill && draw->address_u == 3u && draw->address_v == 3u);
    *recomp_memory_u32(STAGES + 44u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    CHECK(!draw->alpha_kill);
    *recomp_memory_u32(STAGES + 44u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:vertex-program-sampler:0:", 28u) == 0);
    *recomp_memory_u32(STAGES + 44u) = 0u;
    *recomp_memory_u32(STAGES + 128u + 44u) = 4u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:vertex-program-sampler:1:", 28u) == 0);
    *recomp_memory_u32(STAGES + 128u + 44u) = 0u;
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected restored program stop: %s\n", stop_reason); return 1; }
    /* SetStateUP copies each declaration-sized record, then advances by stride;
       a smaller stride makes records overlap, including the final record tail. */
    const uint32_t overlap = (uint32_t)sizeof ram - 96u;
    for (uint32_t i = 0u; i < 24u; ++i) *recomp_memory_u32(overlap + i * 4u) = 0x3f000000u + i;
    invoke(0x348240u, 5u, 3u, overlap, 24u);
    CHECK(draw->vertex_stride == 48u && draw->vertex_count == 3u);
    for (uint32_t i = 0u; i < 3u; ++i)
        CHECK(memcmp(captured_vertices + i * 12u, ram + overlap + i * 24u, 48u) == 0);
    const uint16_t reordered[] = {2u, 0u, 1u};
    recomp_guest_store(INDICES, reordered, sizeof reordered);
    *recomp_memory_u32(0x3579a8u) = 24u;
    *recomp_memory_u32(BUFFER + 4u) = overlap;
    invoke(0x348450u, 5u, 3u, INDICES, 0u);
    CHECK(draw->vertex_stride == 48u && draw->vertex_count == 3u &&
        memcmp(captured_indices, reordered, sizeof reordered) == 0);
    for (uint32_t i = 0u; i < 3u; ++i)
        CHECK(memcmp(captured_vertices + i * 12u, ram + overlap + i * 24u, 48u) == 0);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, overlap + 4u, 24u); return 1; }
    CHECK(strcmp(stop_reason, "test:guest-span") == 0);
    /* A gap in the UV sets is not an FVF layout. */
    *recomp_memory_u32(PROGRAM + 0xccu) = 2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 48u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:vertex-program-declaration:00009001:0:", 43) == 0);
    return 0;
}

static int distortion_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    /* Definition 0x7af5d8: PROJECT2D/DOTPRODUCT/DOT_ST, r0 = t2, zero final words. */
    static const uint32_t shader[][2] = {
        {0x00u, 0xda301010u}, {0x68u, 0xc0u}, {0x88u, 0xca200000u}, {0xb4u, 0xc0u},
        {0xd4u, 0x00011101u}, {0xd8u, 0x2621u}, {0xdcu, 0x11u},
    };
    for (uint32_t i = 0u; i < sizeof shader / sizeof shader[0]; ++i)
        *recomp_memory_u32(INPUT + shader[i][0]) = shader[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    *recomp_memory_u32(DEVICE + 0xf88u) = *recomp_memory_u32(DEVICE + 0xf90u) = 0x7000u;
    *recomp_memory_u32(STAGES + 256u) = *recomp_memory_u32(STAGES + 260u) = 3u;
    float source[5][14];
    for (uint32_t i = 0u; i < 5u * 14u; ++i) source[i / 14u][i % 14u] = (float)i;
    recomp_guest_store(VERTICES, source, sizeof source);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x140404u;
    invoke(0x348240u, 5u, 3u, VERTICES, 56u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(submits == 1u && draw->distortion && draw->fvf == 0x404u && draw->vertex_stride == 48u);
    CHECK(draw->address_u == 3u && draw->reflection_address_u == 1u && draw->has_texture);
    for (uint32_t v = 0u; v < 3u; ++v)
        for (uint32_t i = 0u; i < 12u; ++i) CHECK(captured_vertices[v * 12u + i] == source[v][i]);
    /* With no specular attribute the SDK supplies zero, even when enabled. */
    *recomp_memory_u32(0x3553fcu) = 1u;
    invoke(0x348240u, 6u, 4u, VERTICES, 56u);
    CHECK(submits == 2u && draw->distortion && draw->fvf == 0x404u && draw->vertex_stride == 48u);
    CHECK(!draw->directional.enabled && *recomp_memory_u32(0x3553fcu) == 1u);
    for (uint32_t v = 0u; v < 4u; ++v)
        for (uint32_t i = 0u; i < 12u; ++i) CHECK(captured_vertices[v * 12u + i] == source[v][i]);
    /* Fog changes the SDK's final combiner. */
    *recomp_memory_u32(0x3553d0u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 56u); return 1; }
    CHECK(submits == 2u && strncmp(stop_reason, "d3d8:draw-vertex-layout", 23u) == 0);
    *recomp_memory_u32(0x3553d0u) = 0u;
    *recomp_memory_u32(*recomp_memory_u32(*recomp_memory_u32(OUTPUT) + 8u) + 0xdcu) = 0x22u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 56u); return 1; }
    CHECK(submits == 2u && strncmp(stop_reason, "d3d8:draw-vertex-layout", 23u) == 0);
    return 0;
}

static void set_stage(uint32_t stage, uint32_t field, uint32_t value)
{
    *recomp_memory_u32(STAGES + stage * 128u + field * 4u) = value;
}

static int factor_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected factor stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    CHECK(recomp_d3d_set_texture_factor(&state, 0x804080c0u));
    for (uint32_t format = 0u; format < 4u; ++format) {
        const bool screen = (format & 1u) != 0u, white = format >= 2u;
        *recomp_memory_u32(DEVICE + 0x798u) = (screen ? 0x104u : 0x102u) | (white ? 0u : 0x40u);
        for (uint32_t reverse = 0u; reverse < 2u; ++reverse) {
            for (uint32_t alpha = 0u; alpha < (white ? 1u : 2u); ++alpha) {
                const uint32_t source = alpha == 0u ? 3u : 0u;
                set_stage(0u, 14u, reverse ? 3u : 2u); set_stage(0u, 15u, reverse ? 2u : 3u);
                set_stage(0u, 18u, reverse ? source : 2u); set_stage(0u, 19u, reverse ? 2u : source);
                invoke(0x348240u, 5u, 3u, VERTICES, (screen ? 24u : 20u) + (white ? 0u : 4u));
                const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
                CHECK(draw->has_texture && !draw->directional.enabled && draw->directional.stage_mode);
                CHECK(draw->directional.stage[0] == 1.0f && draw->directional.stage[1] == 2.0f &&
                    draw->directional.stage[2] == 1.0f && draw->directional.stage[3] == (alpha == 0u ? 2.0f : 1.0f));
                CHECK(draw->texture_factor == 0x804080c0u && !draw->texture_factor_modulate &&
                    !draw->modulate_texture_factor_alpha && draw->material_alpha_mode == RECOMP_D3D_MATERIAL_ALPHA_NONE);
            }
        }
    }
    CHECK(submits == 12u);
    return 0;
}

static int position_texgen_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected position texgen stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    set_stage(0u, 21u, 2u); set_stage(0u, 28u, 0x20000u);
    const float matrix[16] = {2,3,4,5, 6,7,8,9, 10,11,12,13, 14,15,16,17};
    recomp_guest_store(DEVICE + 0xce0u, matrix, sizeof matrix);
    const float singular[16] = {0,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1};
    recomp_guest_store(DEVICE + 0xde0u, singular, sizeof singular);
    for (uint32_t normal = 0u; normal < 2u; ++normal) {
        for (uint32_t count = 2u; count <= 3u; ++count) {
            set_stage(0u, 21u, count);
            *recomp_memory_u32(DEVICE + 0x798u) = normal ? 0x112u : 0x142u;
            invoke(0x348240u, 5u, 3u, VERTICES, normal ? 32u : 24u);
            const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
            CHECK(draw->texgen_projected && !draw->texgen_reflection && !draw->texture_transform_uv);
            CHECK(draw->reflection_world_view[0] == 0.0f && draw->reflection_world_view[12] == 5.0f);
            for (uint32_t i = 0u; i < 16u; ++i)
                CHECK(draw->reflection_transform[i] == (i % 4u == 2u ? (i == 14u ? 1.0f : 0.0f) : matrix[i]));
        }
    }
    CHECK(submits == 4u);
    set_stage(0u, 21u, 0x103u);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submitted.data.draw.texgen_projected &&
        memcmp(submitted.data.draw.reflection_transform, matrix, sizeof matrix) == 0);
    /* 0x1df200's 1/0 projector: a non-finite translation sends every clamped
       sample to the edge, here one transparent white texel. */
    texture.format_byte = 0x0fu;
    texture.bits_per_pixel = 8u;
    texture.width = texture.height = 8u;
    /* Synthetic blocks: alpha endpoint 0 everywhere, color endpoint 1 white. */
    memset(ram + 0x8000u, 0, 64u);
    for (uint32_t block = 0u; block < 4u; ++block) {
        uint8_t *bytes = ram + 0x8000u + block * 16u;
        bytes[1] = 1u;
        bytes[10] = bytes[11] = 0xffu;
        memset(bytes + 12u, 0x55, 4u);
    }
    float infinite[16];
    for (uint32_t i = 0u; i < 16u; ++i) infinite[i] = NAN;
    infinite[12] = -INFINITY; infinite[13] = INFINITY; infinite[15] = 1.0f;
    recomp_guest_store(DEVICE + 0xce0u, infinite, sizeof infinite);
    set_stage(0u, 0u, 3u); set_stage(0u, 1u, 3u); set_stage(0u, 21u, 2u);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    const float *edge = submitted.data.draw.reflection_transform;
    CHECK(submits == 6u && edge[0] == 0.0f && edge[5] == 0.0f &&
        edge[12] == -1.0f && edge[13] == -1.0f && edge[14] == 1.0f);
    /* A wrapped axis, or an edge texel that differs, leaves the sample unknown. */
    for (uint32_t variant = 0u; variant < 2u; ++variant) {
        if (variant == 0u) set_stage(0u, 1u, 1u);
        else { set_stage(0u, 1u, 3u); ram[0x8000u + 3u * 16u + 15u] = 0x15u; }
        if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
        CHECK(strncmp(stop_reason, "d3d8:reflection-transform:", 26u) == 0 && submits == 6u);
    }
    return 0;
}

/* The stage effect's overlay passes, as FUN_0027be10 and FUN_0027c400 set them. */
static int effect_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(0x3553f8u) = 0u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    const float texture0[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,3,1};
    recomp_guest_store(DEVICE + 0xce0u, texture0, sizeof texture0);
    /* Pass 2: SELECTARG1(TEXTURE) / MODULATE(TEXTURE, CURRENT), projected
       camera-space texgen, stage 1 alpha MODULATE(TFACTOR, CURRENT). */
    set_stage(0u, 12u, 2u); set_stage(0u, 14u, 2u); set_stage(0u, 15u, 1u);
    set_stage(0u, 16u, 4u); set_stage(0u, 18u, 2u); set_stage(0u, 19u, 1u);
    set_stage(0u, 21u, 0x103u); set_stage(0u, 28u, 0x20000u);
    set_stage(1u, 11u, 4u); set_stage(1u, 12u, 2u); set_stage(1u, 14u, 1u); set_stage(1u, 15u, 2u);
    set_stage(1u, 16u, 4u); set_stage(1u, 18u, 3u); set_stage(1u, 19u, 1u);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(submits == 1u && draw->texgen_projected && !draw->texgen_reflection &&
        draw->reflection_transform[14] == 3.0f && draw->modulate_texture_factor_alpha &&
        !draw->stage1_alpha_mask && !draw->directional.enabled && draw->directional.stage_mode);
    CHECK(draw->directional.stage[0] == 1.0f && draw->directional.stage[1] == 0.0f &&
        draw->directional.stage[2] == 1.0f && draw->directional.stage[3] == 1.0f);
    /* FUN_0027be10 then binds a stage-1 texture (0x27c370) that its ops never
       read; it only applies its ALPHAKILL at UV0. */
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(submits == 2u && draw->stage1_alpha_mask && draw->stage1_alpha_kill && draw->stage1_kill_only &&
        draw->modulate_texture_factor_alpha && draw->texgen_projected &&
        draw->reflection_bytes == ram + texture.data);
    set_stage(1u, 28u, 1u);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(submits == 2u && strncmp(stop_reason, "d3d8:stage1-mask:", 17) == 0);
    set_stage(1u, 28u, 0u);
    /* Pass 3: a pretransformed quad, stage-0 alpha MODULATE(TFACTOR, CURRENT)
       and stage 1 alpha MODULATE(TEXTURE, CURRENT) over UV1 with ALPHAKILL. */
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected stop: %s\n", stop_reason); return 1; }
    set_stage(0u, 18u, 3u); set_stage(0u, 21u, 0u); set_stage(0u, 28u, 0u);
    set_stage(1u, 18u, 2u); set_stage(1u, 28u, 1u); set_stage(1u, 0u, 3u);
    *recomp_memory_u32(DEVICE + 0x798u) = 0x244u;
    invoke(0x348240u, 6u, 4u, VERTICES, 36u);
    CHECK(submits == 3u && draw->stage1_alpha_mask && draw->stage1_alpha_kill && !draw->stage1_kill_only &&
        draw->modulate_texture_factor_alpha && !draw->texgen_projected &&
        draw->reflection_bytes == ram + texture.data && draw->reflection_address_u == 3u);
    CHECK(draw->directional.stage[0] == 1.0f && draw->directional.stage[1] == 0.0f &&
        draw->directional.stage[2] == 0.0f && draw->directional.stage[3] == 1.0f);
    /* An unbound mask compiles to a pass-through (0x352e46). */
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0u;
    invoke(0x348240u, 6u, 4u, VERTICES, 36u);
    CHECK(submits == 4u && !draw->stage1_alpha_mask && draw->modulate_texture_factor_alpha);
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    set_stage(1u, 28u, 0u);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 6u, 4u, VERTICES, 36u); return 1; }
    CHECK(submits == 4u && strncmp(stop_reason, "d3d8:stage1-mask:", 17) == 0);
    return 0;
}

/* The stage effect's bump pass (FUN_0027ffc0 definition, FUN_002807f0 state). */
static int bump_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected bump stop: %s\n", stop_reason); return 1; }
    initialize();
    static const uint32_t shader[][2] = {
        {0x00u, 0xdbdb1010u}, {0x04u, 0xdbdb1010u}, {0x20u, 0x130c0300u}, {0x24u, 0x1480u},
        {0x68u, 0xb0u}, {0x6cu, 0xb0u}, {0x90u, 0xca3b0000u}, {0x94u, 0xccc40000u},
        {0xbcu, 0xc0u}, {0xc0u, 0xc0u}, {0xd4u, 0x11104u}, {0xd8u, 0x318c1u},
    };
    for (uint32_t i = 0u; i < sizeof shader / sizeof shader[0]; ++i)
        *recomp_memory_u32(INPUT + shader[i][0]) = shader[i][1];
    invoke(0x349490u, INPUT, OUTPUT, 0u, 0u);
    const uint32_t definition = *recomp_memory_u32(*recomp_memory_u32(OUTPUT) + 8u);
    invoke(0x349520u, *recomp_memory_u32(OUTPUT), 0u, 0u, 0u);
    /* Declaration 0x7afd0c: position, D3DCOLOR v3 at 12, v9-v12 at 16-40. */
    enum { PROGRAM = 0x9000u };
    for (uint32_t i = 0u; i < 16u; ++i) *recomp_memory_u32(PROGRAM + 0x1cu + i * 16u) = 2u;
    *recomp_memory_u32(PROGRAM + 4u) = 0x10u;
    *recomp_memory_u32(PROGRAM + 8u) = 1u;
    *recomp_memory_u32(PROGRAM + 0xcu) = 5u;
    *recomp_memory_u32(PROGRAM + 0x1cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0x48u) = 12u;
    *recomp_memory_u32(PROGRAM + 0x4cu) = 0x40u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        *recomp_memory_u32(PROGRAM + 0xa8u + i * 16u) = 16u + i * 8u;
        *recomp_memory_u32(PROGRAM + 0xacu + i * 16u) = 0x22u;
    }
    const uint32_t upload[5] = {(4u << 18u) | 0xb00u, 1u, 2u, 3u, 4u};
    recomp_guest_store(PROGRAM + 0x114u, upload, sizeof upload);
    *recomp_memory_u32(DEVICE + 0x798u) = PROGRAM + 1u;
    const float one = 1.0f;
    recomp_guest_store(DEVICE + 0x95cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x960u, &one, 4u);
    recomp_guest_store(DEVICE + 0x96cu, &one, 4u);
    recomp_guest_store(DEVICE + 0x970u, &one, 4u);
    recomp_guest_store(DEVICE + 0x948u, &one, 4u);
    /* Stage 0: signed V8U8 map; stage 2: scene; stage 3: mask. */
    texture.format_byte = 0x28u;
    texture.bits_per_pixel = 16u;
    *recomp_memory_u32(DEVICE + 0xf88u) = *recomp_memory_u32(DEVICE + 0xf90u) =
        *recomp_memory_u32(DEVICE + 0xf94u) = 0x7000u;
    set_stage(0u, 10u, 0xe0000000u);
    set_stage(0u, 0u, 1u); set_stage(0u, 1u, 1u);
    set_stage(3u, 0u, 3u); set_stage(3u, 1u, 3u);
    const float m2[4] = {0.5f, 0.0f, 0.0f, -0.5f}, m3[4] = {0.25f, 0.125f, -0.125f, 0.25f};
    recomp_guest_store(STAGES + 2u * 128u + 22u * 4u, m2, sizeof m2);
    recomp_guest_store(STAGES + 3u * 128u + 22u * 4u, m3, sizeof m3);
    /* LINEAR fog from 10 to 30 (0x34f5f0): bias 30/20, scale -1/20. */
    const float range[2] = {10.0f, 30.0f};
    *recomp_memory_u32(0x3553d0u) = 1u;
    *recomp_memory_u32(0x3553d4u) = 3u;
    recomp_guest_store(0x3553d8u, range, sizeof range);
    *recomp_memory_u32(0x355488u) = 0xff112233u;
    invoke(0x348240u, 5u, 3u, VERTICES, 48u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(submits == 1u && draw->bump_env && draw->fvf == 0x442u && draw->program_count == 1u);
    CHECK(draw->alpha_mask.format_byte == 0x28u && draw->alpha_mask_bytes == ram + 0x8000u &&
        !draw->has_alpha_mask && draw->bump_address_u == 1u && draw->has_texture);
    CHECK(draw->reflection_bytes == ram + 0x8000u && draw->reflection_address_u == 3u);
    CHECK(memcmp(draw->bump_matrices[0], m2, sizeof m2) == 0 && memcmp(draw->bump_matrices[1], m3, sizeof m3) == 0);
    CHECK(draw->bump_fog[0] == 1.5f && draw->bump_fog[1] == -0.05f && draw->bump_fog[2] == 1.0f);
    CHECK(draw->fog_color == 0xff112233u && !draw->directional.stage_mode);
    /* The same shader under a position/normal/UV program (the 0x112 variant). */
    *recomp_memory_u32(PROGRAM + 0x38u) = 12u;
    *recomp_memory_u32(PROGRAM + 0x3cu) = 0x32u;
    *recomp_memory_u32(PROGRAM + 0x48u) = 0u;
    *recomp_memory_u32(PROGRAM + 0x4cu) = 2u;
    *recomp_memory_u32(PROGRAM + 0xa8u) = 24u;
    for (uint32_t i = 1u; i < 4u; ++i) *recomp_memory_u32(PROGRAM + 0xacu + i * 16u) = 2u;
    *recomp_memory_u32(0x3553d0u) = 0u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 2u && draw->bump_env && draw->fvf == 0x112u && draw->bump_fog[2] == 0.0f);
    /* Exponential fog, an unsigned map or other shader words stay guarded. */
    *recomp_memory_u32(0x3553d0u) = 1u;
    *recomp_memory_u32(0x3553d4u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(submits == 2u && strcmp(stop_reason, "d3d8:stage-bump-fog:1") == 0);
    *recomp_memory_u32(0x3553d0u) = 0u;
    set_stage(0u, 10u, 0u);
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(submits == 2u && strncmp(stop_reason, "d3d8:stage-bump-inputs:2:28:16:0:", 33) == 0);
    set_stage(0u, 10u, 0xe0000000u);
    *recomp_memory_u32(definition + 0xd8u) = 0x318c2u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(submits == 2u && strncmp(stop_reason, "d3d8:vertex-program-stages:", 27) == 0 &&
        strstr(stop_reason, ":n=1") != NULL);
    /* The water shader (FUN_0026bd20) reads stages 1 and 3 and needs SPECULARENABLE. */
    static const uint32_t water[][2] = {
        {0x00u, 0u}, {0x04u, 0x1d1d1010u}, {0x08u, 0xdddd1010u}, {0x0cu, 0xdddd1010u},
        {0x10u, 0u}, {0x14u, 0u}, {0x18u, 0xdbd41010u}, {0x20u, 0x130c0300u}, {0x24u, 0x1c80u},
        {0x68u, 0u}, {0x6cu, 0xd0u}, {0x70u, 0xd0u}, {0x74u, 0xd0u}, {0x78u, 0u}, {0x7cu, 0u},
        {0x80u, 0xc0u}, {0x88u, 0x444b0000u}, {0x8cu, 0x0d0d0000u}, {0x90u, 0xcdcd0000u},
        {0x94u, 0xc5d90000u}, {0x98u, 0x9d208c20u}, {0x9cu, 0xdbc9200cu}, {0xa0u, 0u},
        {0xa8u, 0u}, {0xb4u, 0x820d0u}, {0xb8u, 0xd0u}, {0xbcu, 0xd0u}, {0xc0u, 0xc0u},
        {0xc4u, 0xc00u}, {0xc8u, 0xc00u}, {0xccu, 0u}, {0xd4u, 0x11107u}, {0xd8u, 0x300c1u},
        {0xdcu, 0u}, {0xe0u, 0u},
    };
    for (uint32_t i = 0u; i < sizeof water / sizeof water[0]; ++i)
        *recomp_memory_u32(definition + water[i][0]) = water[i][1];
    *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    set_stage(1u, 3u, 2u); set_stage(1u, 4u, 2u);
    const float m1[4] = {0.75f, 0.0f, 0.0f, 0.75f};
    recomp_guest_store(STAGES + 1u * 128u + 22u * 4u, m1, sizeof m1);
    *recomp_memory_u32(0x3553fcu) = 1u;
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 3u && draw->bump_env && draw->bump_water && draw->has_texture &&
        memcmp(draw->bump_matrices[0], m1, sizeof m1) == 0 && memcmp(draw->bump_matrices[1], m3, sizeof m3) == 0);
    *recomp_memory_u32(0x3553fcu) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
    CHECK(submits == 3u && strcmp(stop_reason, "d3d8:stage-water-specular") == 0);
    return 0;
}

static int volume_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected volume stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0x798u) = 0x142u;
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(STAGES + 8u) = 3u;
    texture.width = texture.height = 64u;
    texture.volume_depth = 8u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    const RecompD3dPresenterDrawCommand *draw = &submitted.data.draw;
    CHECK(draw->texture.volume_depth == 8u && draw->texture_byte_count == 64u * 64u * 8u * 4u &&
        draw->address_w == 3u);
    const float transform[16] = {1,0,0,0, 0,1,0,0, 0,0,0.75f,0, 0,0,0,1};
    recomp_guest_store(DEVICE + 0xce0u, transform, sizeof transform);
    *recomp_memory_u32(STAGES + 21u * 4u) = 3u;
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(draw->texture_transform_uv && draw->reflection_transform[10] == 0.75f);
    /* COUNT3 camera-space coordinates retain R for volume sampling. */
    set_stage(0u, 28u, 0x20000u);
    invoke(0x348240u, 5u, 3u, VERTICES, 24u);
    CHECK(draw->texgen_projected && !draw->texture_transform_uv &&
        memcmp(draw->reflection_transform, transform, sizeof transform) == 0);
    for (uint32_t projected = 0u; projected < 2u; ++projected) {
        set_stage(0u, 21u, projected ? 0x103u : 2u);
        if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
        CHECK(strncmp(stop_reason, "d3d8:draw-texture-coordinates:", 29u) == 0);
    }
    set_stage(0u, 21u, 3u);
    set_stage(0u, 28u, 0u);
    texture.data = RECOMP_XBOX_RAM_SIZE - 64u * 64u * 4u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strcmp(stop_reason, "test:guest-span") == 0);
    texture.data = 0x8000u;
    *recomp_memory_u32(STAGES + 8u) = 4u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:volume-draw-layout:", 23) == 0);
    *recomp_memory_u32(STAGES + 8u) = 1u;
    *recomp_memory_u32(STAGES + 28u * 4u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 24u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-texture-coordinates:", 29u) == 0 &&
        strstr(stop_reason, ":fvf=142:flags=0,3:tci=1:") != NULL);
    return 0;
}

static int reflection_inverse_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected inverse stop: %s\n", stop_reason); return 1; }
    const float singular[16] = {0,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1};
    const uint32_t reflection[3][6] = {{2,2,1,4,2,0}, {14,2,1,14,2,1}, {4,0,1,4,0,1}};
    const uint32_t fields[] = {12,14,15,16,18,19};
    for (uint32_t normals = 0u; normals < 2u; ++normals) {
        for (uint32_t generated = 0u; generated < 4u; ++generated) {
            initialize();
            *recomp_memory_u32(DEVICE + 0x798u) = normals ? 0x112u : 0x142u;
            *recomp_memory_u32(DEVICE + 0xf88u) = *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
            recomp_guest_store(DEVICE + 0xde0u, singular, sizeof singular);
            for (uint32_t stage = 0u; stage < 3u; ++stage)
                for (uint32_t i = 0u; i < 6u; ++i) set_stage(stage, fields[i], reflection[stage][i]);
            for (uint32_t stage = 0u; stage < 2u; ++stage) {
                set_stage(stage, 21u, (generated & (1u << stage)) ? 2u : 0u);
                set_stage(stage, 28u, (generated & (1u << stage)) ? 0x30000u : 0u);
            }
            if (normals && generated) {
                if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 32u); return 1; }
                CHECK(strncmp(stop_reason, "d3d8:reflection-normal:", 22u) == 0 && submits == 0u);
            } else {
                invoke(0x348240u, 5u, 3u, VERTICES, normals ? 32u : 24u);
                CHECK(submits == 1u && submitted.data.draw.has_reflection &&
                    submitted.data.draw.reflection_world_view[12] == 5.0f &&
                    submitted.data.draw.reflection_normal[0] == 0.0f);
            }
        }
        /* A secondary blend influence follows the same consumed-normal rule. */
        initialize();
        *recomp_memory_u32(DEVICE + 0x798u) = normals ? 0x116u : 0x146u;
        *recomp_memory_u32(0x355484u) = 1u;
        *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
        set_stage(0u, 21u, 2u); set_stage(0u, 28u, 0x30000u);
        recomp_guest_store(DEVICE + 0xe20u, singular, sizeof singular);
        if (normals) {
            if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 36u); return 1; }
            CHECK(strstr(stop_reason, "influence=1/1") != NULL && submits == 0u);
        } else {
            invoke(0x348240u, 5u, 3u, VERTICES, 28u);
            CHECK(submitted.data.draw.texgen_reflection &&
                submitted.data.draw.reflection_blend_world_view[0][12] == 5.0f);
        }
        *recomp_memory_u32(DEVICE + 0xe20u) = 0x7fc00000u;
        /* A NaN skinning matrix drops the draw, as NV2A clips NaN vertices. */
        if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected NaN stop: %s\n", stop_reason); return 1; }
        const uint32_t before = submits;
        invoke(0x348240u, 5u, 3u, VERTICES, normals ? 36u : 28u);
        CHECK(submits == before);
    }
    /* Normalized, 0x34b530 never divides: a subnormal determinant whose
       inverse overflows still uploads a finite adjugate. */
    initialize();
    *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
    *recomp_memory_u32(DEVICE + 0xf88u) = *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
    *recomp_memory_u32(0x355498u) = 1u;
    const float thin[16] = {1e-39f,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1};
    recomp_guest_store(DEVICE + 0xde0u, thin, sizeof thin);
    for (uint32_t stage = 0u; stage < 3u; ++stage)
        for (uint32_t i = 0u; i < 6u; ++i) set_stage(stage, fields[i], reflection[stage][i]);
    set_stage(0u, 21u, 2u); set_stage(0u, 28u, 0x30000u);
    invoke(0x348240u, 5u, 3u, VERTICES, 32u);
    CHECK(submits == 1u && submitted.data.draw.reflection_normal[0] == 1.0f &&
        submitted.data.draw.reflection_normal[5] == 1e-39f);
    return 0;
}

static int reflection_coverage_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected coverage stop: %s\n", stop_reason); return 1; }
    for (uint32_t stage = 0u; stage < 2u; ++stage) {
        initialize();
        *recomp_memory_u32(DEVICE + 0x798u) = 0x112u;
        *recomp_memory_u32(DEVICE + 0xf88u) = *recomp_memory_u32(DEVICE + 0xf8cu) = 0x7000u;
        const float collapsed[16] = {0,0,0,0, 0,1,0,0, 0,0,0,0, 5,6,7,1};
        recomp_guest_store(DEVICE + 0xde0u, collapsed, sizeof collapsed);
        const uint32_t reflection[3][6] = {{2,2,1,4,2,0}, {14,2,1,14,2,1}, {4,0,1,4,0,1}};
        const uint32_t fields[] = {12,14,15,16,18,19};
        for (uint32_t st = 0u; st < 3u; ++st)
            for (uint32_t f = 0u; f < 6u; ++f) set_stage(st, fields[f], reflection[st][f]);
        set_stage(stage, 21u, 2u); set_stage(stage, 28u, 0x30000u);
        /* Every vertex on one line: no triangle has area, so no query runs. */
        covered = true;
        invoke(0x348240u, 6u, 3u, VERTICES, 32u);
        CHECK(submits == 0u && coverage_queries == 0u && recomp_runtime.registers.esp == STACK + 20u);
        /* Issue #32: the same squash after rounding, rows 1 and 2 parallel to float precision. */
        const float rounded[16] = {0,0,0,0, 0.164432883f,0.923869848f,0.0500328951f,0,
            -0.0598328076f,-0.336171985f,-0.0182056576f,0, -1.97934079f,1.32895517f,0.295400441f,1};
        recomp_guest_store(DEVICE + 0xde0u, rounded, sizeof rounded);
        invoke(0x348240u, 6u, 3u, VERTICES, 32u);
        CHECK(submits == 0u && coverage_queries == 0u && recomp_runtime.registers.esp == STACK + 20u);
        /* Rows apart by a few times the allowance keep area and need the query. */
        float apart[16];
        memcpy(apart, rounded, sizeof apart);
        apart[8] *= 1.000002f;
        recomp_guest_store(DEVICE + 0xde0u, apart, sizeof apart);
        covered = false;
        invoke(0x348240u, 6u, 3u, VERTICES, 32u);
        CHECK(submits == 0u && coverage_queries == 1u && recomp_runtime.registers.esp == STACK + 20u);
        coverage_queries = 0u;
        recomp_guest_store(DEVICE + 0xde0u, collapsed, sizeof collapsed);
        /* A flattened plane keeps area; only raster coverage proves it unused. */
        *recomp_memory_u32(DEVICE + 0xde0u + 40u) = 0x3f800000u;
        covered = false;
        invoke(0x348240u, 6u, 3u, VERTICES, 32u);
        CHECK(submits == 0u && coverage_queries == 1u && recomp_runtime.registers.esp == STACK + 20u);
        covered = true;
        if (setjmp(stopped) == 0) { invoke(0x348240u, 6u, 3u, VERTICES, 32u); return 1; }
        CHECK(strncmp(stop_reason, "d3d8:reflection-normal:", 22u) == 0 && submits == 0u && coverage_queries == 2u);
        /* An expanding mesh renders normally, without a synchronous query. */
        *recomp_memory_u32(DEVICE + 0xde0u) = *recomp_memory_u32(DEVICE + 0xde0u + 40u) = 0x3d4ccccdu;
        invoke(0x348240u, 6u, 3u, VERTICES, 32u);
        CHECK(submits == 1u && coverage_queries == 2u);
    }
    return 0;
}

static int frame_buffer_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected frame-buffer stop: %s\n", stop_reason); return 1; }
    initialize();
    *recomp_memory_u32(DEVICE + 0xf88u) = 0x7000u;
    *recomp_memory_u32(DEVICE + 0x1a24u + 4u) = 0x10000u;
    *recomp_memory_u32(DEVICE + 0x1a24u + 24u + 4u) = texture.data;
    CHECK(recomp_d3d8_state_set_simple(0x40304u, 1u));
    CHECK(recomp_d3d8_state_set_simple(0x40344u, 0x8003u));
    CHECK(recomp_d3d8_state_set_simple(0x40348u, 0x8004u));
    CHECK(recomp_d3d8_state_set_simple(0x4034cu, 0x80ff0000u));
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.texture_is_backbuffer && submitted.data.draw.texture_is_front_buffer);
    CHECK(submitted.data.draw.blend.src_factor == RECOMP_D3D_BLEND_CONSTANT_ALPHA &&
        submitted.data.draw.blend.dst_factor == RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA &&
        submitted.data.draw.blend.constant_color == 0x80ff0000u);
    texture.data = 0x10000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.texture_is_backbuffer && !submitted.data.draw.texture_is_front_buffer);
    texture.data = 0x12000u;
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(!submitted.data.draw.texture_is_backbuffer && !submitted.data.draw.texture_is_front_buffer);
    return 0;
}

static int depth_bias_tests(void)
{
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected depth bias stop: %s\n", stop_reason); return 1; }
    initialize();
    /* 5849 ZBias(1): receiver slope -1/4, units -1, offset fill enabled. */
    CHECK(recomp_d3d8_state_set_simple(0x40384u, 0xbe800000u));
    CHECK(recomp_d3d8_state_set_simple(0x40388u, 0xbf800000u));
    CHECK(recomp_d3d8_state_set_simple(0x40338u, 1u));
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.depth_bias == -1 && submitted.data.draw.slope_scaled_depth_bias == -0.25f);
    CHECK(recomp_d3d8_state_set_simple(0x40338u, 0u));
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.depth_bias == 0 && submitted.data.draw.slope_scaled_depth_bias == 0.0f);
    /* Non-polygons do not inherit fill offset, even when expanded into quads. */
    CHECK(recomp_d3d8_state_set_simple(0x40338u, 1u));
    invoke(0x348240u, 2u, 2u, VERTICES, 28u);
    CHECK(submitted.data.draw.depth_bias == 0 && submitted.data.draw.slope_scaled_depth_bias == 0.0f);
    CHECK(recomp_d3d8_state_set_simple(0x40388u, 0u));
    CHECK(recomp_d3d8_state_set_simple(0x40384u, 0x3f800000u));
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.depth_bias == 0 && submitted.data.draw.slope_scaled_depth_bias == 1.0f);
    CHECK(recomp_d3d8_state_set_simple(0x40384u, 0x7fc00000u));
    if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:draw-depth-bias", 20u) == 0);
    CHECK(recomp_d3d8_state_set_simple(0x40384u, 0u));
    const uint32_t invalid_units[] = {0x3f000000u, 0x7f800000u, 0x4f000000u};
    for (unsigned i = 0; i < sizeof invalid_units / sizeof invalid_units[0]; ++i) {
        CHECK(recomp_d3d8_state_set_simple(0x40388u, invalid_units[i]));
        if (setjmp(stopped) == 0) { invoke(0x348240u, 5u, 3u, VERTICES, 28u); return 1; }
        CHECK(strncmp(stop_reason, "d3d8:draw-depth-bias", 19u) == 0);
    }
    CHECK(recomp_d3d8_state_set_simple(0x40338u, 0u));
    invoke(0x348240u, 5u, 3u, VERTICES, 28u);
    CHECK(submitted.data.draw.depth_bias == 0 && submitted.data.draw.slope_scaled_depth_bias == 0.0f);
    return 0;
}

int main(void)
{
    initialize();
    if (pixel_tests() || draw_tests() || line_tests() || quad_tests() || point_tests() || lighting_tests() || filter_tests() || bright_tests() ||
        distortion_tests() || factor_tests() || position_texgen_tests() || effect_tests() || program_tests() || bump_tests() ||
        volume_tests() || reflection_inverse_tests() || reflection_coverage_tests() || frame_buffer_tests() || depth_bias_tests()) return 1;
    puts("PASS D3D8 pixel lifecycle/constants and semantic draw ABI/snapshots");
    return 0;
}
