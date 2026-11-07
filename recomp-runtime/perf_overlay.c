/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "perf_overlay.h"
#include "input_host_win32.h"
#include "overlay.h"

#include <stdio.h>
#include <SDL3/SDL.h>

const char *recomp_perf_meter_frame(RecompPerfMeter *meter, double now_ms, bool f3_down)
{
    if (f3_down && !meter->held) meter->shown = !meter->shown;
    meter->held = f3_down;
    if (meter->last_ms != 0.0 && now_ms - meter->last_ms > meter->slowest_ms)
        meter->slowest_ms = now_ms - meter->last_ms;
    meter->last_ms = now_ms;
    if (meter->window_ms == 0.0) meter->window_ms = now_ms; /* the first frame opens the window */
    else ++meter->frames;
    const double elapsed = now_ms - meter->window_ms;
    if (elapsed >= 1000.0) {
        snprintf(meter->line, sizeof meter->line, "%.0f FPS  %.1f MS  MAX %.1f",
            meter->frames * 1000.0 / elapsed, elapsed / meter->frames, meter->slowest_ms);
        meter->window_ms = now_ms;
        meter->frames = 0u;
        meter->slowest_ms = 0.0;
    }
    return meter->shown && meter->line[0] != '\0' ? meter->line : NULL;
}

void recomp_perf_overlay(const RecompD3dPresenterTarget *target)
{
    static RecompPerfMeter meter;
    /* SDL's clock is monotonic, so clock changes cannot skew it. */
    const char *line = recomp_perf_meter_frame(&meter, (double)SDL_GetTicksNS() / 1e6,
        recomp_input_host_key(RECOMP_INPUT_HOST_KEY_F3));
    if (line == NULL) return;
    recomp_overlay_rect(4.0f, 4.0f, recomp_overlay_text_width(line, 2.0f) + 12.0f, 26.0f, 0xb0000000u);
    recomp_overlay_text(10.0f, 10.0f, 2.0f, 0xffffffffu, line);
    recomp_overlay_submit(target, recomp_overlay_display_aspect());
}
