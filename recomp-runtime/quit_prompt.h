/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_QUIT_PROMPT_H
#define DOA2U_RECOMP_QUIT_PROMPT_H

#include "input_model.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RecompQuitEvent {
    RECOMP_QUIT_NONE,
    RECOMP_QUIT_OPENED,
    RECOMP_QUIT_CANCELLED,
    RECOMP_QUIT_CHOSEN,
} RecompQuitEvent;

/* Holding Back for two seconds on any pad asks whether to quit to the desktop
   (issue #59): A quits, B cancels. The game sees no input from a pad while
   the prompt is open, nor afterwards until that pad is let go. now_ms is a
   monotonic host clock; a null pad is an absent one. */
RecompQuitEvent recomp_quit_prompt_filter(uint32_t port, RecompInputGamepad *pad, uint64_t now_ms);
bool recomp_quit_prompt_open(void);

#ifdef __cplusplus
}
#endif

#endif
