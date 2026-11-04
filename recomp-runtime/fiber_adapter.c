/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XAPI fibers on native Windows fibers. Ported from committed DOA3 public
   source (doa3-re fiber_adapter.c at 5d35085) without its DOAXBV probes.
   XAPILIB 5849 keeps DOA3's 16-byte fiber object but moves the TLS slots:
   the current fiber is TLS block +0xC and the converted main fiber +0x10.
   Guest FS points at the dedicated PCR block. Windows fibers hold each
   task's C continuation, so XAPI's own stack switch is never executed. */
#include "fiber_adapter.h"
#include "native_fiber.h"
#include "stop_report.h"
#include "xbox_memory_layout.h"

#include <inttypes.h>
#include <stdio.h>

enum {
    TLS_INDEX_ADDRESS = 0x00f2a4c0u,
    TLS_CURRENT_FIBER = 0x0cu,
    TLS_MAIN_FIBER = 0x10u,
    XBE_DEFAULT_STACK_SIZE = 0x00010130u,
    XAPI_MINIMUM_FIBER_STACK = 0x3000u,
};

typedef struct RecompHostFiber {
    bool active;
    uint32_t guest_handle;
    uint32_t guest_stack_base;
    uint32_t guest_stack_size;
    LPVOID native_fiber;
} RecompHostFiber;

static RecompFiberModel fiber_model;
static RecompHostFiber host_fibers[RECOMP_FIBER_MAX_COUNT];
static RecompHostFiber *current_host_fiber;
static bool converted_thread;

static uint32_t *exception_list(void)
{
    return recomp_memory_u32(XBOX_PCR_BASE);
}

static uint32_t tls_block(void)
{
    const uint32_t index = *recomp_memory_u32(TLS_INDEX_ADDRESS);
    const uint32_t slots = *recomp_memory_u32(XBOX_PCR_BASE + 4u);
    return *recomp_memory_u32(slots + index * 4u);
}

static void fail_fiber(const char *reason, uint32_t guest_handle)
{
    recomp_stop(2, "fiber:%s:0x%08" PRIx32, reason, guest_handle);
}

static void finish(uint32_t entry_esp, uint32_t argument_count, uint32_t result)
{
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp = entry_esp + 4u + argument_count * 4u;
}

static uint32_t argument(uint32_t entry_esp, uint32_t index)
{
    return *recomp_memory_u32(entry_esp + 4u + index * 4u);
}

static RecompHostFiber *host_fiber_find(uint32_t guest_handle)
{
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        if (host_fibers[i].active && host_fibers[i].guest_handle == guest_handle) return &host_fibers[i];
    }
    return NULL;
}

uint32_t recomp_fiber_thread_context(void)
{
    return fiber_model.current_handle;
}

void recomp_fiber_thread_restore(uint32_t handle)
{
    fiber_model.current_handle = handle;
    current_host_fiber = handle != 0u ? host_fiber_find(handle) : NULL;
}

const RecompFiberModel *recomp_fiber_adapter_model(void)
{
    return &fiber_model;
}

void recomp_fiber_adapter_reset(void)
{
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT; ++i) {
        RecompHostFiber *host = &host_fibers[i];
        if (host->active && host != current_host_fiber && host->guest_stack_base != 0u) {
            recomp_native_fiber_delete(host->native_fiber);
        }
        *host = (RecompHostFiber){0};
    }
    recomp_fiber_model_reset(&fiber_model);
    current_host_fiber = NULL;
    converted_thread = false;
}

static VOID WINAPI fiber_entry(void *parameter)
{
    RecompHostFiber *host = parameter;
    RecompFiber *fiber = recomp_fiber_find(&fiber_model, host->guest_handle);
    if (fiber == NULL || current_host_fiber != host ||
        fiber_model.current_handle != host->guest_handle) {
        fail_fiber("entry-context", host->guest_handle);
    }
    recomp_runtime.registers = fiber->registers;
    *exception_list() = fiber->exception_list;
    recomp_dispatch_indirect_site(fiber->entry, fiber->registers.esp, __FILE__, __LINE__);
    fail_fiber("entry-returned", host->guest_handle);
}

/* ConvertThreadToFiber(data): ret 4, returns the TLS block +0x10 object. */
static void convert_thread_to_fiber(void)
{
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    const uint32_t data = argument(entry_esp, 0u);
    const uint32_t block = tls_block();
    const uint32_t handle = block + TLS_MAIN_FIBER;
    RecompRegisters registers = recomp_runtime.registers;
    if (converted_thread || current_host_fiber != NULL) fail_fiber("duplicate-convert", handle);
    LPVOID native = recomp_native_fiber_current();
    RecompHostFiber *host = NULL;
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT && host == NULL; ++i) {
        if (!host_fibers[i].active) host = &host_fibers[i];
    }
    if (native == NULL || host == NULL ||
        recomp_fiber_add(&fiber_model, handle, 0u, data, &registers, *exception_list()) == NULL ||
        !recomp_fiber_set_current(&fiber_model, handle)) {
        fail_fiber("convert-state", handle);
    }
    *host = (RecompHostFiber){.active = true, .guest_handle = handle, .native_fiber = native};
    /* shortcut: XAPI reads the stack top from the thread object; the adapter
       entry stack stands in until a caller needs the exact bound. */
    *recomp_memory_u32(handle) = data;
    *recomp_memory_u32(handle + 4u) = entry_esp + 8u;
    *recomp_memory_u32(handle + 8u) = 0u;
    *recomp_memory_u32(block + TLS_CURRENT_FIBER) = handle;
    converted_thread = true;
    current_host_fiber = host;
    finish(entry_esp, 1u, handle);
}

/* CreateFiber(stackSize, entry, parameter): ret 0xC. */
static void create_fiber(void)
{
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t stack_size = argument(entry_esp, 0u);
    const uint32_t entry = argument(entry_esp, 1u);
    const uint32_t parameter = argument(entry_esp, 2u);
    if (!converted_thread || entry == 0u) fail_fiber("create-context", entry);
    if (stack_size == 0u) stack_size = *recomp_memory_u32(XBE_DEFAULT_STACK_SIZE);
    if (stack_size < XAPI_MINIMUM_FIBER_STACK) stack_size = XAPI_MINIMUM_FIBER_STACK;
    if (stack_size > UINT32_MAX - 0xfffu) fail_fiber("stack-size", stack_size);
    stack_size = (stack_size + 0xfffu) & ~0xfffu;
    RecompHostFiber *host = NULL;
    for (size_t i = 0u; i < RECOMP_FIBER_MAX_COUNT && host == NULL; ++i) {
        if (!host_fibers[i].active) host = &host_fibers[i];
    }
    if (host == NULL) fail_fiber("create-host-capacity", entry);
    /* The guest allocator does not reclaim stacks; reuse a slot's stack. */
    uint32_t stack_base = host->guest_stack_size >= stack_size ? host->guest_stack_base : 0u;
    if (stack_base == 0u) stack_base = xbox_HeapAlloc(stack_size, 0x1000u);
    else stack_size = host->guest_stack_size;
    if (stack_base == 0u || stack_base > UINT32_MAX - stack_size) {
        finish(entry_esp, 3u, 0u);
        return;
    }
    recomp_guest_memset(stack_base, 0, stack_size);
    const uint32_t stack_top = stack_base + stack_size;
    const uint32_t handle = stack_top - 0x10u;
    RecompRegisters registers = {0};
    registers.esp = handle - 8u;
    *recomp_memory_u32(registers.esp) = 0u;
    *recomp_memory_u32(registers.esp + 4u) = parameter;
    *recomp_memory_u32(handle) = parameter;
    *recomp_memory_u32(handle + 4u) = stack_top;
    *recomp_memory_u32(handle + 8u) = stack_base;
    *recomp_memory_u32(handle + 12u) = registers.esp;
    if (recomp_fiber_add(&fiber_model, handle, entry, parameter, &registers, 0xffffffffu) == NULL) {
        fail_fiber("create-state", handle);
    }
    LPVOID native = recomp_native_fiber_create(fiber_entry, host);
    if (native == NULL) fail_fiber("create-host", handle);
    *host = (RecompHostFiber){.active = true, .guest_handle = handle,
        .guest_stack_base = stack_base, .guest_stack_size = stack_size, .native_fiber = native};
    fprintf(stderr, "recomp fiber: created handle=0x%08" PRIx32 " entry=0x%08" PRIx32
        " parameter=0x%08" PRIx32 "\n", handle, entry, parameter);
    finish(entry_esp, 3u, handle);
}

/* SwitchToFiber(fiber): ret 4, void. */
static void switch_to_fiber(void)
{
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    const uint32_t result = recomp_runtime.registers.eax;
    const uint32_t target_handle = argument(entry_esp, 0u);
    RecompFiber *outgoing = recomp_fiber_current(&fiber_model);
    RecompFiber *target = recomp_fiber_find(&fiber_model, target_handle);
    RecompHostFiber *outgoing_host = current_host_fiber;
    RecompHostFiber *target_host = host_fiber_find(target_handle);
    if (outgoing == NULL || target == NULL || outgoing_host == NULL || target_host == NULL) {
        fail_fiber("unknown-switch", target_handle);
    }
    if (target_handle == outgoing->guest_handle) {
        finish(entry_esp, 1u, result);
        return;
    }
    outgoing->registers = recomp_runtime.registers;
    outgoing->registers.esp = entry_esp + 8u;
    /* Each Xbox thread owns its x87/SSE state; live values cross switches. */
    recomp_fpu_context_save(&outgoing->fpu);
    outgoing->exception_list = *exception_list();
    *recomp_memory_u32(outgoing->guest_handle + 12u) = outgoing->registers.esp;
    if (!recomp_fiber_set_current(&fiber_model, target_handle)) fail_fiber("switch-state", target_handle);
    recomp_runtime.registers = target->registers;
    recomp_fpu_context_restore(&target->fpu);
    *exception_list() = target->exception_list;
    *recomp_memory_u32(tls_block() + TLS_CURRENT_FIBER) = target_handle;
    current_host_fiber = target_host;
    recomp_native_fiber_switch(target_host->native_fiber);
    if (current_host_fiber != outgoing_host || fiber_model.current_handle != outgoing->guest_handle) {
        fail_fiber("resume-context", outgoing->guest_handle);
    }
    recomp_runtime.registers.eax = result;
}

/* DeleteFiber(fiber): ret 4, void. */
static void delete_fiber(void)
{
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    const uint32_t handle = argument(entry_esp, 0u);
    RecompHostFiber *host = host_fiber_find(handle);
    if (host == NULL || host == current_host_fiber || host->guest_stack_base == 0u ||
        !recomp_fiber_remove(&fiber_model, handle)) {
        fail_fiber("invalid-delete", handle);
    }
    recomp_native_fiber_delete(host->native_fiber);
    *host = (RecompHostFiber){.guest_stack_base = host->guest_stack_base,
        .guest_stack_size = host->guest_stack_size};
    finish(entry_esp, 1u, 0u);
}

RecompFunction recomp_fiber_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case RECOMP_XAPI_CREATE_FIBER: return create_fiber;
    case RECOMP_XAPI_DELETE_FIBER: return delete_fiber;
    case RECOMP_XAPI_SWITCH_TO_FIBER: return switch_to_fiber;
    case RECOMP_XAPI_CONVERT_THREAD_TO_FIBER: return convert_thread_to_fiber;
    default: return NULL;
    }
}
