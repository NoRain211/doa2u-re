#ifndef DOAXBV_RECOMP_D3D_VBLANK_H
#define DOAXBV_RECOMP_D3D_VBLANK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void recomp_d3d_vblank_reset(void);
void recomp_d3d_wait_vblank(void);
void recomp_d3d_wait_present(void);
// RECOMP_PERF_COUNTER: tallies a guest BlockUntilVerticalBlank by return address and
// reports, once a second, those callers and the vblank waits Present added.
void recomp_d3d_note_block(uint32_t return_address);
// Host milliseconds the guest thread has slept in vblank waits; the pacing report resets it.
extern double recomp_d3d_vblank_wait_ms;
// RECOMP_PERF_COUNTER: per-frame breakdown appended to "recomp vblank miss" lines.
// The runner points this at the kernel's per-thread run times; submit ms is the draw-queue copy.
extern void (*recomp_d3d_frame_breakdown)(char *out, size_t size);
extern unsigned recomp_d3d_submit_count;
extern double recomp_d3d_submit_mb;
// Sleeps the calling thread until a steady_clock time, in nanoseconds since its epoch.
void recomp_d3d_sleep_until(long long steady_ns);

#ifdef __cplusplus
}
#endif

#endif
