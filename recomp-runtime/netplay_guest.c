/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_guest.h"
#include "kernel_abi.h"

#include <string.h>

static char fight_names[2][16];
static uint32_t fight_local_side;
static bool fight_names_set;
static uint16_t side_profiles[2][16]; /* UTF-16, as the game names profiles */
static bool side_profiles_set;
static uint8_t profile_slot[0x12140u];

extern void sub_000244F0(void);

void recomp_netplay_guest_set_fight_names(const char *side0, const char *side1, uint32_t local_side)
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

void recomp_netplay_guest_set_side_profiles(const uint16_t *side0, const uint16_t *side1)
{
    const uint16_t *names[2] = {side0, side1};
    side_profiles_set = false;
    for (unsigned side = 0u; side < 2u; ++side) {
        unsigned length = 0u;
        for (; names[side] != NULL && names[side][length] != 0u && length + 1u < 16u; ++length)
            side_profiles[side][length] = names[side][length];
        if (length != 0u && (side_profiles[side][length - 1u] & 0xfc00u) == 0xd800u) --length; /* a split pair */
        side_profiles[side][length] = 0u;
        side_profiles_set |= length != 0u;
    }
}

static void clear_main_profile(void)
{
    /* 0x244f0 profile_main_clear points the current profile data (*0x87cfa8)
       at defaults. Netplay boots no profile, so every port played the default
       controller settings (20 bytes per port at +0x1e: free buttons, type,
       vibration, analog command). Read each side's own profile into side
       slot 1 with profile_read_into_struct (0x24010, cdecl name, profile, 0,
       0; EAX 0 on success) and give its controller 1 settings to that side's
       port. Both peers read the same files, so their guests stay alike. */
    sub_000244F0();
    const uint32_t slot = 0x007cb660u + sizeof profile_slot; /* g_profile_slots[1] */
    uint8_t layouts[2][0x14];
    bool found[2] = {false, false};
    memcpy(profile_slot, recomp_memory(slot, sizeof profile_slot), sizeof profile_slot);
    for (unsigned side = 0u; side < 2u; ++side) {
        if (side_profiles[side][0] == 0u) continue;
        const uint32_t name = slot + 0x12118u; /* the slot's own name field */
        memcpy(recomp_memory(name, sizeof side_profiles[side]), side_profiles[side], sizeof side_profiles[side]);
        const uint32_t args[4] = {name, slot, 0u, 0u};
        kernel_call_guest(0x00024010u, args, 4u);
        found[side] = recomp_runtime.registers.eax == 0u;
        if (found[side]) memcpy(layouts[side], recomp_memory(slot + 0xf200u + 0x1eu, 0x14u), 0x14u);
    }
    memcpy(recomp_memory(slot, sizeof profile_slot), profile_slot, sizeof profile_slot);
    const uint32_t data = *recomp_memory_u32(0x0087cfa8u);
    for (unsigned side = 0u; side < 2u; ++side)
        if (found[side]) memcpy(recomp_memory(data + 0x1eu + side * 0x14u, 0x14u), layouts[side], 0x14u);
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

RecompFunction recomp_netplay_guest_lookup_manual(uint32_t guest_address)
{
    if (fight_names_set && guest_address == 0x0012b460u) return draw_fight_name;
    if (side_profiles_set && guest_address == 0x000244f0u) return clear_main_profile;
    return NULL;
}
