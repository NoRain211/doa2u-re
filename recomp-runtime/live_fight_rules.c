/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_fight_rules.h"

#include "runtime.h" /* RECOMP_XBOX_RAM_SIZE */

/* A virtual address is 10.x.y.z in network order. Reading it as a host number
   would order the octets backwards on a little-endian host, so compare the
   stored bytes as the number they spell. */
static uint32_t address_number(uint32_t ina)
{
    return (ina >> 24) | ((ina >> 8) & 0x0000ff00u) | ((ina << 8) & 0x00ff0000u) | (ina << 24);
}

unsigned recomp_live_player_index(uint32_t local_ina, uint32_t opponent_ina)
{
    return address_number(local_ina) < address_number(opponent_ina) ? 0u : 1u;
}

uint16_t recomp_live_pad_add_stop(uint16_t buttons)
{
    return (uint16_t)(buttons | RECOMP_LIVE_STOP_BUTTON);
}

uint16_t recomp_live_pad_mask_stop(uint16_t buttons)
{
    return (uint16_t)(buttons & (uint16_t)~RECOMP_LIVE_STOP_BUTTON);
}

bool recomp_live_pads_carry_stop(uint16_t local, uint16_t remote)
{
    return ((local | remote) & RECOMP_LIVE_STOP_BUTTON) != 0u;
}

uint32_t recomp_live_window(bool round_live, uint32_t frame)
{
    /* The fight holds still until lockstep batch 10 (game_mode_match_update),
       and a rematch saves meanwhile. */
    return round_live && frame >= 10u ? RECOMP_ROLLBACK_WINDOW : 0u;
}

size_t recomp_live_hash_ignored(RecompRamRange *ignored, size_t capacity)
{
    /* Each side's Live layer, profile, HUD and draw state differ by design, as
       do the cloth and stage effects keyed to frames since boot, so hash only
       the fight: the match and fighter block, the battle state and the game's
       Mersenne Twister (0x8a5768, 625 words). */
    static const RecompRamRange kept[] = {
        {0x00863a8cu, 0x0087d56cu}, /* up to g_game_mode_frame_counter, frames since boot */
        {0x0087d570u, 0x0087f53cu},
        {0x008a3968u, 0x008a396cu}, /* g_battle_state */
        {0x008a5768u, 0x008a612cu},
    };
    /* Inside the block, what each player's own widescreen and 5.1 choices
       shape, which the fight never reads back: camera_setup_view_cone's copy
       of the camera (16:9 frames it from elsewhere), its field of view and
       view cone, the D3D world, view and projection matrices and a
       view-projection one, and the music track.
       Also what the title and menu demo scenes leave behind (marked "demo"):
       their manager's state (0x87d578-0x87d642, 0x87f2b0-0x87f51c; the scene
       is picked at random) and the fight state their fights write and a Live
       fight's first frames do not: copies of fighter positions, vectors,
       matrix rows and the last scene's partner record and winner.
       Two sides with different menu histories differ there from frame 0; a
       fresh runner holds zeros in that fight state. Each run is a span where two runners'
       desync dumps differed; the bytes in between that the fight reads
       (0x877c94-0x877cb8, 0x87d498-0x87d49d, 0x87d5a4, 0x87d63c) stay in.
       Sorted, inside kept. */
    static const RecompRamRange own[] = {
        {0x00877c24u, 0x00877c28u}, {0x00877c6cu, 0x00877c70u}, /* g_view_half_sin_x */
        {0x00877c7au, 0x00877c94u}, /* demo */
        {0x00877c96u, 0x00877c97u}, {0x00877ca4u, 0x00877ca6u}, /* demo */
        {0x00877cb8u, 0x00877e98u}, /* demo */
        {0x00877e9cu, 0x00877ea0u}, /* g_view_half_tan_y */
        {0x00877ea4u, 0x00877ea8u}, {0x00877eb0u, 0x00877ef0u}, /* the world matrix */
        {0x00877ef4u, 0x00877ef8u}, {0x00878300u, 0x00878304u}, /* g_view_half_sin_y */
        {0x0087839cu, 0x008783a0u}, /* g_view_half_cos_y */
        {0x008783a4u, 0x008783a8u}, {0x008783e0u, 0x008783e1u}, {0x008783e8u, 0x008783ecu},
        /* demo: copies of each fighter's position that stage effects read */
        {0x008785f0u, 0x008786ecu}, {0x00878730u, 0x0087876cu}, {0x008787b0u, 0x008787ecu},
        {0x008789f0u, 0x00878aecu}, {0x00878b30u, 0x00878b6cu}, {0x00878bb0u, 0x00878becu},
        {0x008794f0u, 0x008795ecu}, {0x008796f0u, 0x008799f0u},
        {0x00879bf4u, 0x00879bfcu}, /* g_view_half_tan_x, the field of view */
        {0x00879c00u, 0x00879c04u}, {0x00879c10u, 0x00879c50u}, /* the view matrix */
        {0x0087a900u, 0x0087a940u}, /* a view-projection matrix */
        {0x0087ad6cu, 0x0087ad70u}, {0x0087ae1cu, 0x0087ae2cu},
        {0x0087af5cu, 0x0087af78u}, {0x0087af8au, 0x0087af8cu}, /* demo: fighter 2's record */
        {0x0087bfa0u, 0x0087c7a0u}, /* demo */
        {0x0087cfacu, 0x0087cfb0u},
        {0x0087d03cu, 0x0087d04cu}, {0x0087d068u, 0x0087d078u}, /* demo */
        {0x0087d128u, 0x0087d208u}, /* demo: matrix rows */
        {0x0087d214u, 0x0087d224u}, {0x0087d228u, 0x0087d22cu}, /* g_view_half_cos_x */
        {0x0087d23au, 0x0087d23bu}, /* demo: g_match_winner_side */
        {0x0087d320u, 0x0087d322u}, {0x0087d328u, 0x0087d32cu}, /* demo */
        {0x0087d334u, 0x0087d33eu}, {0x0087d344u, 0x0087d35au}, /* demo */
        {0x0087d360u, 0x0087d3a0u}, /* the projection matrix */
        {0x0087d3a4u, 0x0087d3a8u}, {0x0087d3b4u, 0x0087d3b5u}, /* demo */
        {0x0087d3f4u, 0x0087d3f8u},
        {0x0087d40bu, 0x0087d40cu}, {0x0087d413u, 0x0087d448u}, {0x0087d450u, 0x0087d480u}, /* demo */
        {0x0087d488u, 0x0087d48cu}, {0x0087d49eu, 0x0087d49fu}, /* demo */
        {0x0087d49fu, 0x0087d4a0u}, {0x0087d4e0u, 0x0087d550u}, /* demo */
        {0x0087d578u, 0x0087d5a2u}, {0x0087d5a9u, 0x0087d5dcu}, /* demo: the scene manager */
        {0x0087d618u, 0x0087d63au}, {0x0087d640u, 0x0087d642u}, /* demo */
        {0x0087f2b0u, 0x0087f442u}, {0x0087f4d8u, 0x0087f51cu}, /* demo: the scene manager */
        {0x0087f538u, 0x0087f539u}, /* the music track */
    };
    const size_t kept_count = sizeof kept / sizeof *kept, own_count = sizeof own / sizeof *own;
    if (ignored == NULL || capacity < kept_count + 1u + own_count) return 0u;
    size_t count = 0u, from = 0u, next_own = 0u;
    for (size_t i = 0u; i < kept_count; ++i) {
        if (kept[i].start > from) ignored[count++] = (RecompRamRange){from, kept[i].start};
        while (next_own < own_count && own[next_own].start < kept[i].stop) ignored[count++] = own[next_own++];
        from = kept[i].stop;
    }
    if (from < RECOMP_XBOX_RAM_SIZE) ignored[count++] = (RecompRamRange){from, RECOMP_XBOX_RAM_SIZE};
    return count;
}

const char *recomp_live_end_stop_id(RecompLiveEnd end)
{
    switch (end) {
    case RECOMP_LIVE_END_DESYNC: return "rollback:desync";
    case RECOMP_LIVE_END_TIMEOUT: return "rollback:peer-timeout";
    case RECOMP_LIVE_END_REWIND: return "rollback:unrestorable";
    case RECOMP_LIVE_END_PEER_LEFT: return "rollback:peer-left";
    }
    return "rollback:end";
}

bool recomp_live_end_session(bool live, RecompLiveEnd end, const RecompLiveEndOps *ops)
{
    if (!live) {
        /* Every other session keeps its own ending: the runner stops, as before. */
        if (ops != NULL && ops->stop_runner != NULL) ops->stop_runner(ops->context, recomp_live_end_stop_id(end));
        return false;
    }
    /* Live: the game's own Live messages and lockstep carry the fight from
       here, so nothing stops the runner. Tell the other player first, while the
       session's route is still bound, unless they are the one who left. */
    if (end != RECOMP_LIVE_END_PEER_LEFT && ops != NULL && ops->tell_opponent != NULL)
        ops->tell_opponent(ops->context);
    if (ops != NULL && ops->hand_back != NULL) ops->hand_back(ops->context, recomp_live_end_stop_id(end));
    return true;
}
