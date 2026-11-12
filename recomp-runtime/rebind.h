/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_REBIND_H
#define DOA2U_RECOMP_REBIND_H

#include "input_model.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What REBIND in the game's Button Config screen (pc_settings.c) changes:
   the key or host pad button behind each pad button (input_host_win32.h).
   The screen runs on the game's thread; the changes happen where pads are
   sampled, in recomp_rebind_filter. */

/* The next fresh press on the keyboard (pad false) or any host pad (true)
   goes to binding; a press on the other one cancels. */
void recomp_rebind_listen(unsigned binding, bool pad);
bool recomp_rebind_listening(void);
/* Gives every binding of the keyboard or the host pads its default. */
void recomp_rebind_reset(bool pad);
/* The name of what presses a binding, as the JSON files spell it; NULL for nothing. */
const char *recomp_rebind_name(unsigned binding, bool pad);
/* Saves the keys to the file RECOMP_KEYBOARD_SETTINGS names and the pad
   buttons to RECOMP_CONTROLLER_SETTINGS's, in the shape run_game.py reads,
   if anything changed. */
void recomp_rebind_save(void);
/* Sees each sampled pad first: while it listens, and after a change until
   every button is up, the game gets an idle pad. */
void recomp_rebind_filter(uint32_t port, RecompInputGamepad *pad);

#ifdef __cplusplus
}
#endif

#endif
