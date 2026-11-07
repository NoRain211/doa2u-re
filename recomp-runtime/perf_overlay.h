/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_PERF_OVERLAY_H
#define DOA2U_RECOMP_PERF_OVERLAY_H

#include "d3d_presenter.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F3 toggles the frame rate, average frame time and slowest frame of the
   last second in the top-left corner; it starts off. */
typedef struct RecompPerfMeter {
    bool shown, held;
    double last_ms, window_ms, slowest_ms;
    unsigned frames;
    char line[48];
} RecompPerfMeter;

/* Counts one shown frame at now_ms, a monotonic clock above 0, with F3 down
   or not. Returns the line to draw, or NULL while hidden or before the first
   second has passed. */
const char *recomp_perf_meter_frame(RecompPerfMeter *meter, double now_ms, bool f3_down);
/* The runner's overlay hook: counts and draws the meter once per shown frame. */
void recomp_perf_overlay(const RecompD3dPresenterTarget *target);

#ifdef __cplusplus
}
#endif

#endif
