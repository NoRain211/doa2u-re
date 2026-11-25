#include "kernel_abi.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <string.h>

enum {
    TEST_MEMORY_BASE = 0x29000000u,
    TEST_MEMORY_SIZE = 0x00001000u,
    TEST_ENTRY_ESP = TEST_MEMORY_BASE + 0x100u,
    TEST_THREAD = XBOX_STARTUP_THREAD_OBJECT,
};

static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "kernel thread: %s was 0x%08x, expected 0x%08x\n",
        field,
        actual,
        expected);
    return 0;
}

static void prepare_stack(uint8_t *memory, uint32_t priority, int has_priority)
{
    uint32_t *stack = (uint32_t *)(memory + (TEST_ENTRY_ESP - TEST_MEMORY_BASE));

    stack[0] = 0x0010abcdu;
    stack[1] = TEST_THREAD;
    if (has_priority) {
        stack[2] = priority;
    }
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
}

enum { TEST_APC_ROUTINE = 0x00123450u };
static uint32_t apc_seen[3];
static unsigned apc_calls;

static void apc_probe(void)
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        apc_seen[i] = *recomp_memory_u32(recomp_runtime.registers.esp + 4u + i * 4u);
    }
    ++apc_calls;
    recomp_runtime.registers.esp += 16u;
}

static RecompFunction apc_lookup(uint32_t address)
{
    return address == TEST_APC_ROUTINE ? apc_probe : NULL;
}

/* An I/O APC queued by NtReadFile runs at the next alertable wait, once. */
static int test_user_apc(uint8_t *memory)
{
    uint32_t *stack = (uint32_t *)(memory + (TEST_ENTRY_ESP - TEST_MEMORY_BASE));
    RecompFunction delay = recomp_kernel_thread(99u);
    int passed = 1;

    recomp_runtime_set_lookup(apc_lookup);
    recomp_kernel_queue_user_apc(TEST_APC_ROUTINE, 0x11u, 0x22u);
    for (int round = 0; round < 2; ++round) {
        stack[0] = 0x0010abcdu;
        stack[1] = 1u;      /* UserMode */
        stack[2] = 1u;      /* Alertable */
        stack[3] = 0u;
        recomp_runtime.registers.esp = TEST_ENTRY_ESP;
        delay();
        passed &= expect_u32("delay ESP", recomp_runtime.registers.esp,
                             TEST_ENTRY_ESP + 16u);
        passed &= expect_u32("delay status", recomp_runtime.registers.eax,
                             round == 0 ? 0xc0u : 0u);
    }
    passed &= expect_u32("apc calls", apc_calls, 1u);
    passed &= expect_u32("apc context", apc_seen[0], 0x11u);
    passed &= expect_u32("apc status block", apc_seen[1], 0x22u);
    recomp_runtime_set_lookup(NULL);
    return passed;
}

int recomp_kernel_thread_test(void)
{
    static uint8_t memory[TEST_MEMORY_SIZE];
    const RecompMemoryRegion region = {
        .address = TEST_MEMORY_BASE,
        .size = sizeof memory,
        .data = memory,
    };
    RecompFunction query;
    RecompFunction set;
    uint32_t stack_before[3];
    int passed = 1;

    memset(memory, 0, sizeof memory);
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    query = recomp_kernel_thread(124u);
    set = recomp_kernel_thread(143u);
    passed &= expect_u32("query lookup", query != NULL, 1u);
    passed &= expect_u32("set lookup", set != NULL, 1u);
    passed &= expect_u32(
        "neighbor lookup", recomp_kernel_thread(123u) != NULL, 0u);
    passed &= expect_u32(
        "interrupt time lookup", recomp_kernel_thread(125u) != NULL, 1u);
    if (query == NULL || set == NULL) {
        return 0;
    }

    prepare_stack(memory, 0u, 0);
    memcpy(stack_before, memory + (TEST_ENTRY_ESP - TEST_MEMORY_BASE), 8u);
    query();
    passed &= expect_u32("initial priority", recomp_runtime.registers.eax, 0u);
    passed &= expect_u32(
        "query ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
    passed &= expect_u32(
        "query preserves stack",
        memcmp(stack_before, memory + (TEST_ENTRY_ESP - TEST_MEMORY_BASE), 8u),
        0u);

    prepare_stack(memory, 5u, 1);
    set();
    passed &= expect_u32("set previous", recomp_runtime.registers.eax, 0u);
    passed &= expect_u32(
        "set ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 12u);

    prepare_stack(memory, 0u, 0);
    query();
    passed &= expect_u32("query set priority", recomp_runtime.registers.eax, 5u);

    prepare_stack(memory, 0xfffffffdu, 1);
    set();
    passed &= expect_u32("negative set previous", recomp_runtime.registers.eax, 5u);

    prepare_stack(memory, 0u, 0);
    query();
    passed &= expect_u32(
        "negative query priority", recomp_runtime.registers.eax, 0xfffffffdu);

    passed &= test_user_apc(memory);
    return passed;
}
