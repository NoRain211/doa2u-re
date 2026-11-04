#ifndef DOAXBV_RECOMP_XAPI_TIME_ADAPTER_H
#define DOAXBV_RECOMP_XAPI_TIME_ADAPTER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t recomp_xapi_performance_counter(void);
uint64_t recomp_xapi_performance_frequency(void);
uint64_t xbox_ReadTimeStampCounter(void);
/* Frame-locked guest time for run-ahead and rollback. While enabled, each
   Swap advances the counter one 60 Hz frame and waits inside a frame advance
   it to their deadline, so replaying the same frames reads the same times.
   RECOMP_FRAME_CLOCK=1 enables it at startup. */
typedef struct RecompFrameClockState {
    uint64_t base_ns;
    uint64_t offset_ns;
    uint64_t wait_ns;
    uint64_t frame;
    bool enabled;
} RecompFrameClockState;
bool recomp_xapi_frame_clock_enabled(void);
void recomp_xapi_frame_clock_set_enabled(bool enabled);
void recomp_xapi_frame_clock_reset(void);
void recomp_xapi_frame_clock_advance(void);
void recomp_xapi_frame_clock_save(RecompFrameClockState *state);
void recomp_xapi_frame_clock_restore(const RecompFrameClockState *state);
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
