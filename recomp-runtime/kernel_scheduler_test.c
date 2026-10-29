/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_abi.h"
#include "xbox_memory_layout.h"
#include "xapi_time_adapter.h"

#include <stdio.h>
#include <string.h>

enum { WORKER = 0x123400u, APC = 0x123410u, HOST_PEER = 0x123420u, HOST_SPINNER = 0x123430u,
    STATE = 0x2000u, LOCK = 0x2100u, MAIN_ESP = 0x8000u };
static uint32_t events[2], handles[2], turns[2], apcs[2];
static int passed = 1;

static void check(const char *name, uint64_t actual, uint64_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "kernel scheduler: %s was %llu, expected %llu\n",
            name, (unsigned long long)actual, (unsigned long long)expected);
        passed = 0;
    }
}

static uint32_t call(uint32_t ordinal, unsigned count, const uint32_t *arguments)
{
    uint32_t esp = recomp_runtime.registers.esp;
    for (unsigned i = count; i != 0u; --i) {
        recomp_runtime.registers.esp -= 4u;
        *recomp_memory_u32(recomp_runtime.registers.esp) = arguments[i - 1u];
    }
    recomp_runtime.registers.esp -= 4u;
    *recomp_memory_u32(recomp_runtime.registers.esp) = 0u;
    RecompFunction function = recomp_lookup_kernel(0x80000000u | ordinal);
    if (function == NULL) { check("kernel lookup", 0u, 1u); return 0xffffffffu; }
    function();
    check("stdcall stack", recomp_runtime.registers.esp, esp);
    return recomp_runtime.registers.eax;
}

#define KERNEL(ord, ...) call(ord, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t), (uint32_t[]){__VA_ARGS__})

static void apc(void)
{
    uint32_t index = kernel_arg(1u);
    check("APC thread", recomp_kernel_current_thread_id(), handles[index]);
    ++apcs[index];
    kernel_return(3u, 0u);
}

static void worker(void)
{
    uint32_t index = kernel_arg(1u);
    uint32_t object = *recomp_memory_u32(XBOX_PCR_BASE + 0x28u);
    uint32_t tls = *recomp_memory_u32(XBOX_PCR_BASE + 4u);
    check("worker PCR self", *recomp_memory_u32(XBOX_PCR_BASE + 0x1cu), XBOX_PCR_BASE);
    check("worker PRCB", *recomp_memory_u32(XBOX_PCR_BASE + 0x20u), XBOX_PCR_BASE + 0x28u);
    for (uint32_t turn = 0u; turn < 3u; ++turn) {
        check("event wait", KERNEL(234u, events[index], 1u, 0u, 0u), 0u);
        recomp_runtime.registers.ebx = 0x11110000u + index;
        recomp_runtime.mmx[0].q = 0x1234567800000000ull + index;
        recomp_runtime.direction_flag = (int)index;
        recomp_runtime.arithmetic_flags = 0x40u * index;
        *recomp_memory_u32(XBOX_PCR_BASE) = 0xaabb0000u + index;
        *recomp_memory_u32(RECOMP_XBOX_CACHED_ALIAS + 0x278u) = 0xdead0000u + index;
        recomp_kernel_queue_user_apc(APC, index, 0u);
        check("yield", call(238u, 0u, NULL), 0u);
        check("thread identity", recomp_kernel_current_thread_id(), handles[index]);
        check("PCR", *recomp_memory_u32(XBOX_PCR_BASE + 0x28u), object);
        check("TLS", *recomp_memory_u32(XBOX_PCR_BASE + 4u), tls);
        check("SEH", *recomp_memory_u32(XBOX_PCR_BASE), 0xaabb0000u + index);
        check("physical page zero", *recomp_memory_u32(0u), 0xccccccccu);
        check("PCR debug callback", *recomp_memory_u32(XBOX_PCR_BASE + 0x278u), 0u);
        check("register", recomp_runtime.registers.ebx, 0x11110000u + index);
        check("MMX", recomp_runtime.mmx[0].q, 0x1234567800000000ull + index);
        check("direction", recomp_runtime.direction_flag, index);
        check("arithmetic flags", recomp_runtime.arithmetic_flags, 0x40u * index);
        check("alertable APC", KERNEL(99u, 1u, 1u, 0u), 0xc0u);
        check("APC count", apcs[index], turn + 1u);
        ++turns[index];
        check("set peer event", KERNEL(225u, events[1u - index], 0u), 0u);
    }
    kernel_return(2u, 0u);
}

/* Host work ends a wait only once it reports done; meanwhile peers run. */
static int host_peer_ran;

static void host_peer(void)
{
    host_peer_ran = 1;
    kernel_return(2u, 0u);
}

static bool host_peer_done(void *context)
{
    (void)context;
    return host_peer_ran != 0;
}

/* A guest thread polling under a lock for a host read's result never waits,
   so the read's thread must still resume once the read is done. */
static unsigned host_spins;
static int host_spinner_released, host_spinner_saw_release;

static void host_spinner(void)
{
    const uint64_t limit = recomp_xapi_performance_counter() + UINT64_C(2000000000);
    while (!host_spinner_released && recomp_xapi_performance_counter() < limit) {
        KERNEL(277u, LOCK);
        KERNEL(294u, LOCK);
        ++host_spins;
    }
    host_spinner_saw_release = host_spinner_released;
    kernel_return(2u, 0u);
}

static bool host_spin_done(void *context)
{
    (void)context;
    return host_spins >= 100u;
}

static RecompFunction lookup(uint32_t address)
{
    if (address == WORKER) return worker;
    if (address == HOST_PEER) return host_peer;
    if (address == HOST_SPINNER) return host_spinner;
    if (address == APC) return apc;
    return recomp_lookup_kernel(address);
}

int main(void)
{
    static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
    const RecompMemoryRegion region = {0u, sizeof memory, memory};
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    check("image boundary", xbox_HeapSetImageEnd(0x10000u), 1u);
    recomp_runtime_set_lookup(lookup);
    recomp_runtime.registers.esp = MAIN_ESP;
    recomp_guest_memset(RECOMP_XBOX_CACHED_ALIAS, 0xcc, 0x1000u);
    *recomp_memory_u32(XBOX_PCR_BASE) = 0x1122abcdu;
    *recomp_memory_u32(XBOX_PCR_BASE + 4u) = XBOX_STARTUP_THREAD_STACK_SLOT + 0x44u;
    *recomp_memory_u32(XBOX_PCR_BASE + 0x1cu) = XBOX_PCR_BASE;
    *recomp_memory_u32(XBOX_PCR_BASE + 0x20u) = XBOX_PCR_BASE + 0x28u;
    *recomp_memory_u32(XBOX_PCR_BASE + 0x28u) = XBOX_STARTUP_THREAD_OBJECT;
    for (unsigned i = 0u; i < 2u; ++i) {
        check("create event", KERNEL(189u, STATE, 0u, 1u, i == 0u), 0u);
        events[i] = *recomp_memory_u32(STATE);
        check("create thread", KERNEL(255u, STATE, 0u, 0x4000u, 0x40u, 0u,
            WORKER, i, 1u, 0u, 0u), 0u);
        handles[i] = *recomp_memory_u32(STATE);
    }
    for (unsigned i = 0u; i < 2u; ++i) check("resume", KERNEL(224u, handles[i], 0u), 0u);
    check("close thread handle", KERNEL(187u, handles[0]), 0u);
    check("unknown close", KERNEL(187u, 0xdeadbeefu), 0xc0000008u);
    check("repeat close", KERNEL(187u, handles[0]), 0xc0000008u);
    check("closed wait handle", KERNEL(234u, handles[0], 1u, 0u, 0u), 0xc0000008u);
    check("closed resume handle", KERNEL(224u, handles[0], 0u), 0xc0000008u);
    const uint64_t limit = recomp_xapi_performance_counter() + UINT64_C(2000000000);
    while ((turns[0] < 3u || turns[1] < 3u) && recomp_xapi_performance_counter() < limit) {
        recomp_kernel_poll();
    }
    check("busy-poll peer progress", turns[0] + turns[1], 6u);
    recomp_kernel_run_threads();
    check("worker zero", turns[0], 3u);
    check("worker one", turns[1], 3u);
    check("create host peer", KERNEL(255u, STATE, 0u, 0x4000u, 0x40u, 0u,
        HOST_PEER, 0u, 0u, 0u, 0u), 0u);
    check("host wait", recomp_kernel_wait_host(host_peer_done, NULL), 1u);
    check("host wait ran peer", host_peer_ran, 1u);
    check("init lock", KERNEL(291u, LOCK), 0u);
    check("create host spinner", KERNEL(255u, STATE, 0u, 0x4000u, 0x40u, 0u,
        HOST_SPINNER, 0u, 0u, 0u, 0u), 0u);
    check("host wait under spin", recomp_kernel_wait_host(host_spin_done, NULL), 1u);
    host_spinner_released = 1;
    recomp_kernel_run_threads();
    check("spinner yielded to finished host wait", host_spinner_saw_release, 1u);
    check("main stack", recomp_runtime.registers.esp, MAIN_ESP);
    check("main PCR thread restored", *recomp_memory_u32(XBOX_PCR_BASE + 0x28u), XBOX_STARTUP_THREAD_OBJECT);
    check("main TLS restored", *recomp_memory_u32(XBOX_PCR_BASE + 4u), XBOX_STARTUP_THREAD_STACK_SLOT + 0x44u);
    check("main SEH restored", *recomp_memory_u32(XBOX_PCR_BASE), 0x1122abcdu);
    check("physical page zero survives scheduling", *recomp_memory_u32(0u), 0xccccccccu);
    check("cached physical write survives scheduling", *recomp_memory_u32(0x278u) & 0xffff0000u, 0xdead0000u);
    return passed ? 0 : 1;
}
