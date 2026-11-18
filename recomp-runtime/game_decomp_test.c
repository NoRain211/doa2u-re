/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_decomp.h"

#include <stdio.h>

/* The groups bind nothing here; this checks the RECOMP_DECOMP list. */
RecompFunction recomp_render_xform_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_render_mesh_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_render_state_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_fighter_mesh_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_fighter_pose_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_game_math_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_render_draw_lookup_manual(uint32_t address) { (void)address; return NULL; }
RecompFunction recomp_library_hot_lookup_manual(uint32_t address) { (void)address; return NULL; }

int main(void)
{
    static const struct { const char *list, *group; bool expected; } cases[] = {
        {NULL, "game_math", true},
        {"", "game_math", true},
        {"none", "game_math", false},
        {"game_math", "game_math", true},
        {"render_xform,game_math", "game_math", true},
        {"game_math,render_xform", "render_xform", true},
        {"game_mathx", "game_math", false},
        {"xgame_math", "game_math", false},
        {"render_mesh", "render_mesh_extra", false},
    };
    int failed = 0;
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        if (recomp_game_decomp_enabled(cases[i].list, cases[i].group) != cases[i].expected) {
            fprintf(stderr, "game_decomp: list '%s' group '%s'\n",
                cases[i].list != NULL ? cases[i].list : "(unset)", cases[i].group);
            failed = 1;
        }
    }
    return failed;
}
