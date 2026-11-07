/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "perf_overlay.h"
#include "input_host_win32.h"
#include "overlay.h"

#include <stdio.h>
#include <string.h>

/* perf_overlay.c's drawing hook links against these; the test drives the meter. */
bool recomp_input_host_key(RecompInputHostKey key) { (void)key; return false; }
void recomp_overlay_rect(float x, float y, float width, float height, uint32_t argb)
{
    (void)x; (void)y; (void)width; (void)height; (void)argb;
}
float recomp_overlay_text(float x, float y, float scale, uint32_t argb, const char *text)
{
    (void)x; (void)y; (void)scale; (void)argb; (void)text;
    return 0.0f;
}
float recomp_overlay_text_width(const char *text, float scale) { (void)text; (void)scale; return 0.0f; }
void recomp_overlay_submit(const RecompD3dPresenterTarget *target, float display_aspect)
{
    (void)target; (void)display_aspect;
}
float recomp_overlay_display_aspect(void) { return 4.0f / 3.0f; }

static int failures;

static void expect(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

int main(void)
{
    RecompPerfMeter meter = {0};
    expect("hidden at first", recomp_perf_meter_frame(&meter, 100.0, false) == NULL);
    expect("F3 shows it, but nothing is measured yet", recomp_perf_meter_frame(&meter, 120.0, true) == NULL);
    /* 48 frames over the first second: 20 ms each, one of them 60 ms. */
    double ms = 120.0;
    for (unsigned i = 0u; i < 46u; ++i) {
        ms += i == 10u ? 60.0 : 20.0;
        expect("held F3 does not toggle", recomp_perf_meter_frame(&meter, ms, true) == NULL);
    }
    const char *line = recomp_perf_meter_frame(&meter, 1100.0, false);
    expect("a second of frames is reported", line != NULL && strcmp(line, "48 FPS  20.8 MS  MAX 60.0") == 0);
    line = recomp_perf_meter_frame(&meter, 1150.0, false);
    expect("the line stays until the next second", line != NULL && strcmp(line, "48 FPS  20.8 MS  MAX 60.0") == 0);
    expect("F3 again hides it", recomp_perf_meter_frame(&meter, 1170.0, true) == NULL);
    recomp_perf_meter_frame(&meter, 1190.0, false);
    expect("and F3 shows it again", recomp_perf_meter_frame(&meter, 1210.0, true) != NULL);
    line = recomp_perf_meter_frame(&meter, 2100.0, true);
    expect("the next window starts over", line != NULL && strcmp(line, "5 FPS  200.0 MS  MAX 890.0") == 0);
    return failures == 0 ? 0 : 1;
}
