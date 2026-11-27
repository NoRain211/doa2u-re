/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic test for game_state.c's Live screen read: the Live page on top of
   the page stack, the page names, and the fields the Live log prints. */

#include "game_state.h"

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "game_state_test: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;

uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= RECOMP_XBOX_RAM_SIZE || width > RECOMP_XBOX_RAM_SIZE - address) {
        fprintf(stderr, "game_state_test: guest address %08x+%zu is outside guest RAM\n", address, width);
        abort();
    }
    return recomp_fast_ram + address;
}

uint32_t *recomp_memory_u32_checked(uint32_t address)
{
    return (uint32_t *)(void *)recomp_memory(address, sizeof(uint32_t));
}

static void put32(uint32_t address, uint32_t value) { memcpy(recomp_memory(address, 4u), &value, 4u); }

static void test_screen(void)
{
    RecompGameLiveScreen screen;
    *recomp_memory(0x00878392u, 1u) = 8u; /* g_game_mode */
    *recomp_memory(0x00df5dd8u, 1u) = 12u;
    put32(0x00df5e08u, 0x00442190u);
    put32(0x00f099ecu, 2u);
    put32(0x00f09a44u, 4u);
    *recomp_memory(0x00f0a548u, 1u) = 0x01u;
    /* Outside the Live menus the zeroed stacks name page 0, but nothing is shown. */
    recomp_game_live_screen(&screen);
    CHECK(screen.live_page == -1);
    CHECK(screen.live_overlay == -1);
    /* The main stack two pages deep, the main menu then the session lobby on
       top, and the friend list over it. */
    put32(0x00f25208u, 2u);
    put32(0x00f2520cu, 21u);
    *recomp_memory(0x00f2529eu, 1u) = 1u;
    *recomp_memory(0x00f252a4u, 1u) = 1u;
    put32(0x00f25bc0u, 7u);
    *recomp_memory(0x00f25c5cu, 1u) = 1u;
    recomp_game_live_screen(&screen);
    CHECK(screen.game_mode == 8u);
    CHECK(screen.scene == 12u);
    CHECK(screen.scene_table == 0x00442190u);
    CHECK(screen.live_page == 21);
    CHECK(strcmp(recomp_game_live_page_name(screen.live_page), "session_lobby") == 0);
    CHECK(screen.live_overlay == 7);
    CHECK(screen.session_players == 2u);
    CHECK(screen.match_role == 4u);
    CHECK(screen.lockstep_flags == 0x01u);

    /* No page: the stack index is -1. */
    *recomp_memory(0x00f2529eu, 1u) = 0xffu;
    recomp_game_live_screen(&screen);
    CHECK(screen.live_page == -1);
    CHECK(strcmp(recomp_game_live_page_name(screen.live_page), "none") == 0);

    /* A page id past the table reads as none, not as a name past the end. */
    *recomp_memory(0x00f2529eu, 1u) = 0u;
    put32(0x00f25208u, 0x1cu);
    recomp_game_live_screen(&screen);
    CHECK(screen.live_page == -1);
    CHECK(strcmp(recomp_game_live_page_name(26), "ranking_resume") == 0);
    CHECK(strcmp(recomp_game_live_page_name(27), "none") == 0);
}

int main(void)
{
    recomp_fast_ram = calloc(1u, RECOMP_XBOX_RAM_SIZE);
    if (recomp_fast_ram == NULL) return 1;
    test_screen();
    free(recomp_fast_ram);
    if (failures != 0) {
        fprintf(stderr, "game_state_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("game_state_test: all checks passed\n");
    return 0;
}
