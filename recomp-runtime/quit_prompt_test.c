/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "quit_prompt.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

static RecompQuitEvent step(uint32_t port, uint16_t buttons, int a, int b, uint64_t ms, RecompInputGamepad *out)
{
    memset(out, 0, sizeof *out);
    out->buttons = buttons;
    out->analog_buttons[0] = a ? 0xffu : 0u;
    out->analog_buttons[1] = b ? 0xffu : 0u;
    return recomp_quit_prompt_filter(port, out, ms);
}

int main(void)
{
    RecompInputGamepad pad;
    expect("a short Back press passes through", step(0u, 0x20u, 0, 0, 100u, &pad) == RECOMP_QUIT_NONE &&
        pad.buttons == 0x20u);
    expect("letting go restarts the hold", step(0u, 0u, 0, 0, 1500u, &pad) == RECOMP_QUIT_NONE);
    expect("hold starts", step(0u, 0x20u, 0, 0, 2000u, &pad) == RECOMP_QUIT_NONE);
    expect("not yet after 1.9 s", step(0u, 0x20u, 0, 0, 3900u, &pad) == RECOMP_QUIT_NONE);
    expect("two seconds open the prompt and hide Back",
        step(0u, 0x20u, 0, 0, 4000u, &pad) == RECOMP_QUIT_OPENED && pad.buttons == 0u);
    expect("A still held from before the prompt does nothing", step(1u, 0u, 1, 0, 4010u, &pad) == RECOMP_QUIT_NONE &&
        pad.analog_buttons[0] == 0u);
    expect("the opening pad must let go first", step(0u, 0x20u, 0, 1, 4020u, &pad) == RECOMP_QUIT_NONE);
    step(0u, 0u, 0, 0, 4030u, &pad);
    expect("B cancels and is swallowed", step(0u, 0u, 0, 1, 4040u, &pad) == RECOMP_QUIT_CANCELLED &&
        pad.analog_buttons[1] == 0u);
    expect("B stays hidden until released", step(0u, 0u, 0, 1, 4050u, &pad) == RECOMP_QUIT_NONE &&
        pad.analog_buttons[1] == 0u);
    step(0u, 0u, 0, 0, 4055u, &pad);
    expect("input returns after release", step(0u, 0u, 1, 0, 4060u, &pad) == RECOMP_QUIT_NONE &&
        pad.analog_buttons[0] == 0xffu);
    step(2u, 0x20u, 0, 0, 5000u, &pad);
    expect("any pad opens it", step(2u, 0x20u, 0, 0, 7000u, &pad) == RECOMP_QUIT_OPENED);
    step(2u, 0u, 0, 0, 7010u, &pad);
    expect("A quits", step(2u, 0u, 1, 0, 7020u, &pad) == RECOMP_QUIT_CHOSEN);
    return failures == 0 ? 0 : 1;
}
