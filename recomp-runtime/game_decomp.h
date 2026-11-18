#ifndef RECOMP_GAME_DECOMP_H
#define RECOMP_GAME_DECOMP_H

#include "runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hand-decompiled game functions that replace their lifted bodies, bound
   through recomp_lookup_manual. RECOMP_DECOMP chooses the groups that bind:
   unset binds all, "none" none, else a comma list such as "game_math", so
   each group can be checked against the lifted code alone. */
RecompFunction recomp_game_decomp_lookup_manual(uint32_t address);
bool recomp_game_decomp_enabled(const char *list, const char *group);

RecompFunction recomp_render_xform_lookup_manual(uint32_t address);
RecompFunction recomp_render_mesh_lookup_manual(uint32_t address);
RecompFunction recomp_render_state_lookup_manual(uint32_t address);
RecompFunction recomp_fighter_mesh_lookup_manual(uint32_t address);
RecompFunction recomp_fighter_pose_lookup_manual(uint32_t address);
RecompFunction recomp_game_math_lookup_manual(uint32_t address);
RecompFunction recomp_render_draw_lookup_manual(uint32_t address);
RecompFunction recomp_library_hot_lookup_manual(uint32_t address);

#ifdef __cplusplus
}
#endif

#endif
