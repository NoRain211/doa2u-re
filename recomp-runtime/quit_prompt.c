/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "quit_prompt.h"

#include <stdbool.h>
#include <string.h>

enum {
    BUTTON_BACK = 0x0020u, HOLD_MS = 2000u, ANALOG_A = 0u, ANALOG_B = 1u,
    STICK_IDLE = 7849, /* XInput's left-stick dead zone */
};

static bool open;
static bool back_held[RECOMP_INPUT_PORT_COUNT];
static uint64_t back_since[RECOMP_INPUT_PORT_COUNT];
static bool latched[RECOMP_INPUT_PORT_COUNT]; /* swallowed until the pad is let go */

static bool pad_idle(const RecompInputGamepad *pad)
{
    if (pad->buttons != 0u) return false;
    for (unsigned i = 0u; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i)
        if (pad->analog_buttons[i] > RECOMP_INPUT_ANALOG_DOWN) return false;
    const int sticks[4] = {pad->thumb_lx, pad->thumb_ly, pad->thumb_rx, pad->thumb_ry};
    for (unsigned i = 0u; i < 4u; ++i)
        if (sticks[i] > STICK_IDLE || sticks[i] < -STICK_IDLE) return false;
    return true;
}

bool recomp_quit_prompt_open(void)
{
    return open;
}

RecompQuitEvent recomp_quit_prompt_filter(uint32_t port, RecompInputGamepad *pad, uint64_t now_ms)
{
    if (port >= RECOMP_INPUT_PORT_COUNT) return RECOMP_QUIT_NONE;
    if (pad == NULL) {
        back_held[port] = false; /* a pad plugged in later starts a fresh hold */
        return RECOMP_QUIT_NONE;
    }
    RecompQuitEvent event = RECOMP_QUIT_NONE;
    if (!open) {
        const bool back = !latched[port] && (pad->buttons & BUTTON_BACK) != 0u;
        if (back && !back_held[port]) back_since[port] = now_ms;
        back_held[port] = back;
        if (back && now_ms - back_since[port] >= HOLD_MS) {
            open = true;
            event = RECOMP_QUIT_OPENED;
            for (unsigned i = 0u; i < RECOMP_INPUT_PORT_COUNT; ++i) latched[i] = true;
            memset(back_held, 0, sizeof back_held);
        }
    } else if (!latched[port]) {
        if (pad->analog_buttons[ANALOG_A] > RECOMP_INPUT_ANALOG_DOWN) {
            event = RECOMP_QUIT_CHOSEN;
        } else if (pad->analog_buttons[ANALOG_B] > RECOMP_INPUT_ANALOG_DOWN) {
            open = false;
            latched[port] = true;
            event = RECOMP_QUIT_CANCELLED;
        }
    }
    if (latched[port] && pad_idle(pad)) latched[port] = false;
    if (open || latched[port]) memset(pad, 0, sizeof *pad);
    return event;
}
