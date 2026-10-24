/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_buffer_adapter.h"
#include "dsound_buffer_model.h"
#include "dsound_api_adapter.h"
#include "audio_output.h"
#include "kernel_abi.h"
#include "stop_report.h"
#include <setjmp.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DESC = 0x1000, FORMAT = 0x2000, OUTPUT = 0x3000, DEVICE = 0x4008,
    DEVICE_SETTINGS = 0x5000, ENTRY = 0x8000, DATA = 0xa000 };
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint32_t memory[RECOMP_XBOX_RAM_SIZE / 4u];
static uint32_t cursor, owners = 1u, device = DEVICE, allocated, freed;
static int fail_after = -1;
static struct Allocation { uint32_t base, size; int live; } allocations[1024];
static uint64_t now;
static int ready = 1, muted;
static uint32_t output_calls, resets;
static float listener[6], route_dry[18], route_wet[6];
static uint32_t speakers = 2u;
static uint32_t route_channels;
static struct Output { uint32_t bytes, rate, channels, bits; int32_t volume; uint8_t pcm[160000]; } output;
static jmp_buf stop_environment;
static int stop_armed;
static char stop_reason[128];

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL buffer adapter line %d: %s\n", __LINE__, #condition); abort(); \
} } while (0)

void recomp_stop(int code, const char *format, ...)
{
    va_list arguments;
    CHECK(code == 2);
    va_start(arguments, format);
    vsnprintf(stop_reason, sizeof stop_reason, format, arguments);
    va_end(arguments);
    if (stop_armed) longjmp(stop_environment, 1);
    fprintf(stderr, "unexpected stop %s\n", stop_reason);
    abort();
}

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    uint32_t window = address & ~(RECOMP_XBOX_RAM_SIZE - 1u);
    uint32_t offset = address & (RECOMP_XBOX_RAM_SIZE - 1u);
    if ((window != 0 && window != RECOMP_XBOX_CACHED_ALIAS && window != RECOMP_XBOX_PHYSICAL_ALIAS) ||
        size > RECOMP_XBOX_RAM_SIZE - offset) recomp_stop(2, "memory:invalid");
    return (uint8_t *)memory + offset;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)(void *)recomp_memory(address, 4); }
uint64_t recomp_test_dsound_now_ms(void) { return now; }
uint32_t recomp_dsound_current_device(void) { return device; }
void recomp_dsound_require_device(uint32_t handle) { CHECK(handle == device && device && ready); }
void recomp_dsound_device_add_owner(void) { ++owners; }
void recomp_dsound_device_release_owner(void) { CHECK(owners); if (!--owners) device = 0; }
int recomp_audio_output_ready(void) { return ready; }
uint32_t recomp_audio_output_speakers(void) { return speakers; }
int recomp_audio_output_enabled(void) { return ready && !muted; }
void recomp_audio_output_reset_voice(uint32_t slot) { CHECK(slot < 256); ++resets; }
uint32_t recomp_dsound_effects_headroom(uint32_t bin) { CHECK(bin < 32u); return 0u; }
void recomp_dsound_listener_vectors(float vectors[6]) { memcpy(vectors, listener, sizeof listener); }
int recomp_audio_output_set_voice_routing(uint32_t slot, uint32_t channels, const float *dry, const float *wet)
{
    CHECK(slot < 256u && channels >= 1u && channels <= 3u && dry && wet);
    route_channels = channels;
    memcpy(route_dry, dry, speakers * channels * sizeof *dry);
    memcpy(route_wet, wet, 2u * channels * sizeof *wet);
    return ready;
}
void recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t rate, uint32_t channels, uint32_t bits, int32_t volume)
{
    CHECK(slot < 256 && bytes <= sizeof output.pcm && pcm && ready);
    if (muted) return;
    ++output_calls;
    output.bytes = bytes; output.rate = rate; output.channels = channels;
    output.bits = bits; output.volume = volume;
    memcpy(output.pcm, pcm, bytes);
}
uint32_t recomp_kernel_allocate_pool(uint32_t size)
{
    if (fail_after == 0) return 0;
    if (fail_after > 0) --fail_after;
    CHECK(allocated < 1024 && size <= RECOMP_XBOX_RAM_SIZE - cursor);
    uint32_t base = cursor;
    cursor += (size + 15u) & ~15u;
    allocations[allocated++] = (struct Allocation){base, size, 1};
    memset(recomp_memory(base, size), 0xcd, size);
    return base;
}
void recomp_kernel_free_pool(uint32_t address)
{
    for (uint32_t i = 0; i < allocated; ++i) if (allocations[i].base == address) {
        CHECK(allocations[i].live);
        allocations[i].live = 0;
        ++freed;
        return;
    }
    CHECK(0);
}

static uint32_t read32(uint32_t address) { uint32_t result; memcpy(&result, recomp_memory(address, 4), 4); return result; }
static void write32(uint32_t address, uint32_t value) { memcpy(recomp_memory(address, 4), &value, 4); }
static void write16(uint32_t address, uint16_t value) { memcpy(recomp_memory(address, 2), &value, 2); }
static uint32_t fbits(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

static uint32_t call(uint32_t address, const uint32_t *arguments, uint32_t count)
{
    RecompFunction function = recomp_dsound_buffer_lookup_manual(address);
    CHECK(function != NULL);
    write32(ENTRY, 0x12345678u);
    for (uint32_t i = 0; i < count; ++i) write32(ENTRY + 4u + i * 4u, arguments[i]);
    write32(ENTRY + 4u + count * 4u, 0xabcd1234u);
    recomp_runtime.registers.esp = ENTRY;
    recomp_runtime.registers.eax = 0xa5a5a5a5u;
    function();
    CHECK(recomp_runtime.registers.esp == ENTRY + 4u + count * 4u);
    CHECK(read32(ENTRY + 4u + count * 4u) == 0xabcd1234u);
    return recomp_runtime.registers.eax;
}
#define CALL(address, ...) call(address, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4u)

static void expect_stop(uint32_t address, uint32_t handle, const char *reason)
{
    stop_armed = 1;
    if (setjmp(stop_environment) == 0) { CALL(address, handle, 0, 0, 0, 0, 0, 0, 0); CHECK(0); }
    CHECK(strncmp(stop_reason, reason, strlen(reason)) == 0);
    stop_armed = 0;
}

static void fixture(uint16_t tag, uint16_t channels, uint32_t bytes)
{
    recomp_dsound_buffers_reset();
    CHECK(allocated == freed);
    memset(allocations, 0, sizeof allocations);
    allocated = freed = 0; cursor = 0x10000u; owners = 1u; device = DEVICE;
    fail_after = -1; now = 0; ready = 1; muted = 0; output_calls = resets = 0;
    memset(&output, 0, sizeof output);
    write32(DEVICE, DEVICE_SETTINGS);
    memset(recomp_memory(DESC, 24), 0, 24);
    write32(DESC, 24); write32(DESC + 4, 0x40000); write32(DESC + 8, bytes); write32(DESC + 12, FORMAT);
    memset(recomp_memory(FORMAT, 20), 0, 20);
    write16(FORMAT, tag); write16(FORMAT + 2, channels); write32(FORMAT + 4, 1000);
    uint32_t align = channels * (tag == 0x69 ? 36u : 2u);
    write32(FORMAT + 8, tag == 0x69 ? 1000 * align / 64 : 1000 * align);
    write16(FORMAT + 12, (uint16_t)align); write16(FORMAT + 14, tag == 0x69 ? 4 : 16);
    write16(FORMAT + 16, tag == 0x69 ? 2 : 0); write16(FORMAT + 18, 64);
}

static uint32_t create(void)
{
    CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == 0);
    uint32_t handle = read32(OUTPUT), root = handle - 0x1cu, settings = read32(handle);
    CHECK(handle && root >= 0x10000 && settings >= 0x10000 && owners == 2);
    CHECK(read32(root) == 0x46c1d8 && read32(root + 4) == 1 && read32(root + 8) == DEVICE - 8);
    CHECK(read32(root + 0x10) == DEVICE_SETTINGS && read32(root + 0x14) == root + 0x14);
    CHECK(read32(root + 0x18) == root + 0x14 && read32(root + 0xc) == 0 && read32(root + 0x20) == 0);
    CHECK(read32(settings + 8) == 0x40000u && read32(settings + 0x20) == 600u);
    CHECK((int32_t)read32(settings + 0x1c) == -600);
    return handle;
}

static void test_pcm_and_headroom(void)
{
    fixture(1, 1, 0);
    uint32_t handle = create(), settings = read32(handle);
    CHECK(read32(settings + 0xbc) == 0 && read32(settings + 0xc0) == 0);
    for (uint16_t i = 0; i < 40; ++i) write16(DATA + i * 2u, i);
    CHECK(CALL(0x365793, handle, DATA | RECOMP_XBOX_CACHED_ALIAS, 80) == 0);
    CHECK(read32(settings + 0xbc) == (DATA | RECOMP_XBOX_CACHED_ALIAS));
    CHECK(CALL(0x364802, handle, 123, 456, 1) == 0);
    now = 10;
    CHECK(CALL(0x36489e, handle | RECOMP_XBOX_PHYSICAL_ALIAS, OUTPUT, OUTPUT + 4) == 0);
    CHECK(read32(OUTPUT) == 20 && read32(OUTPUT + 4) == 20);
    CHECK(output.bytes == 20 && output.rate == 1000 && output.channels == 1 && output.bits == 16 && output.volume == -600);
    CHECK(((int16_t *)(void *)output.pcm)[0] == 0 && ((int16_t *)(void *)output.pcm)[9] == 9);
    write16(DATA, 3000); CHECK(((int16_t *)(void *)output.pcm)[0] == 0);
    CHECK(CALL(0x3647ae, handle, (uint32_t)-2000) == 0);
    CHECK((int32_t)read32(settings + 0x1c) == -2600);
    now = 20; recomp_dsound_buffers_pump(); CHECK(output.volume == -2600);
    CHECK(CALL(0x3647ca, handle, 0) == 0 && (int32_t)read32(settings + 0x1c) == -2000);
    CHECK(CALL(0x3647ca, handle, 500) == 0 && (int32_t)read32(settings + 0x1c) == -2500);
    now = 30; recomp_dsound_buffers_pump(); CHECK(output.volume == -2500);
    CHECK(CALL(0x3647ae, handle, (uint32_t)-10001) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3647ca, handle, UINT32_MAX) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK((int32_t)read32(settings + 0x1c) == -2500 && read32(settings + 0x20) == 500);
    CHECK(CALL(0x364882, handle, OUTPUT) == 0 && read32(OUTPUT) == 5);
    CHECK(CALL(0x364826, handle) == 0);
    now = 40; uint32_t calls = output_calls; recomp_dsound_buffers_pump(); CHECK(output_calls == calls);
    CHECK(CALL(0x364882, handle, OUTPUT) == 0 && read32(OUTPUT) == 0);
    uint32_t references = 0;
    CHECK(recomp_dsound_buffer_addref_root(handle - 0x1c, &references) && references == 2 && owners == 2);
    CHECK(read32(handle - 0x18) == 2);
    CHECK(CALL(0x363641, handle) == 1 && owners == 2);
    CHECK(CALL(0x363b91, handle - 0x1c) == 0 && owners == 1 && allocated == freed);
    CHECK(!recomp_dsound_buffer_addref_root(handle - 0x1c, &references));
    expect_stop(0x363641, handle, "dsound:buffer-handle-invalid");
}

static void test_ownership_and_lock(void)
{
    fixture(1, 1, 0);
    uint32_t handle = create(), settings = read32(handle);
    CHECK(CALL(0x365793, handle, 0, 80) == 0);
    uint32_t data = read32(settings + 0xbc);
    CHECK((data & 31) == 0 && read32(settings + 8) == 0x80040000u && allocated - freed == 3);
    uint32_t before = resets;
    CHECK(CALL(0x365793, handle, data | RECOMP_XBOX_CACHED_ALIAS, 80) == 0 && resets == before);
    CHECK(CALL(0x365793, handle, data, 40) == RECOMP_DSOUND_INVALID_PARAM && allocated - freed == 3);
    CHECK(CALL(0x3648da, handle, 60, 40, OUTPUT, OUTPUT + 4, OUTPUT + 8, OUTPUT + 12, 0) == 0);
    CHECK(read32(OUTPUT) == data + 60 && read32(OUTPUT + 4) == 20 && read32(OUTPUT + 8) == data && read32(OUTPUT + 12) == 20);
    CHECK(CALL(0x36363c, handle, data + 60, 20, data, 20) == 0);
    CHECK(CALL(0x36363c, handle, data + 50, 20, data, 20) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3648da, handle, 60, 40, OUTPUT, OUTPUT + 4, 0, 0, 0) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3648da, handle, 0, 20, OUTPUT, OUTPUT + 4, 0, 0, 0) == 0);
    CHECK(CALL(0x3648da, handle, 80, 20, OUTPUT, OUTPUT + 4, 0, 0, 0) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3648da, handle, 0, 82, OUTPUT, OUTPUT + 4, 0, 0, 0) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3648da, handle, 0, 1, OUTPUT, OUTPUT + 4, 0, 0, 0) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3648da, handle, 0, 0, OUTPUT, OUTPUT + 4, 0, 0, 2) == 0 && read32(OUTPUT + 4) == 80);
    CHECK(CALL(0x3648da, handle, 0, 20, OUTPUT, OUTPUT + 4, OUTPUT + 8, 0, 0) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x3648da, handle, 0, 20, 0x83fffffeu, OUTPUT + 4, 0, 0, 0) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x365793, handle, DATA, 80) == 0 && allocated - freed == 2);
    CHECK(read32(settings + 8) == 0x40000u);
    CHECK(CALL(0x365793, handle, 0x83fffff0u, 80) == RECOMP_DSOUND_POINTER_ERROR && read32(settings + 0xbc) == DATA);
    fail_after = 0;
    CHECK(CALL(0x365793, handle, 0, 80) == RECOMP_DSOUND_OUT_OF_MEMORY && read32(settings + 0xbc) == DATA);
    fail_after = -1;
    CHECK(CALL(0x365793, handle, 0, 0) == 0 && read32(settings + 0xbc) == 0 && read32(settings + 0xc0) == 0);
    CHECK(CALL(0x364802, handle, 0, 0, 0) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x363641, handle) == 0 && allocated == freed);
}

static void test_clock_and_loop(void)
{
    fixture(1, 1, 0);
    uint32_t handle = create();
    CHECK(CALL(0x365121, handle, 2000) == 0);
    CHECK(CALL(0x365793, handle, DATA, 80) == 0);
    CHECK(CALL(0x364862, handle, 20, 40) == 0);
    CHECK(CALL(0x364802, handle, 0, 0, 1) == 0);
    now = 20; recomp_dsound_buffers_pump(); CHECK(output.bytes == 80 && output.rate == 2000);
    CHECK(CALL(0x36489e, handle, OUTPUT, 0) == 0 && read32(OUTPUT) == 40);
    CHECK(CALL(0x3648da, handle, 0, 10, OUTPUT, OUTPUT + 4, 0, 0, 1) == 0 && read32(OUTPUT) == DATA + 40);
    CHECK(CALL(0x3648be, handle, 58) == 0);
    now = 21; CHECK(CALL(0x36489e, handle, OUTPUT, OUTPUT + 4) == 0 && read32(OUTPUT) == 22);
    CHECK(CALL(0x365121, handle, 0) == 0);
    CHECK(CALL(0x365121, handle, 999) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x3648be, handle, 80) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x364862, handle, 60, 40) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x364802, handle, 0, 0, 2) == 0);
    now = 61; CHECK(CALL(0x364882, handle, OUTPUT) == 0 && read32(OUTPUT) == 0 && output.bytes == 80);
    CHECK(CALL(0x364802, handle, 0, 0, 1) == 0);
    now = 200; uint32_t calls = output_calls; recomp_dsound_buffers_pump(); CHECK(output_calls == calls);
    CHECK(CALL(0x36489e, handle, OUTPUT, 0) == 0 && read32(OUTPUT) == 38);
    CHECK(CALL(0x36483e, handle, 0, 0, 0) == 0);
    muted = 1; CHECK(CALL(0x364802, handle, 0, 0, 3) == 0);
    now = 210; CHECK(CALL(0x36489e, handle, OUTPUT, 0) == 0 && read32(OUTPUT) == 20 && output_calls == calls);
    muted = 0;
}

static void block(uint32_t address, uint16_t predictor)
{
    memset(recomp_memory(address, 36), 0x11, 36);
    write16(address, predictor); write16(address + 2, 0);
}
static void test_adpcm(void)
{
    fixture(0x69, 1, 0);
    uint32_t handle = create();
    block(DATA, 100); block(DATA + 36, 1000); block(DATA + 72, 2000);
    CHECK(CALL(0x365793, handle, DATA, 108) == 0);
    CHECK(CALL(0x364862, handle, 36, 36) == 0);
    CHECK(CALL(0x364802, handle, 0, 0, 1) == 0);
    now = 50; recomp_dsound_buffers_pump();
    CHECK(output.bits == 16 && output.channels == 1 && output.bytes == 100);
    CHECK(((int16_t *)(void *)output.pcm)[0] == 100 && ((int16_t *)(void *)output.pcm)[49] == 149);
    now = 100; recomp_dsound_buffers_pump();
    CHECK(((int16_t *)(void *)output.pcm)[0] == 150 && ((int16_t *)(void *)output.pcm)[14] == 1000);
    now = 200; recomp_dsound_buffers_pump();
    CHECK(output.bytes == 200 && ((int16_t *)(void *)output.pcm)[0] == 1036);
    CHECK(((int16_t *)(void *)output.pcm)[28] == 1000 && ((int16_t *)(void *)output.pcm)[92] == 1000);
    CHECK(CALL(0x36489e, handle, OUTPUT, 0) == 0 && read32(OUTPUT) == 36);
    CHECK(CALL(0x3648be, handle, 1) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x365793, handle, DATA, 37) == RECOMP_DSOUND_INVALID_PARAM);
    fixture(0x69, 2, 0); handle = create();
    memset(recomp_memory(DATA, 72), 0x11, 72);
    write16(DATA, 100); write16(DATA + 2, 0); write16(DATA + 4, (uint16_t)-100); write16(DATA + 6, 0);
    CHECK(CALL(0x365793, handle, DATA, 72) == 0 && CALL(0x364802, handle, 0, 0, 0) == 0);
    now = 10; recomp_dsound_buffers_pump();
    CHECK(output.bytes == 40 && output.channels == 2 && ((int16_t *)(void *)output.pcm)[0] == 100);
    CHECK(((int16_t *)(void *)output.pcm)[1] == -100 && ((int16_t *)(void *)output.pcm)[3] == -99);
}

static void test_validation_and_guards(void)
{
    fixture(1, 1, 0);
    CHECK(CALL(0x365fbc, DEVICE, DESC, 0, 0) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x365fbc, DEVICE, 0x83fffff0u, OUTPUT, 0) == RECOMP_DSOUND_POINTER_ERROR && read32(OUTPUT) == 0);
    write32(DESC, 20); CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == RECOMP_DSOUND_INVALID_PARAM);
    write32(DESC, 24); CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 1) == RECOMP_DSOUND_INVALID_PARAM);
    write16(FORMAT + 12, 0); CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == RECOMP_DSOUND_INVALID_PARAM);
    write16(FORMAT + 12, 2); fail_after = 1;
    CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == RECOMP_DSOUND_OUT_OF_MEMORY && allocated == freed && owners == 1);
    fail_after = 2; write32(DESC + 8, 80);
    CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == RECOMP_DSOUND_OUT_OF_MEMORY && allocated == freed && owners == 1);
    fail_after = -1; write32(DESC + 8, 0);
    memmove(recomp_memory(FORMAT + 1, 20), recomp_memory(FORMAT, 20), 20); write32(DESC + 12, FORMAT + 1);
    uint32_t handle = create();
    CHECK(CALL(0x3647e6, handle, 0) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x36513d, handle, 0) == 0);
    write32(DATA, 1); write32(DATA + 4, DATA + 8);
    write32(DATA + 8, 10); write32(DATA + 12, (uint32_t)-2000);
    CHECK(CALL(0x3647e6, handle, DATA) == 0);
    CHECK(read32(read32(handle) + 0x24u) == 2u);
    CHECK(CALL(0x36513d, handle, DATA) == 0);
    CHECK(read32(read32(handle) + 0x24u) == 1u &&
        *recomp_memory_i8(read32(handle) + 0x28u) == 10);
    expect_stop(0x365159, handle, "dsound:buffer-not-3d");
    expect_stop(0x366257, handle, "dsound:stream-unbound");
    CHECK(recomp_dsound_buffer_lookup_manual(0x1234) == NULL);
    stop_armed = 1;
    if (setjmp(stop_environment) == 0) { CALL(0x364802, handle, 0, 0, 4); CHECK(0); }
    CHECK(strcmp(stop_reason, "dsound:buffer-play-flags-unbound:00000004") == 0);
    if (setjmp(stop_environment) == 0) { CALL(0x36483e, handle, 1, 0, 0); CHECK(0); }
    CHECK(strcmp(stop_reason, "dsound:buffer-stop-ex-unbound") == 0);
    write32(DESC + 4, 0x20000);
    if (setjmp(stop_environment) == 0) { CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0); CHECK(0); }
    CHECK(strncmp(stop_reason, "dsound:buffer-create-flags-unbound", strlen("dsound:buffer-create-flags-unbound")) == 0);
    stop_armed = 0;
    ready = 0;
    expect_stop(0x364802, handle, "dsound:audio-output-failed");
    CHECK(CALL(0x363641, handle) == 0 && owners == 1 && allocated == freed);
    ready = 1;
}

static void test_capacity_and_owner(void)
{
    fixture(1, 1, 0);
    for (uint32_t i = 0; i < 256; ++i) CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == 0);
    CHECK(owners == 257 && allocated == 512);
    CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == RECOMP_DSOUND_OUT_OF_MEMORY && read32(OUTPUT) == 0);
    recomp_dsound_buffers_reset(); CHECK(owners == 1 && allocated == freed);
    fixture(1, 1, 0); uint32_t handle = create();
    --owners; CHECK(owners == 1 && device == DEVICE);
    CHECK(CALL(0x363641, handle) == 0 && owners == 0 && device == 0 && allocated == freed);
}

static int near(float a, float b) { return fabsf(a - b) < 0.0001f; }

static void test_3d(void)
{
    /* One flat 1/16 (0x40) ear pair for every direction; both 45-step center tables at 0.5. */
    memset(recomp_memory(0x36de70, 0x115c0 + 0xec6), 0, 0x115c0 + 0xec6);
    *recomp_memory_i8(0x36de70) = 0x40; *recomp_memory_i8(0x36de90) = 0x40;
    for (uint32_t i = 0; i < 45; ++i) { write32(0x36dd00 + 4 * i, fbits(.5f)); write32(0x36ddb8 + 4 * i, fbits(.5f)); }
    memset(listener, 0, sizeof listener);
    fixture(1, 1, 0); write32(DESC + 4, 0x40010);
    CHECK(CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0) == 0);
    uint32_t handle = read32(OUTPUT), settings = read32(handle);
    CHECK(read32(settings + 0x20) == 0 && read32(settings + 0x24) == 5);
    CHECK(*recomp_memory_i8(settings + 0x29) == 8 && *recomp_memory_i8(settings + 0x2c) == 10);
    for (uint16_t i = 0; i < 2000; ++i) write16(DATA + i * 2u, 1000);
    CHECK(CALL(0x365793, handle, DATA, 4000) == 0 && CALL(0x364802, handle, 0, 0, 1) == 0);
    /* rolloff 1, min 1: ten units away is -20 dB on XTLK front and I3DL2; back bins are off. */
    CHECK(CALL(0x36517d, handle, fbits(1), 1) == 0);
    CHECK(CALL(0x3651a1, handle, fbits(10), 0, 0, 0) == 0);
    now = 20; recomp_dsound_buffers_pump();
    CHECK(output.bits == 32 && output.channels == 3 && output.rate == 48000 && output.bytes >= 900 * 12);
    const float *ears = (const float *)(const void *)output.pcm;
    /* Equal ears settle at in / (1 - feed / (1 - pole)) through the crosstalk canceller. */
    const float pole = (float)0x7645a1 / 0x800000, feed = (float)(0xf88313 - 0x1000000) / 0x800000;
    CHECK(near(ears[2400], 1000.f / 16 / 32768 / (1 - feed / (1 - pole))) && ears[2401] == ears[2400] &&
        near(ears[2402], 1000.f / 32768));
    /* Default 3D bins: doubled ears to their sides; I3DL2 (route 4) takes the unfiltered voice. */
    CHECK(route_channels == 3 && near(route_dry[0], .2f) && route_dry[1] == 0 && route_dry[2] == 0 &&
        route_dry[3] == 0 && near(route_dry[4], .2f) && route_dry[5] == 0 &&
        route_wet[0] == 0 && near(route_wet[2], .1f) && near(route_wet[5], .1f));
    /* DOA2's 3D layout puts center in slot 4: ahead, center 0.25 (-12.04 dB) and front 0.75 (-2.49 dB). */
    CHECK(CALL(0x3651a1, handle, 0, 0, fbits(10), 1) == 0);
    const uint32_t bins[16] = {6,0, 8,0, 7,0, 9,0, 2,0, 10,0, 3,0, 22,(uint32_t)-10000};
    memcpy(recomp_memory(DATA + 0x2000, sizeof bins), bins, sizeof bins);
    write32(DATA + 0x1ff8, 8); write32(DATA + 0x1ffc, DATA + 0x2000);
    CHECK(CALL(0x36513d, handle, DATA + 0x1ff8) == 0);
    const float front = 2 * powf(10, -2249 / 2000.f), center = powf(10, -3204 / 2000.f) * .70710678f;
    /* Center (route 4) folds the unfiltered voice into both sides. */
    CHECK(near(route_dry[0], front) && route_dry[1] == 0 && near(route_dry[2], center) &&
        route_dry[3] == 0 && near(route_dry[4], front) && near(route_dry[5], center) && near(route_wet[2], .1f));
    /* 5.1: ahead stays on the front pair and center; behind moves to the back pair. */
    speakers = 6; recomp_dsound_buffers_refresh_routing();
    CHECK(near(route_dry[0], front) && near(route_dry[4], front) && route_dry[12] == 0 &&
        route_dry[16] == 0 && near(route_dry[8], center / .70710678f));
    CHECK(CALL(0x3651a1, handle, 0, 0, fbits(-10), 0) == 0);
    recomp_dsound_buffers_refresh_routing();
    CHECK(route_dry[0] == 0 && route_dry[8] == 0 && near(route_dry[12], .2f) && near(route_dry[16], .2f));
    speakers = 2;
    CHECK(CALL(0x3651a1, handle, 0, 0, fbits(10), 1) == 0);
    /* Listener commit: at the source, only the center split remains. */
    listener[2] = 10; recomp_dsound_buffers_refresh_routing();
    CHECK(near(route_dry[4], 2 * powf(10, -249 / 2000.f)));
    /* Approaching at half the speed of sound plays 1.5x: 1000 Hz becomes 1500 Hz. */
    listener[2] = 0; now = 100;
    CHECK(CALL(0x36489e, handle, OUTPUT, 0) == 0);
    uint32_t before = read32(OUTPUT);
    CHECK(CALL(0x3651d6, handle, 0, 0, fbits(-171), 0) == 0);
    now = 120; CHECK(CALL(0x36489e, handle, OUTPUT, 0) == 0 && read32(OUTPUT) == (before + 60) % 4000);
    CHECK(CALL(0x36520b, handle, fbits(2), 0) == 0 && CALL(0x36522f, handle, fbits(0), 0) == 0);
    CHECK(CALL(0x365159, handle, fbits(5), 0) == 0 && CALL(0x363641, handle) == 0 && allocated == freed);
    fixture(1, 2, 0); write32(DESC + 4, 0x40010);
    stop_armed = 1;
    if (setjmp(stop_environment) == 0) { CALL(0x365fbc, DEVICE, DESC, OUTPUT, 0); CHECK(0); }
    CHECK(strcmp(stop_reason, "dsound:buffer-3d-format-unbound") == 0);
    stop_armed = 0;
}

int main(void)
{
    test_pcm_and_headroom(); test_ownership_and_lock(); test_clock_and_loop();
    test_adpcm(); test_validation_and_guards(); test_capacity_and_owner(); test_3d();
    recomp_dsound_buffers_reset(); CHECK(allocated == freed);
    puts("PASS 5849 buffer RAM layout, PCM/ADPCM output, headroom, clocks, locks, aliases, ownership, 3D voices and guards");
    return 0;
}
