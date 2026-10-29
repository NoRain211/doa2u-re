/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scene_freeze_adapter.h"
#include "scene_freeze.h"

#include "d3d8_adapter.h"
#include "d3d8_draw_adapter.h"
#include "runtime.h"

#include <stdio.h>

/* Scene freeze: stops the game where it is while it goes on drawing, so the
   free camera can look around a still scene. The same design as the DOA3 and
   DOAXBV ports', on DOA2U's task table.

   DOA2U runs its systems as cooperative tasks on fibers. The task table at
   0x00A94570 holds 24 records of 0x1C bytes: +0 the state (bit 0 in use,
   bit 1 paused, bit 2 running, bit 3 finished), +4 the frames to wait before
   the next turn, +8 its fiber and +0xC its function. The scheduler
   (0x000FA440) keeps the task it is running in 0x00A94814 and switches to the
   task's fiber once its wait runs out; the task's yield (0x000FA3D0) clears
   bit 2, stores its wait at +4 and switches back to the scheduler's fiber at
   0x00A94810. Tasks are created by 0x000FA570 (slot, function).

   This adapter watches every turn and counts the draws each task issues
   (scene_freeze.c keeps the history). While frozen, a turn of a task that has
   not drawn lately is declined: the switch returns at once and the record is
   left as a yield of one frame leaves it. The drawing tasks keep their turns,
   so the picture stays on screen. The cameras are built by a task that draws
   nothing (0x0003E490); recomp_scene_freeze_keep lets the free camera keep it
   running while frozen, so a frozen scene can still be looked around.

   Hiding the overlay is a flag the draw adapter reads: it leaves out the
   game's fixed-function screen-space draws, frozen or not
   (d3d8_draw_adapter.c).

   A step lets every task take its turns until the next frame is presented,
   so a frozen scene can be moved on one frame at a time. */
enum {
    TASK_TABLE = 0x00A94570u,
    TASK_RECORD_SIZE = 0x1Cu,
    TASK_COUNT = 24u,
    TASK_STATE = 0x00u,
    TASK_WAIT = 0x04u,
    TASK_FIBER = 0x08u,
    TASK_FUNCTION = 0x0Cu,
    TASK_STATE_RUNNING = 0x04u,
    TASK_CURRENT = 0x00A94814u,
    /* Fight slot 12: updates what the drawing tasks draw each frame without
       drawing itself. A frozen fight without it stops within about 35
       frames on a stale pointer in the drawing task 0x0005C3E0. */
    FIGHTER_TASK_FUNCTION = 0x00116B20u,
};

static RecompSceneFreezeModel model;
static bool frozen, overlay_hidden;
static uint32_t turn_draws;
/* While the swap counter is below this, a frozen scene runs as usual. */
static uint32_t step_until;
static RecompSceneFreezeFrameHook frame_hook;
static uint32_t kept_function;
/* No frame yet: the swap counter starts at 0. */
static uint32_t hooked_frame = UINT32_MAX;

void recomp_scene_freeze_set(bool freeze)
{
    if (frozen == freeze) return;
    frozen = freeze;
    fprintf(stderr, "recomp scene freeze: %s", freeze ? "on, drawing tasks" : "off");
    if (freeze) {
        for (uint32_t slot = 0u; slot < TASK_COUNT; ++slot) {
            uint32_t function = *recomp_memory_u32(TASK_TABLE + slot * TASK_RECORD_SIZE + TASK_FUNCTION);
            if (recomp_scene_freeze_keeps(&model, slot, function)) fprintf(stderr, " %08x", function);
        }
    }
    fputc('\n', stderr);
}

bool recomp_scene_freeze_active(void)
{
    return frozen;
}

bool recomp_scene_freeze_holding(void)
{
    return frozen && recomp_d3d8_swap_counter() >= step_until;
}

void recomp_scene_freeze_set_overlay_hidden(bool hidden)
{
    if (overlay_hidden == hidden) return;
    overlay_hidden = hidden;
    fprintf(stderr, "recomp scene freeze: overlay %s\n", hidden ? "hidden" : "shown");
}

bool recomp_scene_freeze_overlay_hidden(void)
{
    return overlay_hidden;
}

void recomp_scene_freeze_set_frame_hook(RecompSceneFreezeFrameHook hook)
{
    frame_hook = hook;
}

void recomp_scene_freeze_keep(uint32_t function)
{
    kept_function = function;
}

void recomp_scene_freeze_step(void)
{
    if (frozen) step_until = recomp_d3d8_swap_counter() + 1u;
}

bool recomp_scene_freeze_switch(uint32_t outgoing_handle, uint32_t target_handle)
{
    uint32_t slot = *recomp_memory_u32(TASK_CURRENT);
    if (slot >= TASK_COUNT) return false;
    uint32_t record = TASK_TABLE + slot * TASK_RECORD_SIZE;
    uint32_t fiber = *recomp_memory_u32(record + TASK_FIBER);
    uint32_t function = *recomp_memory_u32(record + TASK_FUNCTION);
    uint32_t draws = recomp_d3d8_draw_submitted();
    uint32_t frame = recomp_d3d8_swap_counter();
    /* Before holding is read: the hook's controls can step or thaw the scene. */
    if (frozen && frame_hook != NULL && hooked_frame != frame) {
        hooked_frame = frame;
        frame_hook(frame);
    }
    const bool holding = frozen && frame >= step_until;

    if (outgoing_handle == fiber && target_handle != fiber) {
        /* The turn is over. A frozen scene keeps its history as it stood. */
        if (!holding) recomp_scene_freeze_note_turn(&model, slot, function, draws - turn_draws);
        return false;
    }
    if (target_handle != fiber || outgoing_handle == fiber) return false;
    turn_draws = draws;
    if (!holding || recomp_scene_freeze_keeps(&model, slot, function) ||
        function == FIGHTER_TASK_FUNCTION || (kept_function != 0u && function == kept_function))
        return false;
    *recomp_memory_u32(record + TASK_STATE) &= ~TASK_STATE_RUNNING;
    *recomp_memory_u32(record + TASK_WAIT) = 1u;
    return true;
}
