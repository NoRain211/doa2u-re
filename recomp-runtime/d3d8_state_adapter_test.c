/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_state_adapter.h"
#include "d3d8_adapter.h"
#include "d3d_light_model.h"

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL D3D state line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

enum { DEVICE = 0x00355500u, STACK = 0x1000u, INPUT = 0x2000u, DIRTY = 0x00355058u };
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[0x400000u];
static uint32_t allocated = 0x00360000u, freed, submits;
static bool fail_allocation;
static RecompD3dPresenterCommand submitted;
static jmp_buf stopped;
static char stop_reason[128];

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
    if (address > sizeof ram || size > sizeof ram - address) abort();
    return ram + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void recomp_guest_load(void *to, uint32_t from, size_t size)
{
    memcpy(to, recomp_memory(from, size), size);
}
void recomp_guest_store(uint32_t to, const void *from, size_t size)
{
    memcpy(recomp_memory(to, size), from, size);
}
void recomp_guest_memcpy(uint32_t to, uint32_t from, size_t size)
{
    memcpy(recomp_memory(to, size), recomp_memory(from, size), size);
}
uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    uint32_t result = allocated;
    if (fail_allocation || size > sizeof ram - allocated) return 0u;
    allocated += size;
    return result;
}
void recomp_kernel_free_pool(uint32_t address) { freed = address; }
uint32_t recomp_d3d8_require_device(void) { return DEVICE; }
void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    submitted = *command;
    ++submits;
}

static uint32_t invoke(uint32_t address, uint32_t a, uint32_t b)
{
    RecompFunction function = recomp_d3d8_state_lookup_manual(address);
    if (function == NULL) abort();
    recomp_runtime.registers.esp = STACK;
    *recomp_memory_u32(STACK) = 0x12345678u;
    *recomp_memory_u32(STACK + 4u) = a;
    *recomp_memory_u32(STACK + 8u) = b;
    recomp_runtime.registers.eax = 0x11223344u;
    function();
    return recomp_runtime.registers.eax;
}

static RecompD3dLightRecord record(uint32_t index)
{
    RecompD3dLightRecord result;
    recomp_guest_load(&result, *recomp_memory_u32(DEVICE + 0x7a4u) +
        index * sizeof result, sizeof result);
    return result;
}

static int check_frame_state(void);

int main(void)
{
    int passed = 1;
    CHECK(recomp_d3d8_state_lookup_manual(0u) == NULL);
    for (uint32_t i = 0u; i < 768u; ++i) ram[INPUT + i] = (uint8_t)i;
    *recomp_memory_u32(DEVICE + 0x2478u) = 1u;
    CHECK(invoke(0x344180u, 1u, INPUT) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u && submits == 1u);
    CHECK(submitted.type == RECOMP_D3D_PRESENTER_COMMAND_GAMMA);
    CHECK(memcmp(submitted.data.gamma, ram + INPUT, 768u) == 0);
    CHECK(*recomp_memory_u32(DEVICE + 0x240cu) == 1u);
    CHECK(*recomp_memory_u32(DEVICE + 0x2408u) == 1u);
    CHECK(memcmp(ram + DEVICE + 0x2104u, ram + INPUT, 768u) == 0);
    invoke(0x344180u, 2u, INPUT);
    CHECK(submits == 1u);
    ram[INPUT] = 123u;
    *recomp_memory_u32(DEVICE + 0x2478u) = 2u;
    invoke(0x344180u, 2u, INPUT);
    CHECK(submits == 2u && *recomp_memory_u32(DEVICE + 0x2404u) == 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x240cu) == 0u);

    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    recomp_guest_store(INPUT, matrix, sizeof matrix);
    CHECK(invoke(0x344210u, 9u, INPUT) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    CHECK(memcmp(ram + DEVICE + 0xea0u, matrix, sizeof matrix) == 0);
    memset(ram + INPUT, 0, sizeof matrix);
    CHECK(invoke(0x344330u, 9u, INPUT) == 0x11223344u);
    CHECK(memcmp(ram + INPUT, matrix, sizeof matrix) == 0);
    CHECK(*recomp_memory_u32(DIRTY) == 0x200u);
    invoke(0x344210u, 1u, INPUT);
    float derived[3];
    recomp_guest_load(derived, DEVICE + 0x93cu, sizeof derived);
    CHECK(derived[0] == 1.0f && derived[1] == 1.0f && derived[2] == 1.0f);
    CHECK((*recomp_memory_u32(DEVICE + 8u) & 2u) != 0u);
    matrix[10] = 2.0f; matrix[11] = 1.0f; matrix[14] = -4.0f; matrix[15] = 0.0f;
    recomp_guest_store(INPUT, matrix, sizeof matrix);
    invoke(0x344210u, 1u, INPUT);
    recomp_guest_load(derived, DEVICE + 0x93cu, sizeof derived);
    CHECK(derived[0] == 2.0f && derived[1] == 4.0f && derived[2] == 0.25f);
    CHECK((*recomp_memory_u32(DEVICE + 8u) & 2u) == 0u);
    matrix[10] = matrix[11];
    recomp_guest_store(INPUT, matrix, sizeof matrix);
    invoke(0x344210u, 1u, INPUT);
    recomp_guest_load(derived, DEVICE + 0x93cu, sizeof derived);
    CHECK(derived[0] == 0.0f && derived[1] == 1.0f && derived[2] == 1.0f);
    if (setjmp(stopped) == 0) { invoke(0x344210u, 10u, INPUT); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-transform:10") == 0);
    CHECK(invoke(0x3465c0u, 0xaabbccddu, 0u) == 0x11223344u);
    CHECK(*recomp_memory_u32(0x355488u) == 0xaabbccddu);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);

    CHECK(invoke(0x345ea0u, 0xbf000000u, 0x3e800000u) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    float offsets[2];
    recomp_guest_load(offsets, DEVICE + 0xef8u, sizeof offsets);
    CHECK(offsets[0] == 0.03125f && offsets[1] == 0.78125f);
    if (setjmp(stopped) == 0) { invoke(0x345ea0u, 0u, 0x7fc00000u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:screen-space-offset") == 0);
    CHECK(*recomp_memory_u32(DEVICE + 0xef8u) == 0x3d000000u);
    CHECK(invoke(0x345ea0u, 0u, 0u) == 0x11223344u);
    recomp_guest_load(offsets, DEVICE + 0xef8u, sizeof offsets);
    CHECK(offsets[0] == 0.53125f && offsets[1] == 0.53125f);

    int32_t scissor[4] = {12, 8, 120, 80};
    recomp_guest_store(INPUT, scissor, sizeof scissor);
    *recomp_memory_u32(STACK + 12u) = INPUT;
    CHECK(invoke(0x3459f0u, 1u, 0u) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u);
    CHECK(*recomp_memory_u32(DEVICE + 0x1c00u) == 1u &&
        *recomp_memory_u32(DEVICE + 0x1c04u) == 0u);
    CHECK(memcmp(ram + DEVICE + 0x1b80u, scissor, sizeof scissor) == 0);
    *recomp_memory_u32(INPUT + 8u) = 1u;
    if (setjmp(stopped) == 0) { invoke(0x3459f0u, 1u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:scissor-rectangle") == 0);
    CHECK(memcmp(ram + DEVICE + 0x1b80u, scissor, sizeof scissor) == 0);
    if (setjmp(stopped) == 0) { invoke(0x3459f0u, 2u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-scissors:2:0") == 0);
    if (setjmp(stopped) == 0) { invoke(0x3459f0u, 1u, 1u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-scissors:1:1") == 0);
    *recomp_memory_u32(STACK + 12u) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x3459f0u, 1u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:scissor-pointer") == 0);
    CHECK(invoke(0x3459f0u, 0u, 0u) == 0x11223344u);
    CHECK(*recomp_memory_u32(DEVICE + 0x1c00u) == 0u);

    CHECK(invoke(0x345300u, 3u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    RecompD3dLightRecord light = record(3u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a8u) == 16u);
    CHECK(light.desc.type == 3u && light.flags == 7u && light.slot == UINT32_MAX);
    CHECK(light.desc.diffuse[3] == 0.0f && light.normalized_direction[2] == -1.0f);
    invoke(0x345300u, 0u, 1u);
    invoke(0x345300u, 1u, 1u);
    CHECK(record(0u).slot == 0u && record(1u).slot == 1u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7ccu) == 5u);
    uint32_t old_array = *recomp_memory_u32(DEVICE + 0x7a4u);
    RecompD3dLightDesc desc;
    recomp_d3d_light_default(&desc);
    desc.type = 1u;
    recomp_guest_store(INPUT, &desc, sizeof desc);
    CHECK(invoke(0x345040u, 16u, INPUT) == 0u);
    uint32_t new_array = *recomp_memory_u32(DEVICE + 0x7a4u);
    CHECK(freed == old_array && new_array != old_array);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a8u) == 32u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7acu) == new_array);
    CHECK(*recomp_memory_u32(DEVICE + 0x7b0u) == new_array + sizeof light);
    CHECK(record(0u).slot == 0u && record(16u).desc.type == 1u);
    CHECK(invoke(0x345040u, 0u, INPUT) == 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7ccu) == 6u);
    *recomp_memory_u32(DIRTY) = 0u;
    invoke(0x345300u, 0u, 1u);
    CHECK(*recomp_memory_u32(DIRTY) == 0u);
    invoke(0x345300u, 0u, 0u);
    CHECK(record(0u).slot == UINT32_MAX && *recomp_memory_u32(DEVICE + 0x7acu) == 0u);
    CHECK(*recomp_memory_u32(DEVICE + 0x7ccu) == 4u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x10000u);
    fail_allocation = true;
    CHECK(invoke(0x345040u, 32u, INPUT) == 0x8007000eu);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a4u) == new_array);
    CHECK(*recomp_memory_u32(DEVICE + 0x7a8u) == 32u);
    CHECK(invoke(0x345300u, UINT32_MAX, 0u) == 0x8007000eu);
    fail_allocation = false;
    desc.type = 2u;
    recomp_guest_store(INPUT, &desc, sizeof desc);
    if (setjmp(stopped) == 0) { invoke(0x345040u, 0u, INPUT); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-spot-light") == 0);
    for (uint32_t i = 0u; i < 8u; ++i) invoke(0x345300u, i, 1u);
    if (setjmp(stopped) == 0) { invoke(0x345300u, 8u, 1u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-active-light-count") == 0);

    /* SDK defaults: simple states reach the model, deferred set dirty bits,
       and every replayed state lands in the shadow the game reads back. */
    for (uint32_t state = 0x39u; state < 0x5cu; ++state)
        *recomp_memory_u32(0x3c18d0u + state * 4u) = 0x40000u + state * 4u;
    *recomp_memory_u32(0x3c18d0u + 0x43u * 4u) = 0x40358u;
    *recomp_memory_u32(0x3536e0u + (0x43u - 0x39u) * 4u) = 0x01010101u;
    *recomp_memory_u32(0x3536e0u + (0x66u - 0x39u) * 4u) = 1u;
    *recomp_memory_u32(0x3c16b0u + 0x66u * 4u) = 0x1200u;
    *recomp_memory_u32(0x3536e0u + (0x7eu - 0x39u) * 4u) = 0xdeadbeefu;
    *recomp_memory_u32(0x3536e0u + (0x93u - 0x39u) * 4u) = 0x901u;
    *recomp_memory_u32(DIRTY) = 0u;
    recomp_d3d8_state_reset();
    recomp_d3d8_state_write_defaults(true);
    uint32_t mask = 0u;
    CHECK(recomp_d3d_get_simple_render_state(recomp_d3d8_state_model(), 0x40358u, &mask) &&
        mask == 0x01010101u && *recomp_memory_u32(0x35536cu) == 0x01010101u);
    CHECK(*recomp_memory_u32(0x3553f8u) == 1u && *recomp_memory_u32(DIRTY) == 0x1200u);
    CHECK(*recomp_memory_u32(0x355458u) == 0u && *recomp_memory_u32(0x35549cu) == 1u);
    CHECK(*recomp_memory_u32(0x3554acu) == 0x901u);
    *recomp_memory_u32(0x3c18d0u + 0x43u * 4u) = 0u;
    if (setjmp(stopped) == 0) { recomp_d3d8_state_write_defaults(true); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:default-simple-state:43:00000000") == 0);
    /* BumpEnv stores BUMPENVMAT01 for stage 1 and pops three arguments. */
    *recomp_memory_u32(STACK + 12u) = 0x3f000000u;
    CHECK(invoke(0x346bc0u, 1u, 23u) == 0u && recomp_runtime.registers.esp == STACK + 16u);
    CHECK(*recomp_memory_u32(0x355060u + (32u + 23u) * 4u) == 0x3f000000u);
    if (setjmp(stopped) == 0) { invoke(0x346bc0u, 1u, 21u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-bump-env:1:21") == 0);
    /* BorderColor shadows stage 2's D3DTSS_BORDERCOLOR and pops two arguments. */
    CHECK(invoke(0x346c30u, 2u, 0xff336699u) == 0u && recomp_runtime.registers.esp == STACK + 12u);
    CHECK(*recomp_memory_u32(0x355060u + (64u + 29u) * 4u) == 0xff336699u);
    passed &= check_frame_state() == 0;
    if (passed) puts("PASS D3D8 gamma, transform, fog, light, shader and frame-state ABI/shadow state");
    return passed ? 0 : 1;
}

static int check_frame_state(void)
{
    /* SetRenderState_Simple: fastcall, plain ret, caller owns its shadows. */
    recomp_runtime.registers.ecx = 0x4035cu;
    recomp_runtime.registers.edx = 1u;
    CHECK(invoke(0x346240u, 0u, 0u) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 4u);

    CHECK(invoke(0x347490u, 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x35549cu) == 1u && recomp_runtime.registers.esp == STACK + 8u);
    CHECK(invoke(0x347520u, 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554a0u) == 1u);
    CHECK(invoke(0x346930u, 0x1b02u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x35548cu) == 0x1b02u);
    if (setjmp(stopped) == 0) { invoke(0x346930u, 0x1b03u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:unsupported-fill-mode", 26) == 0);
    CHECK(invoke(0x346540u, 0u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554bcu) == 0u);
    /* ShadowFunc is void/stdcall and preserves the state read by save/restore. */
    CHECK(recomp_d3d8_state_lookup_manual(0x346580u) != NULL);
    *recomp_memory_u32(0x3554d0u) = 0x207u;
    *recomp_memory_u32(DIRTY) = 0x1234u;
    *recomp_memory_u32(DEVICE) = 0xdeadbeefu;
    CHECK(invoke(0x346580u, 0x200u, 0u) == 0x11223344u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(*recomp_memory_u32(0x3554d0u) == 0x200u &&
        *recomp_memory_u32(DIRTY) == 0x1234u && *recomp_memory_u32(DEVICE) == 0xdeadbeefu);
    if (setjmp(stopped) == 0) { invoke(0x346580u, 0x207u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-shadow-func:00000207") == 0);
    CHECK(*recomp_memory_u32(0x3554d0u) == 0x200u);
    CHECK(invoke(0x346610u, 0x901u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554acu) == 0x901u);
    CHECK(invoke(0x347800u, 0u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554c0u) == 0u);
    if (setjmp(stopped) == 0) { invoke(0x347800u, 0x11u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:unsupported-state-value", 28) == 0);
    CHECK(invoke(0x3477a0u, 2u, 0u) == 0u && *recomp_memory_u32(0x3554c8u) == 2u);
    CHECK(invoke(0x3477d0u, 1u, 0u) == 0u && *recomp_memory_u32(0x3554ccu) == 1u);
    if (setjmp(stopped) == 0) { invoke(0x3477a0u, 3u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:unsupported-state-value:003554c8", 37) == 0);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x3466c0u, 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x355498u) == 1u && *recomp_memory_u32(DIRTY) == 0x200u);

    /* SetMaterial copies 17 DWORDs into the device shadow and dirties it. */
    for (uint32_t i = 0u; i < 68u; ++i) ram[INPUT + i] = (uint8_t)(0xc0u + i);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x3443a0u, INPUT, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(memcmp(ram + DEVICE + 0xf00u, ram + INPUT, 68u) == 0);
    CHECK(*recomp_memory_u32(DIRTY) == 0x9000u);
    if (setjmp(stopped) == 0) { invoke(0x3443a0u, 0u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:material-pointer") == 0);

    CHECK(invoke(0x346760u, 0x40000000u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554d4u) == 0x40000000u);
    if (setjmp(stopped) == 0) { invoke(0x346760u, 0xbf800000u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:unsupported-line-width:bf800000") == 0);
    CHECK(invoke(0x346760u, 0x3f800000u, 0u) == 0u);
    CHECK(*recomp_memory_u32(0x3554d4u) == 0x3f800000u);
    CHECK(invoke(0x346700u, 0xff123456u, 0u) == 0u);
    CHECK(recomp_d3d8_state_model()->texture_factor == 0xff123456u);
    CHECK(recomp_d3d8_state_set_simple(0x40304u, 1u));
    CHECK(recomp_d3d8_state_set_simple(0x40344u, 0x302u));
    CHECK(recomp_d3d8_state_set_simple(0x40348u, 0x303u));
    CHECK(!recomp_d3d8_state_set_simple(0x40345u, 0u));
    RecompD3dBlendState blend;
    recomp_d3d_blend_state(recomp_d3d8_state_model(), &blend);
    CHECK(blend.blend_enable && blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
        blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA);
    /* Great Opera's floor shadow layer: ONE/ONE with REVSUBTRACTSIGNED darkens. */
    CHECK(recomp_d3d8_state_set_simple(0x40344u, 1u));
    CHECK(recomp_d3d8_state_set_simple(0x40348u, 1u));
    CHECK(recomp_d3d8_state_set_simple(0x40350u, 0xf005u));
    recomp_d3d_blend_state(recomp_d3d8_state_model(), &blend);
    CHECK(blend.src_factor == RECOMP_D3D_BLEND_ONE && blend.dst_factor == RECOMP_D3D_BLEND_ONE &&
        blend.op == RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT);
    CHECK(recomp_d3d8_state_set_simple(0x40350u, 0x8006u));
    CHECK(recomp_d3d8_state_set_simple(0x40344u, 0x302u));
    CHECK(recomp_d3d8_state_set_simple(0x40348u, 0x303u));
    RecompD3dDepthState depth;
    recomp_d3d_depth_state(recomp_d3d8_state_model(), &depth);
    CHECK(depth.depth_write_enable && depth.depth_test_enable && depth.stencil_enable);
    if (setjmp(stopped) != 0) { fprintf(stderr, "Unexpected blend stop: %s\n", stop_reason); return 1; }
    const uint32_t blend_modes[] = {0u, 1u, 3u, 5u};
    for (uint32_t i = 0u; i < 4u; ++i) {
        CHECK(invoke(0x346a60u, blend_modes[i], 0u) == 0u);
        CHECK(*recomp_memory_u32(0x355484u) == blend_modes[i]);
    }
    for (uint32_t mode = 2u; mode <= 6u; mode += 2u) {
        if (setjmp(stopped) == 0) { invoke(0x346a60u, mode, 0u); return 1; }
        CHECK(*recomp_memory_u32(0x355484u) == 5u);
    }
    /* SetVertexShader binds the FVF declaration and latches device shadows. */
    *recomp_memory_u32(DEVICE + 0x794u) = 0x357b80u;
    *recomp_memory_u32(0x357b80u + 4u) = 0x402u;
    *recomp_memory_u32(0x357b80u + 0x10u) = 0x44u;
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x349320u, 0x112u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(*recomp_memory_u32(DEVICE + 0x794u) == 0x357b80u);
    CHECK(*recomp_memory_u32(DEVICE + 0x798u) == 0x112u);
    CHECK(*recomp_memory_u32(DEVICE + 0x79cu) == 0u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x1670u);
    CHECK(*recomp_memory_u32(0x357b80u + 0x1cu) == 0x32u);
    CHECK(*recomp_memory_u32(0x357b80u + 0x38u) == 0x0cu);
    CHECK(*recomp_memory_u32(0x357b80u + 0xa8u) == 0x18u);
    CHECK(*recomp_memory_u32(0x357b80u + 0x10u) == 2u);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x349320u, 0x112u, 0u) == 0u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x70u);
    /* A program handle loads its code at slot 0 and selects its object. */
    enum { PROGRAM = 0x358000u };
    const uint32_t upload[5] = {(4u << 18u) | 0xb00u, 1u, 2u, 3u, 4u};
    *recomp_memory_u32(PROGRAM + 4u) = 0x10u;
    *recomp_memory_u32(PROGRAM + 8u) = 1u;
    *recomp_memory_u32(PROGRAM + 0xcu) = 5u;
    recomp_guest_store(PROGRAM + 0x114u, upload, sizeof upload);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x349320u, PROGRAM + 1u, 0u) == 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(*recomp_memory_u32(DEVICE + 0x794u) == PROGRAM);
    CHECK(*recomp_memory_u32(DEVICE + 0x798u) == PROGRAM + 1u);
    CHECK(*recomp_memory_u32(DEVICE + 0x79cu) == 0u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x1070u);
    CHECK(memcmp(ram + DEVICE + 0x10a8u, upload + 1, 16u) == 0);
    *recomp_memory_u32(DIRTY) = 0u;
    CHECK(invoke(0x349320u, PROGRAM + 1u, 0u) == 0u);
    CHECK(*recomp_memory_u32(DIRTY) == 0x70u);
    *recomp_memory_u32(PROGRAM + 0x114u) = (4u << 18u) | 0xb80u;
    if (setjmp(stopped) == 0) { invoke(0x349320u, PROGRAM + 1u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:vertex-program-unsupported:00358001") == 0);
    *recomp_memory_u32(DEVICE + 0x794u) = 0x357b80u;
    float values[16];
    for (uint32_t i = 0u; i < 16u; ++i) values[i] = (float)i;
    recomp_guest_store(INPUT, values, sizeof values);
    recomp_runtime.registers.ecx = 96u;
    recomp_runtime.registers.edx = INPUT;
    recomp_runtime.registers.esp = STACK;
    recomp_d3d8_state_lookup_manual(0x348e40u)();
    CHECK(recomp_runtime.registers.esp == STACK + 4u);
    CHECK(memcmp(ram + 0x354440u + 96u * 16u, values, sizeof values) == 0);
    recomp_runtime.registers.ecx = 191u;
    recomp_d3d8_state_lookup_manual(0x348de0u)();
    CHECK(memcmp(ram + 0x354440u + 191u * 16u, values, 16u) == 0);
    recomp_runtime.registers.ecx = 189u;
    if (setjmp(stopped) == 0) { recomp_d3d8_state_lookup_manual(0x348e40u)(); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:vertex-constant-range:189") == 0);
    if (setjmp(stopped) == 0) { invoke(0x349320u, 0x902u, 0u); return 1; }
    CHECK(strncmp(stop_reason, "d3d8:invalid-fvf", 16) == 0);
    *recomp_memory_u32(DEVICE + 0x794u) = 0u;
    if (setjmp(stopped) == 0) { invoke(0x349320u, 0x112u, 0u); return 1; }
    CHECK(strcmp(stop_reason, "d3d8:missing-declaration") == 0);
    return 0;
}
