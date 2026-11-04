/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "program_manual.h"
#include "d3d8_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_draw_adapter.h"
#include "dsound_api_adapter.h"
#include "fiber_adapter.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
#include "stop_report.h"
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
       display-variant selection, but omit the playback wait. void/ret 0. */
    const uint32_t movie = 0u;
    kernel_call_guest(0x000f9e10u, &movie, 1u);
    kernel_call_guest(0x000f9a10u, NULL, 0u);
    recomp_runtime.registers.esp += 4u;
}

static bool boot_opening_pending = true;
bool recomp_skip_intro;

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
    char name[16] = "";
    for (uint32_t i = 0u; path != 0u && i + 1u < sizeof name; ++i) {
        name[i] = (char)tolower(*recomp_memory(path + i, 1u));
        if (name[i] == '\0') break;
    }
    if (strcmp(name, "d:\\doa2.xbe") == 0) recomp_stop(4, "xapi:relaunch-title");
    recomp_stop(2, "xapi:launch-new-image:%s", path == 0u ? "dashboard" : name);
}

RecompFunction recomp_lookup_manual(uint32_t guest_address)
{
    if (guest_address == 0x000d92c0u) return skip_startup_legal;
    if (recomp_skip_intro && guest_address == 0x000d9300u) return skip_startup_logo;
    if (recomp_skip_intro && guest_address == 0x00069180u) return movie_scene_tick;
    if (guest_address == 0x002bab9du) return launch_new_image;
    if (guest_address == 0x00336dfeu) return crt_fmod_intrinsic;
    if (guest_address == 0x002bd531u) return host_kernel_init_segment;
    if (guest_address == 0x002b8a24u) return get_tick_count;
    if (guest_address == 0x00381331u) return xnet_startup;
    RecompFunction function = recomp_d3d8_resource_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_state_lookup_manual(guest_address);
    if (function == NULL) function = recomp_d3d8_draw_lookup_manual(guest_address);
    if (function == NULL) function = recomp_dsound_api_lookup_manual(guest_address);
    /* XAPI fibers: native Windows fibers replace the SDK stack switch. */
    if (function == NULL) function = recomp_fiber_lookup_manual(guest_address);
    if (function == NULL) function = recomp_input_lookup_manual(guest_address);
    return function;
}
