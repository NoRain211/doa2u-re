/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "program_manual.h"
#include "d3d8_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_draw_adapter.h"
#include "combo_hud.h"
#include "cpu_costume.h"
#include "dsound_api_adapter.h"
#include "fiber_adapter.h"
#include "free_camera_adapter.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "mwply_adapter.h"
#include "netplay_guest.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
#include "stop_report.h"
#include "stage_select.h"
#include "unlock_profile.h"
#include "pc_settings.h"
#include "runner_assets.h"
#include "menu_quit.h"
#include <stdbool.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

static void host_kernel_init_segment(void)
{
    /* XAPI 5849's void/ret-0 kernel compatibility entry patches old kernels
       and tail-calls INIT/LDT setup. The native host owns kernel lifetime. */
    fprintf(stderr, "recomp xapi: kernel INIT segment owned by host\n");
    recomp_runtime.registers.esp += 4u;
}

static void get_tick_count(void)
{
    /* 5849 reads KeTickCount. Clock reads also offer a bounded scheduler slice
       because the native host has no preemptive console clock interrupt. */
    recomp_kernel_poll();
    const uint32_t tick = (uint32_t)(recomp_xapi_performance_counter() / 1000000u);
    *recomp_memory_u32(XBOX_KERNEL_DATA_BASE + 0x40u) = tick;
    kernel_return(0u, tick);
}

static void xnet_startup(void)
{
    /* shortcut: offline host has no Xbox network stack; replace this policy
       when native online/System Link enters scope. 5849 returns INT/ret 4. */
    fprintf(stderr, "recomp xnet: startup unavailable policy=offline error=10091\n");
    kernel_return(1u, 10091u); /* Native policy: WSASYSNOTREADY. */
}

static void crt_fmod_intrinsic(void)
{
    /* CRT 0x336dfe: ST(1) is dividend, ST(0) divisor; ret 0 leaves one result.
       Native libm replaces the entire intrinsic, including its table dispatch. */
    const unsigned divisor = recomp_runtime.fpu_top;
    const unsigned result = (divisor + 1u) & 7u;
    recomp_runtime.fpu_stack[result] = fmod(
        recomp_runtime.fpu_stack[result], recomp_runtime.fpu_stack[divisor]);
    recomp_runtime.fpu_top = result;
    recomp_runtime.registers.esp += 4u;
}

static void skip_startup_legal(void)
{
    /* 0xd92c0 owns the conditional startup legal presentation. Skipping its
       menu/asset setup also avoids the corresponding cleanup; the original
       disabled branch returns directly. No arguments, void/ret 0. */
    recomp_runtime.registers.esp += 4u;
}

static void skip_startup_logo(void)
{
    /* 0xd9300 plays movie 0 (the logo clip) until it ends or a pad button is
       pressed. Keep its movie setup and teardown, including the guest's
       display-variant selection, but omit the playback wait. With 5.1 on,
       movie_open also starts the clip's AIX soundtrack (0xa94454); closing it
       while it still prepares (state byte +1 == 1) leaves CRI's audio server
       on freed handles, so yield those frames as the original's loop did.
       void/ret 0. */
    const uint32_t movie = 0u, frames = 1u;
    kernel_call_guest(0x000f9e10u, &movie, 1u);
    for (unsigned waited = 0u; waited < 120u; ++waited) {
        const uint32_t aix = *recomp_memory_u32(0x00a94454u);
        if (aix == 0u || *recomp_memory(aix + 1u, 1u) != 1u) break;
        kernel_call_guest(0x000fa3d0u, &frames, 1u); /* task_yield */
    }
    kernel_call_guest(0x000f9a10u, NULL, 0u);
    recomp_runtime.registers.esp += 4u;
}

static bool boot_opening_pending = true;
bool recomp_skip_intro;

/* The lifted projection builder. */
extern void sub_00034280(void);

/* 0x34280(matrix, fov, aspect, near, far, offset, scale) builds a camera's
   projection: X scale 1 / (tan(fov / 2) * aspect), Y scale 1 / tan(fov / 2).
   Widescreen views pass 1 / (9/16), from a 9/16 the compiler shares with
   unrelated thresholds, so the shape goes in here instead: the view keeps its
   height and widens (or, at 16:10, narrows) to the screen. cdecl, ret 0. */
static void build_projection(void)
{
    const uint32_t aspect_arg = recomp_runtime.registers.esp + 12u;
    float aspect;
    memcpy(&aspect, recomp_memory(aspect_arg, sizeof aspect), sizeof aspect);
    if (aspect == 1.0f / 0.5625f && (recomp_kernel_video_flags() & 0x00010000u) != 0u) {
        aspect = recomp_kernel_widescreen_aspect();
        memcpy(recomp_memory(aspect_arg, sizeof aspect), &aspect, sizeof aspect);
    }
    sub_00034280();
}

extern void sub_00034E10(void);
extern void sub_00036490(void);
extern void sub_0026C2E0(void);
extern void sub_00279D60(void);
extern void sub_0027A070(void);
extern void sub_0027A430(void);
extern void sub_00280460(void);

/* Camera setup (0x34E10) and a camera query (0x36490) take the horizontal
   half angle the game culls against from that same 9/16 at 0x3c2cec; left
   at 16:9, a wider screen culls whatever lies outside its middle 16:9. The
   shape goes in for these calls only, as other code reads 9/16 as a threshold;
   neither call reaches that code. Mirror cameras (0x26c2e0, 0x279d60,
   0x27a070, 0x27a430, 0x280460, every reader of 0x877ea0) build their
   projection and cone from the back buffer's 720/480 there instead. At 32:9
   Koku An's mirrored floor covered only the middle 42% and smeared its edge
   texels over the rest (issue #139), so they get the shape too; 16:9 keeps
   the game's 1.5. */
static void with_screen_float(uint32_t address, float shaped, void (*body)(void))
{
    uint8_t *slot = recomp_memory(address, sizeof(float));
    float saved;
    memcpy(&saved, slot, sizeof saved);
    if ((recomp_kernel_video_flags() & 0x00010000u) != 0u && recomp_kernel_widescreen_aspect() != 16.0f / 9.0f)
        memcpy(slot, &shaped, sizeof shaped);
    body();
    memcpy(slot, &saved, sizeof saved);
}
static void camera_setup(void) { with_screen_float(0x003c2cecu, 1.0f / recomp_kernel_widescreen_aspect(), sub_00034E10); }
static void camera_query(void) { with_screen_float(0x003c2cecu, 1.0f / recomp_kernel_widescreen_aspect(), sub_00036490); }
static void with_mirror_shape(void (*body)(void))
{
    with_screen_float(0x00877ea0u, recomp_kernel_widescreen_aspect(), body);
}
static void water_mirror_camera(void) { with_mirror_shape(sub_0026C2E0); }
static void floor_mirror_camera(void) { with_mirror_shape(sub_00279D60); }
static void floor_mirror_camera_at(void) { with_mirror_shape(sub_0027A070); }
static void floor_mirror_render(void) { with_mirror_shape(sub_0027A430); }
static void plane_mirror_camera(void) { with_mirror_shape(sub_00280460); }

void recomp_program_manual_reset(void)
{
    boot_opening_pending = true;
}

static void movie_scene_tick(void)
{
    /* 0x69180 ticks movie scenes 1 and 5: once 0xfa170 reports the movie
       finished, it sets the scene's done byte (+2) and returns the scene.
       Boot's first movie scene is the opening movie (scene 5, movie 4); end
       it on its first tick so its close tears it down. Attract replays play.
       cdecl scene pointer, ret 0. */
    const uint32_t scene = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    bool done = true;
    if (boot_opening_pending) {
        boot_opening_pending = false;
    } else {
        kernel_call_guest(0x000fa170u, NULL, 0u);
        done = recomp_runtime.registers.eax != 0u;
    }
    if (done) {
        *recomp_memory(scene + 2u, 1u) = 1u;
        recomp_runtime.registers.eax = scene;
    }
    recomp_runtime.registers.esp += 4u;
}

static void launch_new_image(void)
{
    /* XLaunchNewImageA(path, launch data) never returns. 0xda4d0 relaunches
       D:\doa2.xbe after the Start+Back+trigger soft reset; exit 4 asks the
       launcher to start a fresh runner. shortcut: launch data is dropped, so
       the relaunch boots like a cold start; carry it if a caller needs it. */
    const uint32_t path = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    const uint32_t data = *recomp_memory_u32(recomp_runtime.registers.esp + 8u);
    char name[16] = "";
    for (uint32_t i = 0u; path != 0u && i + 1u < sizeof name; ++i) {
        name[i] = (char)tolower(*recomp_memory(path + i, 1u));
        if (name[i] == '\0') break;
    }
    if (strcmp(name, "d:\\doa2.xbe") == 0) recomp_stop(4, "xapi:relaunch-title");
    if (path == 0u) {
        /* A null path opens the Dashboard. Each game caller is the player's
           Yes (Live account and network prompts, save-space prompts; issue
           #5), and the desktop stands in for the Dashboard. */
        enum { XLD_LAUNCH_DASHBOARD_ERROR = 1u };
        const uint32_t reason = data == 0u ? 0u : *recomp_memory_u32(data);
        if (reason != XLD_LAUNCH_DASHBOARD_ERROR) recomp_stop(0, "xapi:dashboard");
        recomp_stop(2, "xapi:launch-new-image:dashboard");
    }
    recomp_stop(2, "xapi:launch-new-image:%s", name);
}

RecompFunction recomp_lookup_manual(uint32_t guest_address)
{
    RecompFunction stage = recomp_stage_select_lookup_manual(guest_address);
    if (stage != NULL) return stage;
    if (guest_address == 0x000d92c0u) return skip_startup_legal;
    if (recomp_skip_intro && guest_address == 0x000d9300u) return skip_startup_logo;
    if (recomp_skip_intro && guest_address == 0x00069180u) return movie_scene_tick;
    RecompFunction netplay = recomp_netplay_guest_lookup_manual(guest_address);
    if (netplay != NULL) return netplay;
    if (guest_address == 0x002bab9du) return launch_new_image;
    if (guest_address == 0x00336dfeu) return crt_fmod_intrinsic;
    if (guest_address == 0x002bd531u) return host_kernel_init_segment;
    if (guest_address == 0x002b8a24u) return get_tick_count;
    if (guest_address == 0x00381331u) return xnet_startup;
    if (guest_address == 0x00034280u) return build_projection;
    if (guest_address == 0x00034e10u) return camera_setup;
    if (guest_address == 0x00036490u) return camera_query;
    if (guest_address == 0x0026c2e0u) return water_mirror_camera;
    if (guest_address == 0x00279d60u) return floor_mirror_camera;
    if (guest_address == 0x0027a070u) return floor_mirror_camera_at;
    if (guest_address == 0x0027a430u) return floor_mirror_render;
    if (guest_address == 0x00280460u) return plane_mirror_camera;
    RecompFunction function = recomp_d3d8_resource_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_state_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_draw_lookup_manual(guest_address);
    if (function == NULL) function = recomp_dsound_api_lookup_manual(guest_address);
    /* XAPI fibers: native Windows fibers replace the SDK stack switch. */
    if (function == NULL) function = recomp_fiber_lookup_manual(guest_address);
    if (function == NULL) function = recomp_input_lookup_manual(guest_address);
    /* CRI Sofdec: movies decode on a host thread (mwply_adapter.c). */
    if (function == NULL) function = recomp_mwply_lookup_manual(guest_address);
    /* The free camera wraps the camera build, only while it is allowed. */
    if (function == NULL) function = recomp_free_camera_lookup_manual(guest_address);
    if (function == NULL) function = recomp_cpu_costume_lookup_manual(guest_address);
    /* The combo counter, critical bar and life bar colour (combo_hud.c). */
    if (function == NULL) function = recomp_combo_hud_lookup_manual(guest_address);
    /* Unlock Everything keeps saves and awards to what was earned. */
    if (function == NULL) function = recomp_unlock_profile_lookup_manual(guest_address);
    /* PC SETTINGS in the game's Options menu. */
    if (function == NULL) function = recomp_pc_settings_lookup_manual(guest_address);
    /* Quit Game, the main menu's last entry. */
    if (function == NULL) function = recomp_menu_quit_lookup_manual(guest_address);
    /* The runner's own sprites (PC Settings' label, Quit Game's). */
    if (function == NULL) function = recomp_runner_assets_lookup_manual(guest_address);
    return function;
}
