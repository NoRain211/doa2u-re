/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "quit_prompt.h"

#include <stdbool.h>
#include <string.h>

enum {
    BUTTON_BACK = 0x0020u, HOLD_MS = 2000u, ANALOG_A = 0u, ANALOG_B = 1u,
    /* A stick past this reads as pushed. XInput's dead zone (7849) held a
       drifting handheld stick (GPD Win 5) latched for good, so the latch
       uses the threshold at which a stick counts as a direction instead. */
    STICK_IDLE = 16000,
};

static bool open, requested;
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
    return open || requested;
}

void recomp_quit_prompt_request(void)
{
    requested = !open;
}

static RecompQuitEvent open_prompt(void)
{
    open = true;
    requested = false;
    for (unsigned i = 0u; i < RECOMP_INPUT_PORT_COUNT; ++i) latched[i] = true;
    memset(back_held, 0, sizeof back_held);
    return RECOMP_QUIT_OPENED;
}

RecompQuitEvent recomp_quit_prompt_filter(uint32_t port, RecompInputGamepad *pad, uint64_t now_ms)
{
    if (port >= RECOMP_INPUT_PORT_COUNT) return RECOMP_QUIT_NONE;
    RecompQuitEvent event = requested ? open_prompt() : RECOMP_QUIT_NONE;
    if (pad == NULL) {
        back_held[port] = false; /* a pad plugged in later starts a fresh hold */
        return event;
    }
    if (!open) {
        const bool back = !latched[port] && (pad->buttons & BUTTON_BACK) != 0u;
        if (back && !back_held[port]) back_since[port] = now_ms;
        back_held[port] = back;
        if (back && now_ms - back_since[port] >= HOLD_MS) event = open_prompt();
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
