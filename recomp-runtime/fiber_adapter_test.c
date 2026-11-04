/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fiber_adapter.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <string.h>

enum {
    TLS_INDEX = 0x00f2a4c0u,
    TLS_SLOTS = 0x00f30000u,
    TLS_BLOCK = 0x00f30100u,
    ENTRY_ESP = 0x00f31000u,
    FIBER_ENTRY = 0x000b5570u,
    STACK_SIZE = 0x3000u,
};

static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
static int passed = 1;
static uint32_t entry_count;

static void check(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) return;
    fprintf(stderr, "fiber adapter: %s was 0x%08x, expected 0x%08x\n", field, actual, expected);
    passed = 0;
}

static uint32_t call(uint32_t address, uint32_t count, const uint32_t *arguments)
{
    uint32_t *stack = recomp_memory_u32(ENTRY_ESP);
    stack[0] = 0x0010abcdu;
    for (uint32_t i = 0u; i < count; ++i) stack[i + 1u] = arguments[i];
    recomp_runtime.registers.esp = ENTRY_ESP;
    recomp_fiber_lookup_manual(address)();
    check("caller ESP", recomp_runtime.registers.esp, ENTRY_ESP + 4u + count * 4u);
    return recomp_runtime.registers.eax;
}

/* A guest task: checks its parameter and TLS publication, then yields back. */
static void test_fiber_entry(void)
{
    const uint32_t esp = recomp_runtime.registers.esp;
    const uint32_t handle = recomp_fiber_adapter_model()->current_handle;
    check("entry parameter", *recomp_memory_u32(esp + 4u), 0x22222222u);
    for (;;) {
        ++entry_count;
        check("task TLS fiber", *recomp_memory_u32(TLS_BLOCK + 0xcu), handle);
        recomp_runtime.registers.ebx = 0x12345678u;
        *recomp_memory_u32(XBOX_PCR_BASE) = 0x87654321u;
        recomp_runtime.registers.esp = esp - 8u;
        *recomp_memory_u32(esp - 4u) = TLS_BLOCK + 0x10u;
        recomp_fiber_lookup_manual(RECOMP_XAPI_SWITCH_TO_FIBER)();
        check("task resumed ESP", recomp_runtime.registers.esp, esp);
        check("task resumed EBX", recomp_runtime.registers.ebx, 0x12345678u);
        check("task resumed SEH", *recomp_memory_u32(XBOX_PCR_BASE), 0x87654321u);
    }
}

int main(void)
{
    const RecompMemoryRegion region = {0u, sizeof memory, memory};
    const RecompFunctionEntry functions[] = {{FIBER_ENTRY, test_fiber_entry}};
    recomp_runtime_init(&region, 1u, NULL, 0u, functions, 1u);
    check("heap", xbox_HeapSetImageEnd(0x01000000u), 1u);
    recomp_fiber_adapter_reset();
    *recomp_memory_u32(XBOX_PCR_BASE) = 0xffffffffu;
    *recomp_memory_u32(XBOX_PCR_BASE + 4u) = TLS_SLOTS;
    *recomp_memory_u32(TLS_INDEX) = 1u;
    *recomp_memory_u32(TLS_SLOTS + 4u) = TLS_BLOCK;
    *recomp_memory_u32(0x00010130u) = 0x5000u;

    uint32_t arguments[3] = {0xabcdef01u};
    const uint32_t main_fiber = call(RECOMP_XAPI_CONVERT_THREAD_TO_FIBER, 1u, arguments);
    check("main fiber is TLS block +0x10", main_fiber, TLS_BLOCK + 0x10u);
    check("main fiber data", *recomp_memory_u32(main_fiber), 0xabcdef01u);
    check("main fiber published", *recomp_memory_u32(TLS_BLOCK + 0xcu), main_fiber);

    arguments[0] = 0u; arguments[1] = FIBER_ENTRY; arguments[2] = 0x11111111u;
    const uint32_t first = call(RECOMP_XAPI_CREATE_FIBER, 3u, arguments);
    check("XBE default stack size", *recomp_memory_u32(first + 4u) - *recomp_memory_u32(first + 8u), 0x5000u);
    const uint32_t checkpoint = xbox_HeapCheckpoint();
    arguments[0] = first;
    call(RECOMP_XAPI_DELETE_FIBER, 1u, arguments);
    *recomp_memory_u32(*recomp_memory_u32(first + 8u) + 0x100u) = 0xdeadbeefu;

    arguments[0] = STACK_SIZE; arguments[1] = FIBER_ENTRY; arguments[2] = 0x22222222u;
    const uint32_t second = call(RECOMP_XAPI_CREATE_FIBER, 3u, arguments);
    check("stack slot reused", second, first);
    check("guest heap unchanged", xbox_HeapCheckpoint(), checkpoint);
    check("reused stack cleared", *recomp_memory_u32(*recomp_memory_u32(second + 8u) + 0x100u), 0u);
    check("object parameter", *recomp_memory_u32(second), 0x22222222u);

    for (uint32_t i = 1u; i <= 2u; ++i) {
        arguments[0] = second;
        recomp_runtime.registers.ebx = 0xabcdef12u;
        call(RECOMP_XAPI_SWITCH_TO_FIBER, 1u, arguments);
        check("task ran and yielded", entry_count, i);
        check("main EBX", recomp_runtime.registers.ebx, 0xabcdef12u);
        check("main SEH", *recomp_memory_u32(XBOX_PCR_BASE), 0xffffffffu);
        check("main TLS fiber", *recomp_memory_u32(TLS_BLOCK + 0xcu), main_fiber);
    }
    check("thread context", recomp_fiber_thread_context(), main_fiber);
    arguments[0] = second;
    call(RECOMP_XAPI_DELETE_FIBER, 1u, arguments);
    recomp_fiber_adapter_reset();
    if (passed) puts("PASS XAPI 5849 fibers: create, delete reuse, switch/yield, TLS and SEH");
    return passed ? 0 : 1;
}
