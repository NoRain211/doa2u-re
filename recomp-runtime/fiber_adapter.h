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

#ifdef __cplusplus
}
#endif

#endif
