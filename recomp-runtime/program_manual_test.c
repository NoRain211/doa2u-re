/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "program_manual.h"
#include "xbox_memory_layout.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint32_t tick, polls;
static uint32_t guest_stack[32];
static uint32_t guest_calls[4], guest_call_count, finished;
static uint8_t scene[8];
static char guest_path[16];
static uint32_t launch_reason; /* at 0x3100: the launch data's first field */
static uint32_t aix_handle; /* 0xa94454: the logo clip's AIX, or 0 */
static uint8_t aix[2];      /* at 0x4000; byte 1 is its state, 1 while preparing */
static jmp_buf stopped;
static int stop_code;
static char stop_id[64];
static float nine_sixteenths = 0.5625f, seen_shape; /* 0x3c2cec */
static float back_buffer_shape = 1.5f, seen_reflection; /* 0x877ea0 */
static uint32_t video_flags;
static float screen_aspect = 16.0f / 9.0f;
uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    if (address == XBOX_KERNEL_DATA_BASE + 0x40u) return &tick;
    if (address == 0x00a94454u) return &aix_handle;
    if (address == 0x3100u) return &launch_reason;
    if (address >= 0xfc0u && address < 0xfc0u + sizeof guest_stack && address % 4u == 0u)
        return &guest_stack[(address - 0xfc0u) / 4u];
    abort();
}
uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x3000u && address + width <= 0x3000u + sizeof guest_path)
        return (uint8_t *)&guest_path[address - 0x3000u];
    if (address >= 0x4000u && address + width <= 0x4000u + sizeof aix) return &aix[address - 0x4000u];
    if (address == 0x003c2cecu && width == sizeof nine_sixteenths) return (uint8_t *)&nine_sixteenths;
    if (address == 0x00877ea0u && width == sizeof back_buffer_shape) return (uint8_t *)&back_buffer_shape;
    if (address < 0x2000u || address + width > 0x2000u + sizeof scene) abort();
    return &scene[address - 0x2000u];
}
void recomp_stop(int fallback_code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(stop_id, sizeof stop_id, format, args);
    va_end(args);
    stop_code = fallback_code;
    longjmp(stopped, 1);
}
void recomp_kernel_poll(void) { ++polls; }
uint64_t recomp_xapi_performance_counter(void) { return UINT64_C(123456789000); }

void recomp_dispatch_indirect_site(uint32_t address, uint32_t saved_esp, const char *member, int line)
{
    (void)saved_esp;
    (void)member;
    (void)line;
    if (guest_call_count == 4u) abort();
    guest_calls[guest_call_count++] = address;
    if (address == 0x000fa3d0u) aix[1] = 2u; /* a yielded frame lets CRI's server start the AIX */
    recomp_runtime.registers.eax = address == 0x000fa170u ? finished : 0u;
}

RecompFunction recomp_d3d8_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_d3d8_resource_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_d3d8_draw_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_d3d8_state_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_dsound_api_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_netplay_guest_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_fiber_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_stage_select_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_input_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_mwply_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

RecompFunction recomp_free_camera_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_cpu_costume_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_combo_hud_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_hit_voice_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_unlock_profile_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_pc_settings_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_menu_quit_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}
RecompFunction recomp_runner_assets_lookup_manual(uint32_t address)
{
    (void)address;
    return NULL;
}

uint32_t recomp_kernel_video_flags(void) { return video_flags; }
float recomp_kernel_widescreen_aspect(void) { return screen_aspect; }
void sub_00034280(void) {}
void sub_00034E10(void) { seen_shape = nine_sixteenths; }
void sub_00036490(void) { seen_shape = nine_sixteenths; }
void sub_0026C2E0(void) { seen_reflection = back_buffer_shape; }
void sub_00279D60(void) { seen_reflection = back_buffer_shape; }
void sub_0027A070(void) { seen_reflection = back_buffer_shape; }
void sub_0027A430(void) { seen_reflection = back_buffer_shape; }
void sub_00280460(void) { seen_reflection = back_buffer_shape; }

/* The culling views see the screen's shape during the call; the shared values come back after. */
static int test_culling_shape(void)
{
    const struct { uint32_t flags; float aspect, seen, reflection; } cases[] = {
        {0x004e0000u, 32.0f / 9.0f, 0.5625f, 1.5f},     /* 4:3 mode reads 3/4 elsewhere */
        {0x004f0000u, 16.0f / 9.0f, 0.5625f, 1.5f},
        {0x004f0000u, 32.0f / 9.0f, 9.0f / 32.0f, 32.0f / 9.0f},
    };
    const uint32_t wrapped[] = {0x00034e10u, 0x00036490u};
    const uint32_t mirrors[] = {0x0026c2e0u, 0x00279d60u, 0x0027a070u, 0x0027a430u, 0x00280460u};
    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        video_flags = cases[i].flags;
        screen_aspect = cases[i].aspect;
        for (unsigned j = 0u; j < 2u; ++j) {
            seen_shape = 0.0f;
            recomp_lookup_manual(wrapped[j])();
            if (fabsf(seen_shape - cases[i].seen) > 1e-7f || nine_sixteenths != 0.5625f) return 0;
        }
        for (unsigned j = 0u; j < sizeof mirrors / sizeof mirrors[0]; ++j) {
            seen_reflection = 0.0f;
            recomp_lookup_manual(mirrors[j])();
            if (seen_reflection != cases[i].reflection || back_buffer_shape != 1.5f) return 0;
        }
    }
    video_flags = 0u;
    screen_aspect = 16.0f / 9.0f;
    return 1;
}

static int test_fmod(void)
{
    RecompFunction function = recomp_lookup_manual(0x00336dfeu);
    if (function == NULL) return 0;
    const double cases[][3] = {
        {5.5, 2.0, 1.5}, {-5.5, 2.0, -1.5}, {5.5, -2.0, 1.5},
        {0.0, 2.0, 0.0}, {-0.0, 2.0, -0.0}, {4.0, 2.0, 0.0},
        {5.0, INFINITY, 5.0}, {INFINITY, 2.0, NAN}, {1.0, 0.0, NAN},
        {NAN, 2.0, NAN}, {2.0, NAN, NAN}, {0x1p100, 3.0, 1.0},
        {0x1p-1074, 1.0, 0x1p-1074}
    };
    for (unsigned top = 0u; top < 8u; ++top) {
        for (unsigned i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            recomp_runtime.registers = (RecompRegisters){1u, 2u, 3u, 4u, 5u, 6u, 7u, 0x1000u};
            RecompRegisters expected = recomp_runtime.registers;
            expected.esp += 4u;
            recomp_runtime.fpu_top = top;
            recomp_runtime.fpu_control_word = 0x027fu;
            for (unsigned j = 0u; j < 8u; ++j) recomp_runtime.fpu_stack[j] = 100.0 + j;
            recomp_runtime.fpu_stack[top] = cases[i][1];
            recomp_runtime.fpu_stack[(top + 1u) & 7u] = cases[i][0];
            function();
            const unsigned result_top = (top + 1u) & 7u;
            const double result = recomp_runtime.fpu_stack[result_top];
            const double want = cases[i][2];
            if (memcmp(&expected, &recomp_runtime.registers, sizeof(expected)) != 0 ||
                recomp_runtime.fpu_top != result_top || recomp_runtime.fpu_control_word != 0x027fu ||
                (isnan(want) ? !isnan(result) : result != want || !!signbit(result) != !!signbit(want))) return 0;
            for (unsigned j = 0u; j < 8u; ++j) {
                if (j != top && j != result_top && recomp_runtime.fpu_stack[j] != 100.0 + j) return 0;
            }
        }
    }
    return 1;
}

static int test_startup_legal_skip(void)
{
    RecompFunction function = recomp_lookup_manual(0x000d92c0u);
    if (function == NULL) {
        fprintf(stderr, "FAIL startup legal skip is not bound\n");
        return 0;
    }
    for (unsigned boot = 0u; boot < 2u; ++boot) {
        uint32_t expected_stack[32];
        memset(guest_stack, 0xa5, sizeof guest_stack);
        memcpy(expected_stack, guest_stack, sizeof expected_stack);
        recomp_runtime.registers = (RecompRegisters){1u, 2u, 3u, 4u, 5u, 6u, 7u, 0x1000u};
        RecompRegisters expected = recomp_runtime.registers;
        expected.esp += 4u;
        function();
        if (memcmp(&expected, &recomp_runtime.registers, sizeof expected) != 0 ||
            memcmp(expected_stack, guest_stack, sizeof guest_stack) != 0) return 0;
    }
    return recomp_lookup_manual(0x000951b0u) == NULL &&
        recomp_lookup_manual(0x00092e30u) == NULL &&
        recomp_lookup_manual(0x000f9e10u) == NULL &&
        recomp_lookup_manual(0x000fa170u) == NULL && recomp_lookup_manual(0x000f9a10u) == NULL;
}

static int test_startup_logo_skip(void)
{
    RecompFunction function = recomp_lookup_manual(0x000d9300u);
    if (function == NULL) return 0;
    aix_handle = 0u;
    guest_call_count = 0u;
    recomp_runtime.registers.esp = 0x1000u;
    function();
    if (!(guest_call_count == 2u && guest_calls[0] == 0x000f9e10u &&
            guest_calls[1] == 0x000f9a10u && recomp_runtime.registers.esp == 0x1004u)) return 0;
    /* With 5.1 the clip's AIX must leave its preparing state before the close. */
    aix_handle = 0x4000u;
    aix[1] = 1u;
    guest_call_count = 0u;
    recomp_runtime.registers.esp = 0x1000u;
    function();
    return guest_call_count == 3u && guest_calls[0] == 0x000f9e10u && guest_calls[1] == 0x000fa3d0u &&
        guest_calls[2] == 0x000f9a10u && recomp_runtime.registers.esp == 0x1004u;
}

/* Boot's first movie-scene tick ends the opening at once; later ticks wait for the movie. */
static int test_boot_opening_skip(void)
{
    RecompFunction function = recomp_lookup_manual(0x00069180u);
    if (function == NULL) return 0;
    const uint32_t expected_done[] = {1u, 0u, 1u}, polled[] = {0u, 1u, 1u};
    for (unsigned i = 0u; i < 3u; ++i) {
        memset(scene, 0, sizeof scene);
        guest_call_count = 0u;
        finished = i == 2u;
        recomp_runtime.registers.esp = 0x1000u;
        recomp_runtime.registers.eax = 0x77u;
        *recomp_memory_u32(0x1004u) = 0x2000u;
        function();
        if (scene[2] != expected_done[i] || guest_call_count != polled[i] ||
            (polled[i] && guest_calls[0] != 0x000fa170u) ||
            recomp_runtime.registers.eax != (expected_done[i] ? 0x2000u : 0u) ||
            recomp_runtime.registers.esp != 0x1004u) {
            fprintf(stderr, "FAIL movie scene tick %u\n", i);
            return 0;
        }
    }
    return 1;
}

/* Only the game's own relaunch asks the launcher for a fresh runner; a chosen
   Dashboard ends the game normally and an error Dashboard still fails. */
static int test_launch_new_image(void)
{
    RecompFunction function = recomp_lookup_manual(0x002bab9du);
    if (function == NULL) return 0;
    const struct { const char *path; uint32_t reason; int code; const char *id; } cases[] = {
        {"D:\\DOA2.xbe", 0u, 4, "xapi:relaunch-title"},
        {"D:\\doa1.xbe", 0u, 2, "xapi:launch-new-image:d:\\doa1.xbe"},
        {NULL, 7u, 0, "xapi:dashboard"},
        {NULL, 1u, 2, "xapi:launch-new-image:dashboard"},
        {NULL, UINT32_MAX, 0, "xapi:dashboard"}, /* no launch data */
    };
    for (unsigned i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        memset(guest_path, 0, sizeof guest_path);
        if (cases[i].path != NULL) strcpy(guest_path, cases[i].path);
        recomp_runtime.registers.esp = 0x1000u;
        *recomp_memory_u32(0x1004u) = cases[i].path != NULL ? 0x3000u : 0u;
        *recomp_memory_u32(0x1008u) = cases[i].reason == UINT32_MAX ? 0u : 0x3100u;
        *recomp_memory_u32(0x3100u) = cases[i].reason;
        if (setjmp(stopped) == 0) {
            function();
            return 0;
        }
        if (stop_code != cases[i].code || strcmp(stop_id, cases[i].id) != 0) {
            fprintf(stderr, "FAIL launch %u: %d %s\n", i, stop_code, stop_id);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    /* The movie skips are opt-in. */
    if (recomp_lookup_manual(0x000d9300u) != NULL || recomp_lookup_manual(0x00069180u) != NULL) return 1;
    recomp_skip_intro = true;
    if (!test_fmod() || !test_startup_legal_skip() || !test_startup_logo_skip() ||
        !test_boot_opening_skip() || !test_launch_new_image() || !test_culling_shape()) return 1;
    RecompFunction init_segment = recomp_lookup_manual(0x002bd531u);
    if (init_segment == NULL) return 1;
    recomp_runtime.registers.esp = 0x1000u;
    recomp_runtime.registers.eax = 0x12345678u;
    init_segment();
    if (recomp_runtime.registers.esp != 0x1004u || recomp_runtime.registers.eax != 0x12345678u) return 1;
    RecompFunction get_tick = recomp_lookup_manual(0x002b8a24u);
    if (!get_tick) return 1;
    get_tick();
    if (recomp_runtime.registers.esp != 0x1008u ||
        recomp_runtime.registers.eax != 123456u || tick != 123456u || polls != 1u) return 1;
    RecompFunction xnet = recomp_lookup_manual(0x00381331u);
    if (!xnet) return 1;
    xnet();
    return recomp_runtime.registers.esp != 0x1010u ||
        recomp_runtime.registers.eax != 10091u || tick != 123456u || polls != 1u ||
        recomp_lookup_manual(0u) != NULL ||
        recomp_lookup_manual(1u) != NULL ||
        recomp_lookup_manual(UINT32_MAX) != NULL;
}
