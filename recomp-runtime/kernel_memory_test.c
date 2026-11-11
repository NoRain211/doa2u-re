/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_abi.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <string.h>

enum { TEST_ESP = 0x8000u, TEST_STATS = 0x9000u, TEST_BASE = 0xa000u, TEST_SIZE = 0xa004u };
static int passed;

static void check(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "kernel memory: %s was %u, expected %u\n", name, actual, expected);
        passed = 0;
    }
}

static uint32_t call(uint32_t ordinal, unsigned count, const uint32_t *arguments)
{
    uint32_t *stack = (uint32_t *)recomp_memory(TEST_ESP, (count + 1u) * sizeof *stack);
    stack[0] = 0x12345678u;
    for (unsigned i = 0u; i < count; ++i) stack[i + 1u] = arguments[i];
    recomp_runtime.registers.esp = TEST_ESP;
    RecompFunction function = recomp_kernel_memory(ordinal);
    if (function == NULL) { check("ordinal implemented", 0u, 1u); return UINT32_MAX; }
    function();
    check("ABI ESP", recomp_runtime.registers.esp, TEST_ESP + 4u * (count + 1u));
    check("ABI return slot", stack[0], 0x12345678u);
    return recomp_runtime.registers.eax;
}

#define CALL(ord, ...) call(ord, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t), (uint32_t[]){__VA_ARGS__})

static void snapshot(uint32_t words[9])
{
    *recomp_memory_u32(TEST_STATS) = 36u;
    check("query status", CALL(181u, TEST_STATS), 0u);
    memcpy(words, recomp_memory(TEST_STATS, 36u), 36u);
}

int recomp_kernel_memory_test(void)
{
    static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
    const RecompMemoryRegion region = {0u, sizeof memory, memory};
    uint32_t before[9], after[9], invalid[9], original[9];
    passed = 1;
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    if (xbox_HeapSnapshot().image_bytes == 0u) check("image initialization", xbox_HeapSetImageEnd(0x10000u), 1u);

    memset(invalid, 0xa5, sizeof invalid);
    invalid[0] = 32u;
    memcpy(original, invalid, sizeof original);
    check("null model", recomp_kernel_query_memory_statistics(NULL), 0xc000000du);
    check("invalid length model", recomp_kernel_query_memory_statistics(invalid), 0xc000000du);
    check("invalid length preserves output", memcmp(invalid, original, sizeof invalid) == 0, 1u);
    check("null bridge", CALL(181u, 0u), 0xc000000du);
    memcpy(recomp_memory(TEST_STATS, sizeof invalid), invalid, sizeof invalid);
    check("invalid length bridge", CALL(181u, TEST_STATS), 0xc000000du);
    check("invalid bridge preserves output", memcmp(recomp_memory(TEST_STATS, sizeof invalid), original, sizeof invalid) == 0, 1u);

    snapshot(before);
    check("length", before[0], 36u);
    check("64 MiB total pages", before[1], 0x4000u);
    check("image reservation", before[8], xbox_HeapSnapshot().image_bytes / 0x1000u);
    check("bootstrap unavailable", before[2] <= (XBOX_GUEST_ALLOC_LIMIT - xbox_HeapSnapshot().image_bytes) / 0x1000u, 1u);
    check("startup stack accounted", before[7] >= (XBOX_STARTUP_STACK_TOP - XBOX_STARTUP_STACK_BASE) / 0x1000u, 1u);
    check("no guest cache pages", before[5], 0u);

    check("raw heap allocation", xbox_HeapAlloc(0x1000u, 0x1000u) != 0u, 1u);
    snapshot(after);
    check("raw allocation reduces arena", after[2], before[2] - 1u);
    check("raw allocation has no pool category", after[6], before[6]);
    memcpy(before, after, sizeof before);

    uint32_t pool = CALL(14u, 0x2000u);
    check("pool allocation", pool != 0u, 1u);
    snapshot(after);
    check("pool budget", after[2], before[2] - 2u);
    check("pool pages", after[6], before[6] + 2u);
    CALL(17u, pool);
    snapshot(after);
    check("pool freed budget", after[2], before[2]);
    check("pool freed category", after[6], before[6]);
    check("pool reuse address", CALL(14u, 0x2000u), pool);
    snapshot(after);
    check("reuse consumes free capacity", after[2], before[2] - 2u);
    CALL(17u, pool);

    uint32_t stack = CALL(169u, 0x1000u, 0u);
    snapshot(after);
    check("stack capacity", after[7], before[7] + 1u);
    check("stack budget", after[2], before[2] - 1u);
    CALL(170u, stack, stack - 0x1000u);

    uint32_t contiguous = CALL(165u, 0x2000u);
    check("contiguous allocation", contiguous != 0u, 1u);
    snapshot(after);
    check("contiguous budget", after[2], before[2] - 2u);
    check("contiguous pool category", after[6], before[6] + 2u);
    CALL(171u, contiguous);
    check("contiguous reuse address", CALL(165u, 0x2000u), contiguous);
    CALL(171u, contiguous);

    *recomp_memory_u32(TEST_BASE) = 0u;
    *recomp_memory_u32(TEST_SIZE) = 0x1000u;
    check("virtual allocation", CALL(184u, TEST_BASE, 0u, TEST_SIZE, 0x3000u, 4u), 0u);
    snapshot(after);
    check("virtual committed capacity", after[3], before[3] + 0x1000u);
    check("virtual reserved capacity", after[4], before[4] + 0x1000u);
    check("virtual budget", after[2], before[2] - 1u);
    check("virtual free", CALL(199u, TEST_BASE, TEST_SIZE, 0x8000u), 0u);
    snapshot(after);
    check("final available budget", after[2], before[2]);
    check("final virtual committed", after[3], before[3]);
    check("final virtual reserved", after[4], before[4]);
    return passed;
}
