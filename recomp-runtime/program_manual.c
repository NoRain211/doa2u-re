/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "program_manual.h"
#include "d3d8_adapter.h"
#include "d3d8_state_adapter.h"
#include "d3d8_resource_adapter.h"
#include "d3d8_draw_adapter.h"
#include "dsound_api_adapter.h"
#include "fiber_adapter.h"
#include "free_camera_adapter.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "mwply_adapter.h"
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
static char fight_names[2][16];
static uint32_t fight_local_side;
static bool fight_names_set;

void recomp_program_manual_reset(void)
{
    boot_opening_pending = true;
}

void recomp_program_set_fight_names(const char *side0, const char *side1, uint32_t local_side)
{
    const char *names[2] = {side0, side1};
    fight_names_set = false;
    for (unsigned side = 0u; side < 2u; ++side) {
        unsigned length = 0u;
        for (const char *c = names[side]; c != NULL && *c != '\0' && length + 1u < sizeof fight_names[side]; ++c)
            fight_names[side][length++] = *c >= 0x20 && *c < 0x7f ? *c : '?';
        fight_names[side][length] = '\0';
        fight_names_set |= length != 0u;
    }
    fight_local_side = local_side;
}

static void draw_fight_name(void)
{
    /* 0x12b460(side, slot) draws a side's profile name above its life bar; the
       fight HUD (0xb36a0) calls it with slot 0 when not online. cdecl, ret 0,
       AL 1 when drawn. Netplay sides load no profile, so draw each player's
       name where and how the Xbox Live HUD draws gamertags: at (177, 40),
       the local player's in yellow. */
    const uint32_t side = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    uint32_t drawn = 0u;
    if (side < 2u && fight_names[side][0] != '\0') {
        const uint32_t saved_esp = recomp_runtime.registers.esp;
        recomp_runtime.registers.esp -= 2u * sizeof fight_names[side]; /* the wide name, below the caller */
        const uint32_t name = recomp_runtime.registers.esp;
        for (unsigned i = 0u; i < sizeof fight_names[side]; ++i) {
            uint8_t *c = recomp_memory(name + 2u * i, 2u);
            c[0] = (uint8_t)fight_names[side][i];
            c[1] = 0u;
        }
        kernel_call_guest(0x0006baa0u, NULL, 0u); /* ui_draw_state_reset */
        *recomp_memory_u32(0x008a0980u) = 0x01200000u; /* text origin (0, 0x120), as the original */
        kernel_call_guest(0x00099ec0u, NULL, 0u);
        const uint32_t args[10] = {name, 0u, 177u, 40u, 4u, 0x3f800000u,
            side == fight_local_side ? 0xffffff00u : 0xffffffffu, 0x3f800000u, side == 0u ? 0x01u : 0x21u, 0xc0u};
        kernel_call_guest(0x0009f1f0u, args, 10u);
        recomp_runtime.registers.esp = saved_esp;
        drawn = 1u;
    }
    recomp_runtime.registers.eax = drawn;
    recomp_runtime.registers.esp += 4u;
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

/* Stage Select (issue #117). The game keeps 24-byte stage entries at 0x6e22b0:
   +0 Random flag, +1..3 the stage id per variant, +4 the name label, +8 and
   +0xc the 4:3 and wide thumbnails. 0x1354a0 lists a mode's entries as rows
   at 0xdf5f10 and appends Random (entry 0x15); rows are read only through
   0x135410, 0x135510 and 0x135580. These replace all four to add Tengu's
   boss stage (id 18, the Miyama his story fights use) before Random in the
   regular list. Other data follows the table, so its entry lives in the
   reserved kernel page. Labels 61, 14 and 36 are the game's unused Miyama
   name and thumbnails. */
enum {
    STAGE_ENTRIES = 0x006e22b0u,
    STAGE_ENTRY_SIZE = 0x18u,
    STAGE_MODES = 0x006e22a4u, /* per list: first entry, entry count */
    STAGE_ROW_COUNT = 0x006e22aau,
    STAGE_ROWS = 0x00df5f10u,
    STAGE_TAG_MATCH = 0x0087d49du,
    STAGE_REGULAR_LIST = 2u,
    STAGE_TAG_LIST = 1u,
    STAGE_RANDOM_ENTRY = 0x15u,
    MIYAMA_ENTRY = 0x16u,
    MIYAMA_STAGE = 18u,
};
#define MIYAMA_ENTRY_ADDRESS (XBOX_KERNEL_DATA_BASE + 0xc00u)

static uint8_t guest_byte(uint32_t address) { return *recomp_memory(address, 1u); }

static uint32_t stage_entry(uint8_t entry)
{
    return entry == MIYAMA_ENTRY ? MIYAMA_ENTRY_ADDRESS : STAGE_ENTRIES + entry * STAGE_ENTRY_SIZE;
}

static uint8_t stage_row_entry(uint32_t row)
{
    const uint32_t count = guest_byte(STAGE_ROW_COUNT);
    return guest_byte(STAGE_ROWS + (row < count ? row : count - 1u));
}

static void stage_select_build_rows(void)
{
    uint32_t list = *recomp_memory_u32(recomp_runtime.registers.esp + 4u);
    if (guest_byte(STAGE_TAG_MATCH) != 0u) list = STAGE_TAG_LIST;
    const uint8_t first = guest_byte(STAGE_MODES + list * 2u);
    const uint8_t count = guest_byte(STAGE_MODES + list * 2u + 1u);
    uint8_t *rows = recomp_memory(STAGE_ROWS, count + 2u);
    uint8_t row = 0u;
    for (; row < count; ++row) rows[row] = (uint8_t)(first + row);
    if (list == STAGE_REGULAR_LIST) {
        uint8_t *miyama = recomp_memory(MIYAMA_ENTRY_ADDRESS, STAGE_ENTRY_SIZE);
        memcpy(miyama, recomp_memory(STAGE_ENTRIES, STAGE_ENTRY_SIZE), STAGE_ENTRY_SIZE);
        miyama[1] = miyama[2] = miyama[3] = MIYAMA_STAGE;
        const uint16_t labels[3] = {61u, 14u, 36u};
        for (unsigned i = 0u; i < 3u; ++i) memcpy(miyama + 4u + i * 4u, &labels[i], 2u);
        rows[row++] = MIYAMA_ENTRY;
    }
    rows[row] = STAGE_RANDOM_ENTRY;
    *recomp_memory(STAGE_ROW_COUNT, 1u) = (uint8_t)(row + 1u);
    kernel_return(0u, row + 1u);
}

static void stage_select_row_entry(void)
{
    const uint8_t row = *recomp_memory(recomp_runtime.registers.esp + 4u, 1u);
    kernel_return(0u, stage_entry(stage_row_entry(row)));
}

static void stage_select_row_stage(void)
{
    const int8_t row = (int8_t)*recomp_memory(recomp_runtime.registers.esp + 4u, 1u);
    const uint8_t variant = *recomp_memory(recomp_runtime.registers.esp + 8u, 1u);
    kernel_return(0u, guest_byte(stage_entry(stage_row_entry(row < 0 ? 0u : (uint32_t)row)) + 1u + variant));
}

static void stage_select_find_stage(void)
{
    const uint8_t stage = *recomp_memory(recomp_runtime.registers.esp + 4u, 1u);
    const uint32_t variant_out = *recomp_memory_u32(recomp_runtime.registers.esp + 8u);
    const uint32_t count = guest_byte(STAGE_ROW_COUNT);
    for (uint32_t row = 0u; row < count; ++row) {
        const uint32_t entry = stage_entry(guest_byte(STAGE_ROWS + row));
        for (uint32_t step = 1u; step <= 3u; ++step) {
            const uint8_t variant = (uint8_t)(step % 3u); /* the game tries 1, 2, then 0 */
            const uint8_t id = guest_byte(entry + 1u + variant);
            if (id < 0x80u && id == stage) { /* Random's 0xff never matches */
                *recomp_memory(variant_out, 1u) = variant;
                kernel_return(0u, row);
                return;
            }
        }
    }
    *recomp_memory(variant_out, 1u) = 0u;
    kernel_return(0u, 0u);
}

static RecompFunction stage_select_lookup(uint32_t guest_address)
{
    switch (guest_address) {
    case 0x00135410u: return stage_select_row_entry;
    case 0x001354a0u: return stage_select_build_rows;
    case 0x00135510u: return stage_select_row_stage;
    case 0x00135580u: return stage_select_find_stage;
    default: return NULL;
    }
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
    if (guest_address >= 0x00135410u && guest_address <= 0x00135580u) {
        RecompFunction stage = stage_select_lookup(guest_address);
        if (stage != NULL) return stage;
    }
    if (guest_address == 0x000d92c0u) return skip_startup_legal;
    if (recomp_skip_intro && guest_address == 0x000d9300u) return skip_startup_logo;
    if (recomp_skip_intro && guest_address == 0x00069180u) return movie_scene_tick;
    if (fight_names_set && guest_address == 0x0012b460u) return draw_fight_name;
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
    /* CRI Sofdec: movies decode on a host thread (mwply_adapter.c). */
    if (function == NULL) function = recomp_mwply_lookup_manual(guest_address);
    /* The free camera wraps the camera build, only while it is allowed. */
    if (function == NULL) function = recomp_free_camera_lookup_manual(guest_address);
    return function;
}
