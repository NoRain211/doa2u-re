/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_state.h"

#include "input_pulse_source.h"
#include "runtime.h"

bool recomp_game_round_live(void)
{
    if (*recomp_memory(0x008a56d0u, 1u) != 5u || *recomp_memory(0x008783c9u, 1u) != 0u ||
        *recomp_memory(0x0087ae08u, 1u) == 0u)
        return false;
    for (uint32_t side = 0u; side < 2u; ++side) {
        const uint8_t *life = recomp_memory(0x0087ae54u + side * 0x94u, 2u);
        if ((int16_t)(life[0] | life[1] << 8) <= 0) return false;
    }
    return true;
}

bool recomp_game_walk_to_versus(uint32_t frame, bool select, RecompInputGamepad *pad)
{
    const uint8_t state = *recomp_memory(0x008a56d0u, 1u), scene = *recomp_memory(0x00df5dd8u, 1u),
                  target = *recomp_memory(0x00df5ddau, 1u), mode = *recomp_memory(0x00df57b4u, 1u);
    if (state == 4u && (select ? scene != 0u && scene != 2u : scene == 0u && mode == 5u)) return true;
    const RecompInputGamepad none = {0};
    *pad = none;
    if (frame % 16u != 0u || (state == 4u && target != 0u && target != 0xffu)) return false;
    if (state != 4u) pad->buttons = RECOMP_INPUT_BUTTON_START;
    else if (scene == 0u && mode != 5u) pad->buttons = 8u; /* d-pad right */
    else pad->analog_buttons[RECOMP_INPUT_ANALOG_A] = RECOMP_INPUT_ANALOG_PRESSED;
    return false;
}
