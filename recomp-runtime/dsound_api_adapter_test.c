/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_api_adapter.h"
#include "dsound_device_model.h"
#include "audio_output.h"
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t ram[0x800000u];
static uint32_t cursor = 0x700000u, allocation_count, fail_allocation;
static int ready = 1, opened = 1, pump_count, reset_count, shutdown_count;
static int on_owner = 1;
static jmp_buf stopped;
static char stop_reason[128];

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address > sizeof ram || width > sizeof ram - address) abort();
    return ram + address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void recomp_guest_memset(uint32_t address, int value, size_t size)
{
    memset(recomp_memory(address, size), value, size);
}
uint32_t xbox_HeapCheckpoint(void) { return cursor; }
bool xbox_HeapRestore(uint32_t checkpoint) { cursor = checkpoint; return true; }
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t align)
{
    ++allocation_count;
    if (allocation_count == fail_allocation) return 0u;
    const uint32_t result = (cursor + align - 1u) & ~(align - 1u);
    cursor = result + size;
    return result;
}
void recomp_kernel_free_pool(uint32_t address) { (void)address; }
void recomp_audio_output_initialize(void) {}
int recomp_audio_output_enabled(void) { return opened; }
int recomp_audio_output_ready(void) { return ready; }
int recomp_audio_output_on_owner_thread(void) { return on_owner; }
static uint32_t speakers = 2u;
uint32_t recomp_audio_output_speakers(void) { return speakers; }
void recomp_audio_output_shutdown(void) { ++shutdown_count; ready = 0; }
void recomp_dsound_buffers_pump(void) { ++pump_count; }
void recomp_dsound_buffers_reset(void) { ++reset_count; }
RecompFunction recomp_dsound_buffer_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_dsound_effects_lookup_manual(uint32_t address) { (void)address; return NULL; }
void recomp_dsound_effects_reset(void) {}
int recomp_dsound_buffer_addref_root(uint32_t object, uint32_t *count)
{
    (void)object; (void)count; return 0;
}
void recomp_stop(int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(stop_reason, sizeof stop_reason, format, args);
    va_end(args);
    if (code != 2) abort();
    longjmp(stopped, 1);
}

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL dsound API line %d\n", __LINE__); return 1; } } while (0)
static void call(uint32_t address, const uint32_t *args, uint32_t count)
{
    recomp_runtime.registers.esp = 0x1000u;
    recomp_runtime.registers.eax = 0x12345678u;
    memcpy(recomp_memory(0x1004u, count * 4u), args, count * 4u);
    RecompFunction function = recomp_dsound_api_lookup_manual(address);
    if (function == NULL) abort();
    function();
}

int main(void)
{
    const uint32_t output = 0x2000u;
    uint32_t args[5] = {0u, output, 0u, 0u, 0u};
    const uint32_t initial_cursor = cursor;
    ready = 0;
    call(0x366210u, args, 3u);
    CHECK(recomp_runtime.registers.eax == RECOMP_DSOUND_FAIL &&
        recomp_runtime.registers.esp == 0x1010u && *recomp_memory_u32(output) == 0u);
    CHECK(cursor == initial_cursor && *recomp_memory_u32(0x380cc8u) == 0u);
    ready = 1; fail_allocation = 2u;
    call(0x366210u, args, 3u);
    CHECK(recomp_runtime.registers.eax == RECOMP_DSOUND_OUT_OF_MEMORY);
    CHECK(cursor == initial_cursor && recomp_dsound_current_device() == 0u);
    fail_allocation = 0u;
    call(0x366210u, args, 3u);
    const uint32_t handle = *recomp_memory_u32(output), root = handle - 8u;
    const uint32_t settings = *recomp_memory_u32(handle);
    CHECK(recomp_runtime.registers.eax == 0u && recomp_runtime.registers.esp == 0x1010u);
    CHECK(handle == initial_cursor + 8u && *recomp_memory_u32(0x380cc8u) == root);
    CHECK(*recomp_memory_u32(root) == 0x46c1a8u && *recomp_memory_u32(root + 4u) == 6u);
    CHECK(*recomp_memory_u32(root + 0x10u) == root + 0x10u &&
        *recomp_memory_u32(root + 0x14u) == root + 0x10u);
    CHECK(*recomp_memory_u32(settings) == 0x46c234u &&
        *recomp_memory_u32(settings + 8u) == UINT32_MAX &&
        *recomp_memory_u32(settings + 12u) == UINT32_MAX);
    const uint32_t allocated_cursor = cursor;
    call(0x366210u, args, 3u);
    CHECK(cursor == allocated_cursor && *recomp_memory_u32(output) == handle &&
        *recomp_memory_u32(root + 4u) == 7u);
    args[0] = handle;
    call(0x363626u, args, 1u);
    CHECK(recomp_runtime.registers.eax == 6u && recomp_runtime.registers.esp == 0x1008u);
    call(0x363657u, args, 0u);
    CHECK(recomp_runtime.registers.eax == 0x12345678u &&
        recomp_runtime.registers.esp == 0x1004u && *recomp_memory_u32(0x380d1cu) == 3u);
    args[0] = UINT32_MAX; args[1] = UINT32_MAX; args[2] = UINT32_MAX;
    call(0x363621u, args, 3u);
    CHECK(recomp_runtime.registers.eax == 0u && recomp_runtime.registers.esp == 0x1010u);
    args[0] = handle; args[1] = output;
    call(0x364721u, args, 2u);
    CHECK(*recomp_memory_u32(output) == 0u && recomp_runtime.registers.esp == 0x100cu);
    speakers = 6u;
    call(0x364721u, args, 2u);
    CHECK(*recomp_memory_u32(output) == 0x00010002u);
    speakers = 2u;
    call(0x36492cu, args, 0u);
    CHECK(pump_count == 1 && recomp_runtime.registers.eax == 0x12345678u &&
        recomp_runtime.registers.esp == 0x1004u);
    args[0] = root;
    call(0x3636ffu, args, 1u);
    CHECK(recomp_runtime.registers.eax == 7u);
    call(0x365258u, args, 1u);
    CHECK(recomp_runtime.registers.eax == 6u);
    args[0] = handle;
    call(0x363626u, args, 1u);
    CHECK(recomp_runtime.registers.eax == 0u && recomp_dsound_current_device() == 0u &&
        *recomp_memory_u32(0x380cc8u) == 0u && *recomp_memory_u32(handle) == 0u);
    if (setjmp(stopped) == 0) { call(0x363626u, args, 1u); return 1; }
    CHECK(strncmp(stop_reason, "dsound:invalid-device", 21u) == 0);
    args[0] = 0u; args[1] = 0u;
    call(0x366210u, args, 3u);
    CHECK(recomp_runtime.registers.eax == RECOMP_DSOUND_POINTER_ERROR);
    args[1] = output; opened = 0;
    call(0x366210u, args, 3u);
    CHECK(recomp_runtime.registers.eax == 0u && recomp_dsound_current_device() != 0u);
    CHECK(recomp_dsound_api_lookup_manual(0u) == NULL &&
        recomp_dsound_api_lookup_manual(UINT32_MAX) == NULL);
    ready = 0;
    args[0] = recomp_dsound_current_device();
    call(0x363626u, args, 1u);
    CHECK(recomp_runtime.registers.eax == 0u && recomp_dsound_current_device() == 0u);
    ready = 1;
    args[0] = 0u; args[1] = output;
    call(0x366210u, args, 3u);
    on_owner = 0;
    recomp_dsound_api_shutdown();
    CHECK(reset_count == 0 && shutdown_count == 0 && recomp_dsound_current_device() != 0u);
    on_owner = 1;
    recomp_dsound_api_shutdown();
    CHECK(reset_count == 1 && shutdown_count == 1 && recomp_dsound_current_device() == 0u);
    return 0;
}
