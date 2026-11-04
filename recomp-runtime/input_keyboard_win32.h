/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_INPUT_KEYBOARD_WIN32_H
#define DOA2U_INPUT_KEYBOARD_WIN32_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RECOMP_INPUT_KEY_COUNT = 256u };

void recomp_input_keyboard_event(uint32_t key, bool down);
void recomp_input_keyboard_reset(void);
/* Consume press edges while preserving keys still held by the window. */
void recomp_input_keyboard_sample(uint8_t keys[RECOMP_INPUT_KEY_COUNT]);

#ifdef __cplusplus
}
#endif
#endif
