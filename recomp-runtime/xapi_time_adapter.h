#ifndef DOAXBV_RECOMP_XAPI_TIME_ADAPTER_H
#define DOAXBV_RECOMP_XAPI_TIME_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t recomp_xapi_performance_counter(void);
uint64_t recomp_xapi_performance_frequency(void);
uint64_t xbox_ReadTimeStampCounter(void);
/* Opt-in fast-forward of an idle wait; all guest counter readers share it. */
bool recomp_xapi_skip_wait(uint64_t deadline_ns);
/* Hides host time the guest spent held outside its frame loop. */
void recomp_xapi_hold_time(uint64_t held_ns);
/* Optional game hook: while it returns true, waits are skipped as if unpaced. */
extern bool (*recomp_xapi_fast_forward)(void);
RecompFunction recomp_xapi_time_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
