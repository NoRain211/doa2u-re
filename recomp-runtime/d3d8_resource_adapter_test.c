/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_resource_adapter.h"
#include "d3d8_adapter.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL resource line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

enum { DEVICE = 0x355500u, STACK = 0x1000u, TEXTURE = 0x2000u, OUTPUT = 0x3000u, DATA = 0x10000u };
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[0x400000u];
static uint32_t allocated = 0x380004u, free_count;
static uint32_t invalidations, invalidated_base, invalidated_size;
static uint32_t releases, released_base, released_size;
static bool fail_allocation;
static jmp_buf stopped;
static char reason[128];

void recomp_stop(int code, const char *format, ...)
{
    va_list arguments;
    (void)code;
    va_start(arguments, format);
    vsnprintf(reason, sizeof reason, format, arguments);
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
uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    uint32_t result = allocated;
    if (fail_allocation || size > sizeof ram - allocated) return 0u;
    allocated = (allocated + size + 3u) & ~3u;
    return result;
}
void recomp_kernel_free_pool(uint32_t address)
{
    if (address == 0u) abort();
    ++free_count;
}
uint32_t recomp_d3d8_require_device(void) { return DEVICE; }
void recomp_d3d8_release_memory(uint32_t base, uint32_t size)
{
    ++releases;
    released_base = base;
    released_size = size;
}
void recomp_d3d8_invalidate_textures(uint32_t base, uint32_t size)
{
    ++invalidations;
    invalidated_base = base;
    invalidated_size = size;
}

static uint32_t word(uint32_t address) { return *recomp_memory_u32(address); }
static void put(uint32_t address, uint32_t value) { *recomp_memory_u32(address) = value; }
static uint32_t invoke(uint32_t address, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    RecompFunction function = recomp_d3d8_resource_lookup_manual(address);
    if (function == NULL) abort();
    const uint32_t args[6] = {0x12345678u, a, b, c, d, e};
    memcpy(recomp_memory(STACK, sizeof args), args, sizeof args);
    recomp_runtime.registers.esp = STACK;
    recomp_runtime.registers.eax = 0xaabbccddu;
    function();
    return recomp_runtime.registers.eax;
}

static void texture(uint32_t address, uint32_t common, uint32_t format, uint32_t size)
{
    const uint32_t words[6] = {common, DATA, 0u, format, size, 0u};
    memcpy(recomp_memory(address, sizeof words), words, sizeof words);
}

int main(void)
{
    CHECK(recomp_d3d8_resource_lookup_manual(0u) == NULL);
    CHECK(recomp_d3d8_resource_lookup_manual(0x3443f0u) != NULL);
    ram[0x3530b8u + 6u] = 32u;
    ram[0x3530b8u + 0xcu] = 4u;
    ram[0x3530b8u + 0xfu] = 8u;
    ram[0x3530b8u + 0x12u] = 0xa0u;
    texture(TEXTURE, 0x40001u, 0x04330620u, 0u); /* 8x16, three mip levels. */
    CHECK(invoke(0x347f10u, TEXTURE, 0u, 0u, 0u, 0u) == 3u);
    CHECK(recomp_runtime.registers.esp == STACK + 8u);
    CHECK(invoke(0x347e80u, TEXTURE, 0u, 0u, 0u, 0u) == 2u);
    CHECK(invoke(0x347ec0u, TEXTURE, 0u, 0u, 0u, 0u) == 1u);
    CHECK(invoke(0x347fa0u, TEXTURE, 0x80000000u, 0u, 0u, 0u) == 0xaabbccddu);
    CHECK(word(TEXTURE + 4u) == DATA && recomp_runtime.registers.esp == STACK + 12u);
    /* Register keeps rendered targets; only cached texture uploads are dropped. */
    CHECK(invalidations == 1u && invalidated_base == DATA && invalidated_size == 512u && releases == 0u);
    RecompD3dTextureDesc desc;
    CHECK(recomp_d3d8_resource_describe(TEXTURE, &desc));
    CHECK(desc.width == 8u && desc.height == 16u && desc.mip_levels == 3u && desc.bits_per_pixel == 32u);
    CHECK(!recomp_d3d8_resource_inline_texture(3u, &desc));
    invoke(0x345490u, 3u, TEXTURE, 0u, 0u, 0u);
    const uint32_t retained = word(TEXTURE);
    recomp_runtime.registers.ecx = 0x81bc0u;
    recomp_runtime.registers.edx = DATA + 4096u;
    CHECK(invoke(0x3443f0u, 0x02210f20u, 0u, 0u, 0u, 0u) == 0xaabbccddu);
    CHECK(recomp_runtime.registers.esp == STACK + 8u && recomp_runtime.registers.ecx == 0x02210f20u &&
        recomp_runtime.registers.edx == DATA + 4096u);
    CHECK(recomp_d3d8_resource_inline_texture(3u, &desc) && desc.data == DATA + 4096u &&
        desc.width == 4u && desc.height == 4u && desc.format_byte == 0xfu && desc.mip_levels == 1u);
    CHECK(!recomp_d3d8_resource_inline_texture(0u, &desc) && !recomp_d3d8_resource_inline_texture(3u, NULL));
    CHECK(word(DEVICE + 0xf94u) == TEXTURE && word(TEXTURE) == retained && word(TEXTURE + 4u) == DATA);
    invoke(0x345490u, 3u, TEXTURE, 0u, 0u, 0u);
    CHECK(!recomp_d3d8_resource_inline_texture(3u, &desc) && word(TEXTURE) == retained);
    invoke(0x345490u, 3u, 0u, 0u, 0u, 0u);
    recomp_runtime.registers.ecx = 0x81b00u;
    if (setjmp(stopped) == 0) { invoke(0x3443f0u, 0x02210f20u, 0u, 0u, 0u, 0u); return 1; }
    CHECK(strcmp(reason, "d3d8:inline-texture-method:00081b00") == 0);
    recomp_runtime.registers.ecx = 0x81bc0u;
    if (setjmp(stopped) == 0) { invoke(0x3443f0u, 0x00011220u, 0u, 0u, 0u, 0u); return 1; }
    CHECK(strcmp(reason, "d3d8:inline-texture-format:00011220") == 0);
    CHECK(invoke(0x3480b0u, TEXTURE, 1u, OUTPUT, 0u, 0u) == 0xaabbccddu);
    CHECK(word(OUTPUT) == 16u && word(OUTPUT + 4u) == DATA + 512u);
    CHECK(recomp_runtime.registers.esp == STACK + 24u);
    CHECK(releases == 1u && released_base == DATA + 512u && released_size == 128u);
    invoke(0x3480b0u, TEXTURE, 1u, OUTPUT, 0u, 0x80u);
    CHECK(releases == 1u && invalidations == 1u);

    uint32_t surface = invoke(0x348060u, TEXTURE, 1u, 0u, 0u, 0u);
    CHECK(surface != 0u && recomp_runtime.registers.esp == STACK + 12u);
    CHECK(word(surface) == 0x1050001u && word(surface + 20u) == TEXTURE);
    CHECK((word(TEXTURE) & 0xffffu) == 2u);
    CHECK(invoke(0x348170u, surface, OUTPUT, 0u, 0u, 0u) == 0xaabbccddu);
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    CHECK(word(OUTPUT) == 6u && word(OUTPUT + 4u) == 1u && word(OUTPUT + 12u) == 128u);
    CHECK(word(OUTPUT + 20u) == 4u && word(OUTPUT + 24u) == 8u);
    invoke(0x348190u, surface, OUTPUT, 0u, 0x20u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 20u && word(OUTPUT + 4u) == DATA + 512u);
    recomp_d3d8_resource_set_render_targets(surface, 0u);
    CHECK(word(DEVICE + 0x1a04u) == surface && word(DEVICE + 0x1a08u) == 0u);
    CHECK(word(surface) == 0x10d0001u && word(TEXTURE) == 0xc0002u);
    recomp_d3d8_resource_set_render_targets(surface, 0u);
    CHECK(word(surface) == 0x10d0001u && word(TEXTURE) == 0xc0002u);
    CHECK(invoke(0x347ec0u, surface, 0u, 0u, 0u, 0u) == 0u);
    CHECK(free_count == 0u && word(surface) == 0x10d0000u && word(TEXTURE) == 0xc0001u);
    CHECK(recomp_d3d8_resource_add_ref(surface) == 1u && word(TEXTURE) == 0xc0002u);
    CHECK(invoke(0x347ec0u, surface, 0u, 0u, 0u, 0u) == 0u && word(TEXTURE) == 0xc0001u);
    recomp_d3d8_resource_set_render_targets(0u, 0u);
    CHECK(free_count == 1u && (word(TEXTURE) & 0xffffu) == 1u);
    CHECK(word(TEXTURE) == 0x40001u);

    put(DEVICE + 0xcu, 0x80000000u);
    CHECK(invoke(0x345490u, 0u, TEXTURE, 0u, 0u, 0u) == 0u);
    CHECK(word(TEXTURE) == 0xc0001u && word(DEVICE + 0xf88u) == TEXTURE);
    CHECK((word(0x355058u) & 0x4800u) == 0x4800u);
    CHECK(invoke(0x345460u, 0u, 0u, 0u, 0u, 0u) == TEXTURE);
    CHECK(word(TEXTURE) == 0xc0002u);
    CHECK(invoke(0x347ec0u, TEXTURE, 0u, 0u, 0u, 0u) == 1u);
    invoke(0x345490u, 0u, TEXTURE, 0u, 0u, 0u);
    CHECK(word(TEXTURE) == 0xc0001u); /* Rebinding itself cannot destroy it. */
    put(DEVICE + 0x2cu, 99u);
    invoke(0x345490u, 0u, 0u, 0u, 0u, 0u);
    CHECK(word(TEXTURE) == 0x40001u && word(TEXTURE + 8u) == 99u);
    CHECK(word(DEVICE + 0xcu) == 0x80000000u);

    uint32_t vertex = invoke(0x346140u, 96u, 0u, 0u, 0u, 0u);
    CHECK(vertex != 0u && word(vertex) == 0x1000001u);
    CHECK((word(vertex + 4u) & 255u) == 0u);
    CHECK(invoke(0x346190u, vertex, 0u, 0u, 0u, 0u) == word(vertex + 4u));
    CHECK(recomp_runtime.registers.esp == STACK + 12u);
    invoke(0x348f20u, 0u, vertex, 24u, 0u, 0u);
    CHECK(recomp_runtime.registers.esp == STACK + 16u);
    CHECK(word(0x3579a8u) == 24u && word(0x3579b0u) == vertex);
    CHECK(word(vertex) == 0x1080001u && (word(0x355058u) & 0x70u) == 0x70u);
    CHECK(invoke(0x347ec0u, vertex, 0u, 0u, 0u, 0u) == 0u);
    CHECK(free_count == 1u); /* Device binding keeps its storage alive. */
    invoke(0x348f20u, 0u, 0u, 0u, 0u, 0u);
    CHECK(free_count == 3u && word(0x3579b0u) == 0u);
    fail_allocation = true;
    CHECK(invoke(0x346140u, 96u, 0u, 0u, 0u, 0u) == 0u);
    fail_allocation = false;

    /* A rewind restores the table with guest RAM: a release it undoes runs
       again, and a creation run again is listed once. */
    const size_t state_size = recomp_d3d8_resource_state_size();
    uint8_t *saved = malloc(state_size * 2u), *after = saved + state_size;
    CHECK(saved != NULL);
    vertex = invoke(0x346140u, 96u, 0u, 0u, 0u, 0u);
    recomp_d3d8_resource_state_save(saved);
    uint32_t frees = free_count;
    CHECK(invoke(0x347ec0u, vertex, 0u, 0u, 0u, 0u) == 0u && free_count == frees + 2u);
    recomp_d3d8_resource_state_restore(saved);
    put(vertex, 0x1000001u);
    if (setjmp(stopped) != 0) { fprintf(stderr, "FAIL resource rewind: %s\n", reason); return 1; }
    CHECK(invoke(0x347ec0u, vertex, 0u, 0u, 0u, 0u) == 0u && free_count == frees + 4u);
    recomp_d3d8_resource_state_save(saved);
    CHECK(invoke(0x346140u, 96u, 0u, 0u, 0u, 0u) != 0u);
    recomp_d3d8_resource_state_restore(saved);
    vertex = invoke(0x346140u, 96u, 0u, 0u, 0u, 0u);
    CHECK(invoke(0x347ec0u, vertex, 0u, 0u, 0u, 0u) == 0u);
    recomp_d3d8_resource_state_save(after);
    CHECK(memcmp(saved, after, state_size) == 0);
    free(saved);

    texture(TEXTURE + 32u, 0x10001u, 0u, 0u);
    invoke(0x345700u, TEXTURE + 32u, 7u, 0u, 0u, 0u);
    CHECK(word(DEVICE + 0x7a0u) == TEXTURE + 32u && word(DEVICE + 0x1cu) == 7u);
    CHECK(word(0x355054u) == DATA && word(TEXTURE + 32u) == 0x90001u);
    invoke(0x345700u, 0u, 0u, 0u, 0u, 0u);
    CHECK(word(TEXTURE + 32u) == 0x10001u && word(0x355054u) == 0u);
    texture(TEXTURE + 64u, 0x30001u, 0u, 0u);
    invoke(0x345670u, 0u, TEXTURE + 64u, 0u, 0u, 0u);
    CHECK(word(DEVICE + 0xf98u) == TEXTURE + 64u && word(TEXTURE + 64u) == 0xb0001u);
    invoke(0x345670u, 0u, 0u, 0u, 0u, 0u);
    CHECK(word(TEXTURE + 64u) == 0x30001u);

    texture(TEXTURE, 0x40001u, 0x02230c20u, 0u); /* DXT1 4x4 to 1x1. */
    invoke(0x3480b0u, TEXTURE, 2u, OUTPUT, 0u, 0u);
    CHECK(word(OUTPUT) == 8u && word(OUTPUT + 4u) == DATA + 16u);
    texture(TEXTURE, 0x40001u, 0x00011220u, (3u << 12u) | 7u);
    const uint32_t rect[4] = {1u, 1u, 3u, 3u};
    memcpy(recomp_memory(OUTPUT + 64u, sizeof rect), rect, sizeof rect);
    invoke(0x3480b0u, TEXTURE, 0u, OUTPUT, OUTPUT + 64u, 0u);
    CHECK(word(OUTPUT) == 64u && word(OUTPUT + 4u) == DATA + 68u);
    if (setjmp(stopped) == 0) { invoke(0x3480b0u, TEXTURE, 1u, OUTPUT, 0u, 0u); return 1; }
    CHECK(strncmp(reason, "d3d8:resource-level", 19u) == 0);
    if (setjmp(stopped) == 0) { invoke(0x345490u, 4u, 0u, 0u, 0u, 0u); return 1; }
    CHECK(strcmp(reason, "d3d8:texture-stage:4") == 0);
    if (setjmp(stopped) == 0) { invoke(0x347ec0u, TEXTURE, 0u, 0u, 0u, 0u); return 1; }
    CHECK(strncmp(reason, "d3d8:resource-destroy-unowned", 28u) == 0);
    /* An XPR volume texture (4x4x2 A8R8G8B8, two levels) releases every level. */
    texture(TEXTURE + 96u, 0x40001u, 0x1222063au, 0u);
    CHECK(invoke(0x347f10u, TEXTURE + 96u, 0u, 0u, 0u, 0u) == 4u);
    invoke(0x347fa0u, TEXTURE + 96u, 0x80000000u, 0u, 0u, 0u);
    CHECK(invalidated_base == DATA && invalidated_size == 144u);
    CHECK(!recomp_d3d8_resource_describe(TEXTURE + 96u, &desc));
    texture(TEXTURE + 96u, 0xc0001u, 0x3661063au, 0u);
    CHECK(recomp_d3d8_resource_describe(TEXTURE + 96u, &desc));
    CHECK(desc.width == 64u && desc.height == 64u && desc.format_byte == 6u && desc.volume_depth == 8u);
    if (setjmp(stopped) == 0) { invoke(0x3480b0u, TEXTURE + 96u, 0u, OUTPUT, 0u, 0u); return 1; }
    CHECK(strncmp(reason, "d3d8:resource-level", 18u) == 0);
    const uint8_t swizzled[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    const uint8_t expected[16] = {0,1,8,9,2,3,10,11,4,5,12,13,6,7,14,15};
    uint8_t rows[16];
    CHECK(recomp_d3d_texture_unswizzle_volume(swizzled, rows, 4u, 2u, 2u, 1u));
    CHECK(memcmp(rows, expected, sizeof rows) == 0);
    CHECK(!recomp_d3d_texture_unswizzle_volume(swizzled, rows, 4u, 2u, 3u, 1u));
    CHECK(!recomp_d3d_texture_unswizzle_volume(swizzled, rows, 32768u, 32768u, 8u, 4u));
    CHECK(!recomp_d3d_texture_unswizzle_volume(swizzled, rows, 0x80000000u, 0x80000000u, 0x80000000u, 4u));
    texture(TEXTURE + 96u, 0x40001u, 0x12220c3au, 0u);
    CHECK(!recomp_d3d8_resource_describe(TEXTURE + 96u, &desc));
    if (setjmp(stopped) == 0) { invoke(0x347fa0u, TEXTURE + 96u, 0x80000000u, 0u, 0u, 0u); return 1; }
    CHECK(strcmp(reason, "d3d8:volume-register:12220c3a:00000000") == 0);
    recomp_runtime.registers.ecx = 0x81bc0u;
    recomp_runtime.registers.edx = DATA;
    invoke(0x3443f0u, 0x02210620u, 0u, 0u, 0u, 0u);
    CHECK(word(DEVICE + 0xf94u) == 0u && !recomp_d3d8_resource_inline_texture(3u, &desc));
    invoke(0x345490u, 3u, TEXTURE, 0u, 0u, 0u);
    recomp_runtime.registers.ecx = 0x81bc0u;
    invoke(0x3443f0u, 0x02210620u, 0u, 0u, 0u, 0u);
    CHECK(recomp_d3d8_resource_inline_texture(3u, &desc));
    recomp_d3d8_resource_reset();
    CHECK(!recomp_d3d8_resource_inline_texture(3u, &desc));
    puts("PASS D3D8 resource ABI, mip views, bindings, retention and owned destruction");
    return 0;
}
