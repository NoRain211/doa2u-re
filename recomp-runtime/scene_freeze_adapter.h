/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_SCENE_FREEZE_ADAPTER_H
#define DOA2U_RECOMP_SCENE_FREEZE_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Freezes and thaws the running scene; see scene_freeze_adapter.c. */
void recomp_scene_freeze_set(bool frozen);
bool recomp_scene_freeze_active(void);
/* Stops drawing the 2D overlay, frozen or not: the life bars, timer, names
   and menus. */
void recomp_scene_freeze_set_overlay_hidden(bool hidden);
bool recomp_scene_freeze_overlay_hidden(void);
/* True while the scene is frozen and not stepping a frame. */
bool recomp_scene_freeze_holding(void);
/* A task function that keeps its turns while frozen, 0 for none. */
void recomp_scene_freeze_keep(uint32_t function);
/* Lets a frozen scene run one more frame, then holds it again. */
void recomp_scene_freeze_step(void);
/* Called once a frame while the scene is frozen, from the scheduler, since
   the game's own input polling is frozen with it. */
typedef void (*RecompSceneFreezeFrameHook)(uint32_t frame);
void recomp_scene_freeze_set_frame_hook(RecompSceneFreezeFrameHook hook);
/* The fiber adapter's switch hook (recomp_fiber_set_switch_hook). */
bool recomp_scene_freeze_switch(uint32_t outgoing_handle, uint32_t target_handle);

#ifdef __cplusplus
}
#endif

#endif
