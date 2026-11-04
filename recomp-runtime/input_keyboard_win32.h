/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_INPUT_KEYBOARD_WIN32_H
#define DOA2U_INPUT_KEYBOARD_WIN32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RECOMP_INPUT_KEY_COUNT = 256u };

void recomp_input_keyboard_event(uint32_t key, bool down);
void recomp_input_keyboard_reset(void);
/* Consume press edges while preserving keys still held by the window. */
void recomp_input_keyboard_sample(uint8_t keys[RECOMP_INPUT_KEY_COUNT]);
/* Whether the last sample saw the key, to tell its pad alias from a pad's button. */
bool recomp_input_keyboard_sampled(uint32_t key);
/* Typed ASCII characters, including Enter, Backspace and Esc, from WM_CHAR.
   Up to 64 wait and later ones are dropped. take copies up to capacity of
   them, discards the rest and returns the count; out is not NUL-terminated. */
void recomp_input_keyboard_char(uint32_t character);
size_t recomp_input_keyboard_take_text(char *out, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
