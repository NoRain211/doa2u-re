/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_decomp.h"

#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    RecompFunction (*lookup)(uint32_t address);
} groups[] = {
    {"render_xform", recomp_render_xform_lookup_manual},
    {"render_mesh", recomp_render_mesh_lookup_manual},
    {"render_state", recomp_render_state_lookup_manual},
    {"fighter_mesh", recomp_fighter_mesh_lookup_manual},
    {"fighter_pose", recomp_fighter_pose_lookup_manual},
    {"game_math", recomp_game_math_lookup_manual},
    {"render_draw", recomp_render_draw_lookup_manual},
    {"library_hot", recomp_library_hot_lookup_manual},
};

bool recomp_game_decomp_enabled(const char *list, const char *group)
{
    if (list == NULL || list[0] == '\0') return true;
    const size_t length = strlen(group);
    for (const char *at = list; (at = strstr(at, group)) != NULL; at += length) {
        if ((at == list || at[-1] == ',') && (at[length] == ',' || at[length] == '\0')) return true;
    }
    return false;
}

RecompFunction recomp_game_decomp_lookup_manual(uint32_t address)
{
    for (size_t i = 0u; i < sizeof groups / sizeof groups[0]; ++i) {
        const RecompFunction function = groups[i].lookup(address);
        if (function != NULL)
            return recomp_game_decomp_enabled(getenv("RECOMP_DECOMP"), groups[i].name) ? function : NULL;
    }
    return NULL;
}
