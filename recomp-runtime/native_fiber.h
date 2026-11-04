#ifndef RECOMP_NATIVE_FIBER_H
#define RECOMP_NATIVE_FIBER_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "win32_compat.h"
typedef void (*LPFIBER_START_ROUTINE)(void *parameter);
#endif

void *recomp_native_fiber_current(void);
void *recomp_native_fiber_create(LPFIBER_START_ROUTINE entry, void *parameter);
void recomp_native_fiber_switch(void *target);
void recomp_native_fiber_delete(void *fiber);
/* Returns the calling thread from fiber mode; the current fiber is freed. */
void recomp_native_fiber_release_thread(void);

#endif
