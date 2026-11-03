/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_effects_adapter.h"
#include "dsound_effects_image.h"
#include "dsound_device_model.h"
#include "dsound_api_adapter.h"
#include "audio_output.h"
#include "kernel_abi.h"
#include "stop_report.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DEVICE = 0x4008u, SETTINGS = 0x5000u, ENTRY = 0x8000u,
    OUTPUT = 0x9000u, LOCATION = 0xa000u, LISTENER = 0xb000u,
    UPDATE = 0xc000u, INPUT = 0x90000u, BASE = 0x818u,
    STATE_BYTES = 544u, STATE = BASE + 68u,
    DESC = STATE + STATE_BYTES * 2u, TOTAL = DESC + 88u,
    INVALID_EFFECT = 0x88780032u };
static const uint64_t initializer = UINT64_C(0x0123456789abcdef);
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint32_t memory[RECOMP_XBOX_RAM_SIZE / 4u];
static struct Allocation { uint32_t base, bytes; bool live; } allocations[64];
static uint32_t cursor = 0x01000000u, allocated, freed, refreshes, reverb_calls;
static int fail_after = -1, reverb_ready = 1, backend_ready = 1, output_locked;
static RecompAudioReverb reverb_applied;
static jmp_buf stopped;
static int stop_armed;

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL effects adapter line %d: %s\n", __LINE__, #condition); abort(); \
} } while (0)

void recomp_stop(int code, const char *format, ...)
{
    (void)format;
    CHECK(code == 2 && stop_armed);
    longjmp(stopped, 1);
}

uint8_t *recomp_memory(uint32_t address, size_t bytes)
{
    uint32_t window = address & ~(RECOMP_XBOX_RAM_SIZE - 1u);
    uint32_t offset = address & (RECOMP_XBOX_RAM_SIZE - 1u);
    CHECK((window == 0u || window == RECOMP_XBOX_CACHED_ALIAS ||
        window == RECOMP_XBOX_PHYSICAL_ALIAS) && bytes <= RECOMP_XBOX_RAM_SIZE - offset);
    return (uint8_t *)memory + offset;
}
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, 4u);
}
void recomp_guest_memset(uint32_t address, int value, size_t bytes)
{
    memset(recomp_memory(address, bytes), value, bytes);
}
uint32_t recomp_dsound_current_device(void) { return DEVICE; }
void recomp_dsound_require_device(uint32_t handle)
{
    if (handle != DEVICE) recomp_stop(2, "dsound:invalid-device");
}
/* The real routing refresh takes the non-recursive output lock itself. */
void recomp_dsound_buffers_refresh_routing(void) { CHECK(!output_locked); ++refreshes; }
void recomp_dsound_output_lock(void) { CHECK(!output_locked); output_locked = 1; }
void recomp_dsound_output_unlock(void) { CHECK(output_locked); output_locked = 0; }
int recomp_audio_output_ready(void) { return backend_ready; }
int recomp_audio_output_set_reverb(const RecompAudioReverb *parameters)
{
    CHECK(parameters != NULL);
    CHECK(output_locked);
    ++reverb_calls;
    if (!reverb_ready || !backend_ready) return 0;
    reverb_applied = *parameters;
    return 1;
}
uint32_t recomp_kernel_allocate_pool(uint32_t bytes)
{
    if (fail_after == 0) return 0u;
    if (fail_after > 0) --fail_after;
    CHECK(allocated < 64u && bytes != 0u && bytes <= RECOMP_XBOX_RAM_SIZE - cursor);
    const uint32_t base = cursor;
    cursor += (bytes + 15u) & ~15u;
    allocations[allocated++] = (struct Allocation){base, bytes, true};
    memset(recomp_memory(base, bytes), 0xcd, bytes);
    return base;
}
void recomp_kernel_free_pool(uint32_t base)
{
    for (uint32_t i = 0u; i < allocated; ++i) if (allocations[i].base == base) {
        CHECK(allocations[i].live);
        for (uint32_t j = 0u; j < allocations[i].bytes; ++j)
            CHECK(recomp_memory(base, allocations[i].bytes)[j] == 0u);
        allocations[i].live = false;
        ++freed;
        return;
    }
    CHECK(0);
}

static uint32_t read32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, recomp_memory(address, 4u), 4u);
    return value;
}
static void write32(uint32_t address, uint32_t value)
{
    memcpy(recomp_memory(address, 4u), &value, 4u);
}
static bool live(uint32_t address)
{
    for (uint32_t i = 0u; i < allocated; ++i)
        if (allocations[i].base == address) return allocations[i].live;
    return false;
}
static uint32_t call(uint32_t address, const uint32_t *arguments, uint32_t count)
{
    RecompFunction function = recomp_dsound_effects_lookup_manual(address);
    CHECK(function != NULL);
    write32(ENTRY, 0x12345678u);
    for (uint32_t i = 0u; i < count; ++i) write32(ENTRY + 4u + i * 4u, arguments[i]);
    write32(ENTRY + 4u + count * 4u, 0xabcd1234u);
    recomp_runtime.registers.esp = ENTRY;
    recomp_runtime.registers.eax = 0xa5a5a5a5u;
    function();
    CHECK(recomp_runtime.registers.esp == ENTRY + 4u + count * 4u);
    CHECK(read32(ENTRY + 4u + count * 4u) == 0xabcd1234u);
    return recomp_runtime.registers.eax;
}
#define CALL(address, ...) call(address, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4u)

static uint64_t step(uint64_t state)
{
    return state >> 1 | ((state >> 63 ^ state ^ state >> 2 ^ state >> 3) & 1u) << 63;
}
static void encode(const uint8_t *plain, uint8_t *cipher, uint32_t bytes, const uint8_t *key)
{
    uint64_t fixed = initializer;
    uint8_t fixed_key[8];
    for (unsigned i = 0u; i < 81u; ++i) fixed = step(fixed);
    for (unsigned i = 0u; i < 8u; ++i) fixed_key[i] = (uint8_t)(fixed >> (i * 8u));
    if (key == NULL) key = fixed_key;
    uint64_t stream = step(fixed);
    for (uint32_t i = 0u; i < bytes; ++i) {
        unsigned j = i & 7u;
        stream = step(stream);
        cipher[i] = (uint8_t)((plain[i] ^ fixed_key[j] ^ (uint8_t)stream) + key[j] * fixed_key[j]);
    }
}
static void fixture(void)
{
    uint8_t *data = recomp_memory(INPUT, TOTAL), keys[16], plain[32];
    memset(data, 0u, TOTAL);
    write32(INPUT + 0x804u, 17u);
    write32(INPUT + 0x808u, STATE);
    write32(INPUT + 0x80cu, STATE_BYTES / 2u);
    write32(INPUT + DESC, 2u);
    write32(INPUT + DESC + 4u, 32u);
    for (unsigned i = 0u; i < 16u; ++i) keys[i] = (uint8_t)(0x11u + i * 7u);
    for (unsigned i = 0u; i < 2u; ++i) {
        uint32_t map = INPUT + DESC + 8u + i * 32u;
        write32(map, BASE + i * 32u); write32(map + 4u, 32u);
        write32(map + 8u, STATE + i * STATE_BYTES); write32(map + 12u, STATE_BYTES);
        write32(map + 24u, 0xc000u + i * 16u); write32(map + 28u, 16u);
        for (unsigned j = 0u; j < 32u; ++j) plain[j] = (uint8_t)(i * 32u + j);
        encode(plain, data + BASE + i * 32u, 32u, keys + i * 8u);
    }
    for (unsigned i = 0u; i < STATE_BYTES * 2u; ++i) data[STATE + i] = (uint8_t)(0x20u + i % 128u);
    encode(keys, data + DESC + 72u, 16u, NULL);
    write32(LOCATION, 0u); write32(LOCATION + 4u, 1u);
}

int main(void)
{
    const RecompAudioReverb initial = {-1000, -100, 0.0f, 1.5f, 0.8f,
        -1500, 0.02f, -500, 0.03f, 100.0f, 100.0f, 5000.0f};
    RecompAudioReverb changed = initial;
    write32(DEVICE, SETTINGS);
    write32(SETTINGS + 8u, UINT32_MAX); write32(SETTINGS + 12u, UINT32_MAX);
    memcpy(recomp_memory(SETTINGS + 0x84u, sizeof initial), &initial, sizeof initial);
    memcpy(recomp_memory(LISTENER, sizeof initial), &initial, sizeof initial);
    write32(0x36c38cu, (uint32_t)(initializer >> 32u));
    write32(0x36c391u, (uint32_t)initializer);
    CHECK(recomp_dsound_effects_lookup_manual(0u) == NULL);
    CHECK(recomp_dsound_effects_lookup_manual(UINT32_MAX) == NULL);
    CHECK(CALL(0x364764u, DEVICE, 0u, 0u, UPDATE, 4u, 0u) == INVALID_EFFECT);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 0u) == INVALID_EFFECT);
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK);

    fixture();
    write32(OUTPUT, 0xdeadbeefu);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL - 1u, LOCATION, OUTPUT) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(read32(OUTPUT) == 0xdeadbeefu && allocated == 0u);
    CHECK(CALL(0x36473du, DEVICE, 0xfe830000u, TOTAL, LOCATION, OUTPUT) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, RECOMP_XBOX_RAM_SIZE - 4u, OUTPUT) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, RECOMP_XBOX_RAM_SIZE - 2u) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(read32(OUTPUT) == 0xdeadbeefu && allocated == 0u);
    write32(INPUT + DESC + 20u, STATE_BYTES - 4u);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, OUTPUT) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(read32(OUTPUT) == 0xdeadbeefu && allocated == 0u);
    write32(INPUT + DESC + 20u, STATE_BYTES);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, OUTPUT) == RECOMP_DSOUND_OK);
    const uint32_t descriptor = read32(OUTPUT), first_map = descriptor + 8u;
    const uint32_t owned_payload = read32(first_map), owned_scratch = read32(first_map + 24u);
    const uint32_t state = read32(first_map + 8u), second_state = read32(first_map + 40u);
    CHECK(allocated - freed == 3u && live(descriptor) && live(owned_payload) && live(owned_scratch));
    CHECK(read32(descriptor) == 2u && read32(descriptor + 4u) == 32u);
    CHECK(state == owned_payload + 68u && second_state == state + STATE_BYTES);
    CHECK(read32(first_map + 4u) == 32u && read32(first_map + 12u) == STATE_BYTES);
    CHECK(read32(first_map + 16u) == 0u && read32(first_map + 20u) == 0u);
    CHECK(read32(first_map + 28u) == 16u && read32(first_map + 56u) == owned_scratch + 16u);
    CHECK(state < RECOMP_XBOX_RAM_SIZE && owned_scratch < RECOMP_XBOX_RAM_SIZE);
    CHECK(read32(SETTINGS + 8u) == 0u && read32(SETTINGS + 12u) == 1u);
    CHECK(memcmp(&reverb_applied, &initial, sizeof initial) == 0);
    for (unsigned i = 0u; i < 64u; ++i) CHECK(recomp_memory(owned_payload, 64u)[i] == i);
    CHECK(memcmp(recomp_memory(state, STATE_BYTES * 2u), recomp_memory(INPUT + STATE, STATE_BYTES * 2u), STATE_BYTES * 2u) == 0);
    memset(recomp_memory(INPUT, TOTAL), 0u, TOTAL);
    CHECK(read32(state + 8u) == 0x2b2a2928u);
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK);
    CHECK((read32(state + 0x10u) & 4u) != 0u);

    const uint32_t original = read32(second_state + 0x20u);
    write32(UPDATE, 0x11223344u);
    CHECK(CALL(0x364764u, DEVICE, 1u, 0x20u, UPDATE, 4u, 1u) == RECOMP_DSOUND_OK);
    CHECK(read32(second_state + 0x20u) == original);
    write32(UPDATE, 0xaabbccddu);
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK);
    CHECK(read32(second_state + 0x20u) == original);
    write32(second_state + 0x24u, 0x778899aau);
    changed.room = -2000;
    memcpy(recomp_memory(LISTENER, sizeof changed), &changed, sizeof changed);
    uint32_t calls = reverb_calls;
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 1u) == RECOMP_DSOUND_OK);
    CHECK(reverb_calls == calls && read32(SETTINGS + 0x80u) == 0xfffu);
    write32(state + 0x10u, 0xa5a5a5a0u);
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK);
    CHECK(read32(second_state + 0x20u) == 0x11223344u && read32(second_state + 0x24u) == 0x778899aau);
    CHECK(read32(state + 0x10u) == 0xa5a5a5a4u);
    CHECK(read32(SETTINGS + 0x80u) == 0u && memcmp(&reverb_applied, &changed, sizeof changed) == 0);

    uint8_t deferred[8] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7};
    memcpy(recomp_memory(UPDATE, sizeof deferred), deferred, sizeof deferred);
    CHECK(CALL(0x364764u, DEVICE, 1u, 0x30u, UPDATE, 8u, 1u) == RECOMP_DSOUND_OK);
    write32(UPDATE + 16u, 0x11223344u);
    CHECK(CALL(0x364764u, DEVICE, 1u, 0x32u, UPDATE + 16u, 4u, 0u) == RECOMP_DSOUND_OK);
    CHECK(read32(second_state + 0x32u) == 0x11223344u);
    write32(second_state + 0x32u, 0x55667788u);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 0u) == RECOMP_DSOUND_OK);
    CHECK(recomp_memory(second_state + 0x30u, 8u)[0] == 0xa0u && recomp_memory(second_state + 0x30u, 8u)[1] == 0xa1u);
    CHECK(read32(second_state + 0x32u) == 0x55667788u);
    CHECK(recomp_memory(second_state + 0x30u, 8u)[6] == 0xa6u && recomp_memory(second_state + 0x30u, 8u)[7] == 0xa7u);
    CHECK(CALL(0x364764u, DEVICE, 2u, 0u, UPDATE, 4u, 0u) == INVALID_EFFECT);
    CHECK(CALL(0x364764u, DEVICE, 1u, STATE_BYTES - 2u, UPDATE, 4u, 0u) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x364764u, DEVICE, 1u, UINT32_MAX, UPDATE, 4u, 0u) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x364764u, DEVICE, 1u, 0u, UPDATE, 4u, 2u) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(CALL(0x364764u, DEVICE, 1u, 0u, 0xfe830000u, 4u, 0u) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x36604au, DEVICE, 0xfe830000u, 0u) == RECOMP_DSOUND_POINTER_ERROR);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 2u) == RECOMP_DSOUND_INVALID_PARAM);

    write32(UPDATE, 0xdeadbeefu);
    uint32_t prior = read32(second_state + 0x50u);
    CHECK(CALL(0x364764u, DEVICE, 1u, 0x50u, UPDATE, 4u, 1u) == RECOMP_DSOUND_OK);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 1u) == RECOMP_DSOUND_OK);
    reverb_ready = 0;
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(read32(second_state + 0x50u) == prior && read32(SETTINGS + 0x80u) == 0xfffu);
    reverb_ready = 1;
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK && read32(second_state + 0x50u) == 0xdeadbeefu);
    changed.room = -3000;
    memcpy(recomp_memory(LISTENER, sizeof changed), &changed, sizeof changed);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 1u) == RECOMP_DSOUND_OK);
    backend_ready = 0;
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_FAIL);
    CHECK(read32(SETTINGS + 0x80u) == 0xfffu && reverb_applied.room == -2000);
    backend_ready = 1;
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK && reverb_applied.room == -3000);
    CHECK(CALL(0x36478eu, DEVICE, 31u, 0x123456feu) == RECOMP_DSOUND_OK);
    CHECK(recomp_memory(SETTINGS + 0x10u + 31u, 1u)[0] == 0xfeu && refreshes == 1u);
    CHECK(recomp_dsound_effects_headroom(31u) == 6u);
    CHECK(CALL(0x36478eu, DEVICE, 32u, 7u) == RECOMP_DSOUND_INVALID_PARAM && refreshes == 1u);
    CHECK(recomp_dsound_effects_headroom(32u) == 0u);

    /* Listener position/velocity: deferred writes mark dirty, commit retires them
       and recomputes 3D voices once. */
    write32(SETTINGS + 0x30u, 0u);
    refreshes = 0u;
    CHECK(CALL(0x365fe0u, DEVICE, 0x3f800000u, 0x40000000u, 0x40400000u, 1u) == RECOMP_DSOUND_OK);
    CHECK(read32(SETTINGS + 0x38u) == 0x3f800000u && read32(SETTINGS + 0x40u) == 0x40400000u);
    CHECK(read32(SETTINGS + 0x30u) == 1u && refreshes == 0u);
    CHECK(CALL(0x366015u, DEVICE, 0xbf800000u, 0u, 0x41200000u, 0u) == RECOMP_DSOUND_OK);
    CHECK(read32(SETTINGS + 0x44u) == 0xbf800000u && read32(SETTINGS + 0x4cu) == 0x41200000u);
    CHECK(read32(SETTINGS + 0x30u) == 0u && refreshes == 1u);
    float vectors[6];
    recomp_dsound_listener_vectors(vectors);
    CHECK(vectors[0] == 1.0f && vectors[2] == 3.0f && vectors[3] == -1.0f && vectors[5] == 10.0f);
    CHECK(CALL(0x365c6eu, DEVICE) == RECOMP_DSOUND_OK && refreshes == 1u);
    stop_armed = 1;
    if (setjmp(stopped) == 0) { CALL(0x365fe0u, DEVICE + 4u, 0u, 0u, 0u, 0u); CHECK(0); }
    stop_armed = 0;

    fixture();
    for (int i = 0; i < 3; ++i) {
        fail_after = i;
        write32(OUTPUT, 0xdeadbeefu);
        CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, OUTPUT) == RECOMP_DSOUND_OUT_OF_MEMORY);
        CHECK(read32(OUTPUT) == 0xdeadbeefu && allocated - freed == 3u && live(descriptor));
        CHECK(read32(second_state + 0x50u) == 0xdeadbeefu);
    }
    fail_after = -1;
    reverb_ready = 0;
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, OUTPUT) != RECOMP_DSOUND_OK);
    CHECK(read32(OUTPUT) == 0xdeadbeefu && allocated - freed == 3u && live(descriptor));
    reverb_ready = 1;
    write32(LOCATION, 2u);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, OUTPUT) == RECOMP_DSOUND_INVALID_PARAM);
    CHECK(read32(OUTPUT) == 0xdeadbeefu && live(descriptor));
    write32(LOCATION, 0u);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, LOCATION, OUTPUT) == RECOMP_DSOUND_OK);
    CHECK(read32(OUTPUT) != descriptor && !live(descriptor) && !live(owned_payload) && !live(owned_scratch));
    CHECK(read32(descriptor) == 0u && read32(state) == 0u && allocated - freed == 3u);
    const uint32_t retained = read32(OUTPUT);
    CHECK(CALL(0x36473du, DEVICE, INPUT | RECOMP_XBOX_CACHED_ALIAS, TOTAL, 0u, 0u) == RECOMP_DSOUND_FAIL);
    CHECK(read32(OUTPUT) == retained && live(retained));
    CHECK(read32(SETTINGS + 8u) == 0u && read32(SETTINGS + 12u) == 1u);
    CHECK(allocated - freed == 3u);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 0u) == RECOMP_DSOUND_OK);
    stop_armed = 1;
    if (setjmp(stopped) == 0) { CALL(0x36478eu, DEVICE + 4u, 0u, 0u); CHECK(0); }
    stop_armed = 0;
    recomp_dsound_effects_reset();
    CHECK(allocated == freed);
    CHECK(CALL(0x36473du, DEVICE, INPUT, TOTAL, 0u, 0u) == RECOMP_DSOUND_FAIL);
    CHECK(allocated == freed);
    CHECK(CALL(0x36604au, DEVICE, LISTENER, 0u) == INVALID_EFFECT);
    recomp_dsound_effects_reset();
    CHECK(allocated == freed);
    CHECK(!output_locked);
    return 0;
}
