/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kernel_abi.h"
#include "stop_report.h"
#include <inttypes.h>
#include <stdio.h>

static void bridge_rtl_initialize_critical_section(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t critical_section = *recomp_memory_u32(entry_esp + 4u);

    if (critical_section == 0u) {
        recomp_runtime.registers.eax = 0xc0000001u;
    } else {
        *recomp_memory_u32(critical_section) = 0x00000401u;
        *recomp_memory_u32(critical_section + 4u) = 0u;
        *recomp_memory_u32(critical_section + 8u) = critical_section + 8u;
        *recomp_memory_u32(critical_section + 12u) = critical_section + 8u;
        *recomp_memory_u32(critical_section + 16u) = 0xffffffffu;
        *recomp_memory_u32(critical_section + 20u) = 0u;
        *recomp_memory_u32(critical_section + 24u) = 0u;
        recomp_runtime.registers.eax = 0u;
    }
    recomp_runtime.registers.esp = entry_esp + 8u;
}

static void bridge_rtl_enter_critical_section(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t critical_section = *recomp_memory_u32(entry_esp + 4u);
    uint32_t owner;
    uint32_t recursion;
    uint32_t lock_count;

    /* The adapter dereferences its argument before anything else, so a bad
       pointer faults with no context. Report it against the argument slot. */
    if (critical_section == 0u || critical_section >= 0x04000000u) {
        fprintf(
            stderr,
            "recomp kernel: RtlEnterCriticalSection pointer 0x%08" PRIx32
            " out of guest RAM entry_esp=0x%08" PRIx32 "\n",
            critical_section,
            entry_esp);
    }

    owner = *recomp_memory_u32(critical_section + 24u);
    recursion = *recomp_memory_u32(critical_section + 20u);
    lock_count = *recomp_memory_u32(critical_section + 16u);

    if (owner != 0u && owner != recomp_kernel_current_thread_id()) {
        fprintf(
            stderr,
            "recomp kernel: contended critical section 0x%08" PRIx32
            " owner=0x%08" PRIx32 " thread=0x%08" PRIx32 "\n",
            critical_section,
            owner,
            recomp_kernel_current_thread_id());
        recomp_stop(2, "critical-section:0x%08" PRIx32, critical_section);
    }

    *recomp_memory_u32(critical_section + 4u) = 0u;
    *recomp_memory_u32(critical_section + 16u) =
        owner == recomp_kernel_current_thread_id() ? lock_count + 1u : 0u;
    *recomp_memory_u32(critical_section + 20u) =
        owner == recomp_kernel_current_thread_id() ? recursion + 1u : 1u;
    *recomp_memory_u32(critical_section + 24u) = recomp_kernel_current_thread_id();
    recomp_runtime.registers.esp = entry_esp + 8u;
    recomp_runtime.registers.eax = 0u;
}

static void bridge_rtl_leave_critical_section(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t critical_section = *recomp_memory_u32(entry_esp + 4u);
    uint32_t owner = *recomp_memory_u32(critical_section + 24u);
    uint32_t recursion = *recomp_memory_u32(critical_section + 20u);
    uint32_t lock_count = *recomp_memory_u32(critical_section + 16u);

    if (owner != recomp_kernel_current_thread_id() || recursion == 0u) {
        fprintf(
            stderr,
            "recomp kernel: invalid critical-section release 0x%08" PRIx32
            " owner=0x%08" PRIx32 " thread=0x%08" PRIx32 "\n",
            critical_section,
            owner,
            recomp_kernel_current_thread_id());
        recomp_stop(2, "critical-section:0x%08" PRIx32, critical_section);
    }

    --recursion;
    *recomp_memory_u32(critical_section + 16u) = lock_count - 1u;
    *recomp_memory_u32(critical_section + 20u) = recursion;
    if (recursion == 0u) {
        *recomp_memory_u32(critical_section + 24u) = 0u;
    }
    recomp_runtime.registers.esp = entry_esp + 8u;
    recomp_runtime.registers.eax = 0u;
    if (recursion == 0u) recomp_kernel_poll_host_waits();
}

RecompFunction recomp_kernel_startup(uint32_t ordinal)
{
    switch (ordinal) {
    case 277u: return bridge_rtl_enter_critical_section;
    case 291u: return bridge_rtl_initialize_critical_section;
    case 294u: return bridge_rtl_leave_critical_section;
    default: return NULL;
    }
}
