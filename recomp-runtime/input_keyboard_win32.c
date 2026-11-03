/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_keyboard_win32.h"

#include <string.h>

/* The presenter receives events on its worker; the guest samples elsewhere. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK lock = SRWLOCK_INIT;
#else
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define AcquireSRWLockExclusive pthread_mutex_lock
#define ReleaseSRWLockExclusive pthread_mutex_unlock
#endif
static uint8_t held[RECOMP_INPUT_KEY_COUNT], pending[RECOMP_INPUT_KEY_COUNT];
static char text[64];
static size_t text_size;

void recomp_input_keyboard_event(uint32_t key, bool down)
{
    if (key >= RECOMP_INPUT_KEY_COUNT) return;
    AcquireSRWLockExclusive(&lock);
    if (down && !held[key]) pending[key] = 1u;
    held[key] = (uint8_t)down;
    ReleaseSRWLockExclusive(&lock);
}

void recomp_input_keyboard_reset(void)
{
    AcquireSRWLockExclusive(&lock);
    memset(held, 0, sizeof held);
    memset(pending, 0, sizeof pending);
    ReleaseSRWLockExclusive(&lock);
}

void recomp_input_keyboard_sample(uint8_t keys[RECOMP_INPUT_KEY_COUNT])
{
    AcquireSRWLockExclusive(&lock);
    for (uint32_t i = 0; i < RECOMP_INPUT_KEY_COUNT; ++i) {
        keys[i] = held[i] | pending[i];
        pending[i] = 0u;
    }
    ReleaseSRWLockExclusive(&lock);
}

void recomp_input_keyboard_char(uint32_t character)
{
    if (character >= 0x80u) return;
    AcquireSRWLockExclusive(&lock);
    if (text_size < sizeof text) text[text_size++] = (char)character;
    ReleaseSRWLockExclusive(&lock);
}

size_t recomp_input_keyboard_take_text(char *out, size_t capacity)
{
    AcquireSRWLockExclusive(&lock);
    const size_t size = text_size < capacity ? text_size : capacity;
    memcpy(out, text, size);
    text_size = 0u;
    ReleaseSRWLockExclusive(&lock);
    return size;
}
