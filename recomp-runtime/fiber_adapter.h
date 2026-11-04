/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_FIBER_ADAPTER_H
#define DOA2U_RECOMP_FIBER_ADAPTER_H

#include "fiber_model.h"
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Verified XAPILIB 5849 entry points in the selected DOA2U executable. */
enum {
    RECOMP_XAPI_CREATE_FIBER = 0x002bb76au,
    RECOMP_XAPI_DELETE_FIBER = 0x002bb7f6u,
    RECOMP_XAPI_SWITCH_TO_FIBER = 0x002bb809u,
    RECOMP_XAPI_CONVERT_THREAD_TO_FIBER = 0x002bb848u,
};

RecompFunction recomp_fiber_lookup_manual(uint32_t guest_address);
/* The guest-thread scheduler saves and restores each thread's current fiber. */
uint32_t recomp_fiber_thread_context(void);
void recomp_fiber_thread_restore(uint32_t handle);
void recomp_fiber_adapter_reset(void);
const RecompFiberModel *recomp_fiber_adapter_model(void);
/* Guest fiber contexts and the current fiber. Native fiber handles stay
   with the host; restore finds the parked host fiber by guest handle. */
size_t recomp_fiber_adapter_state_size(void);
void recomp_fiber_adapter_state_save(void *state);
void recomp_fiber_adapter_state_restore(const void *state);
/* Guest stacks owned by active XAPI fibers; the stack is scratch, not state. */
typedef struct RecompFiberStackRange {
    uint32_t base;
    uint32_t size;
} RecompFiberStackRange;
size_t recomp_fiber_adapter_stack_ranges(RecompFiberStackRange *ranges, size_t capacity);
/* False when a saved fiber's host slot was reused, so its continuation is
   gone. A deleted fiber stays parked and is restorable. */
bool recomp_fiber_adapter_state_restorable(const void *state);
/* Compares the guest-visible fiber model and slot bookkeeping. Host stack
   bytes hold dead locals and are not part of the simulation. */
bool recomp_fiber_adapter_state_equal(const void *left, const void *right, size_t size);

#ifdef __cplusplus
}
#endif

#endif
