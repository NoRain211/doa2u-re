/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hitbox_view.h"
#include "d3d8_adapter.h"
#include "runtime.h"
#include "scene_freeze_adapter.h"

#include <math.h>
#include <string.h>

/* Sparring and Training are battle modes 9 and 10; combo_hud.c reads the same
   gate. Each fighter's bones are 32 matrices of 64 bytes from 0x00875C20,
   0x800 bytes apart, the hips first, their translation in floats 12 to 14;
   slots 0 and 1 are the two fighters in 1v1, 2 and 3 tag partners. Parent
   links are unknown, so joints are spheres and only the proven hips get a
   larger marker. */
enum {
    BATTLE_MODE = 0x0087cfa0u,
    FIGHTER_BONES = 0x00875c20u,
    FIGHTER_BONES_STRIDE = 0x800u,
    FIGHTER_BONES_COUNT = 32u,
    BONE_MATRIX_BYTES = 0x40u,
    BONE_TRANSLATION = 12u,
    FIGHTER_SLOTS = 2u,

    /* g_match_side_setup at 0x0087ae30, 0x94 bytes per side: root X/Y/Z at
       +0x04/+0x08/+0x0c, the rotation used for hold searches at +0x18, the
       character id at +0x01, the loaded character bank at +0x21, the move id at
       +0x32, the hit-record mirror flag at +0x36, state at +0x37, the facing
       flag at +0x3e and the replay branch at +0x40. */
    SIDE_SETUP = 0x0087ae30u,
    SIDE_STRIDE = 0x94u,
    CHARACTER_DATA_SECTIONS = 0x008a3cf8u,
    CHARACTER_SECTION_COUNT = 0x21u,
    CHARACTER_COLLISION_RECORDS = 0x006ab908u,
    BANK_INDEX_OVERRIDE = 0x00df3db8u,
    COLLISION_RECORD_BYTES = 0x10u,
    HIT_RECORD_BYTES = 0x18u,
    COLLISION_SEARCH_LIMIT = 4096u,
    GUEST_RAM_SIZE = 0x04000000u,

    MAX_VOLUMES = 256u,
    SPHERE_SLICES = 12u,
    SPHERE_STACKS = 8u,
    RING_SEGMENTS = 12u,
    FAN_SEGMENTS = 12u,
    HOLD_HALF_TURN = 0x1555u,
    MAX_VERTICES = 65536u,
    MAX_INDICES = 196608u,
};

static const float MAX_POINT = 1000.0f;
static const float JOINT_RADIUS = 0.035f;
static const float HIPS_RADIUS = 0.065f;
/* fighter_object_sector_search's 4368.6 / surface distance, and the largest
   sweep a flat fan can show. fighter_state_airborne clamps a collision
   record's radius to 2.5 and adds 0.3 as a search margin, but only under a
   session flag, so the overlay's unconditional cap is a display
   approximation. */
static const float HOLD_ANGLE_SCALE = 4368.6f;
static const float MIN_SURFACE_DISTANCE = 0.05f;
static const float MAX_HALF_ANGLE = 3.14159265358979323846f;
static const float BODY_RADIUS_LIMIT = 2.5f;
static const float TURN_RADIANS = 6.28318530717958647692f / 65536.0f;

/* ARGB, as the presenter's diffuse colours are, per colour class and side.
   The skeleton's two sides are P1 cyan and P2 yellow, the hips a paler, brighter
   pair so they read apart from the joints; attack and hurt stay one colour. */
static const uint32_t COLOURS[RECOMP_HITBOX_COLOURS][RECOMP_HITBOX_SIDES] = {
    /* joint */ {0xc040b8ffu, 0xc000ffffu, 0xc0ffff00u},
    /* hips */ {0xc050ff90u, 0xe080ffffu, 0xe0ffff80u},
    /* attack */ {0xc0ff4040u, 0xc0ff4040u, 0xc0ff4040u},
    /* hurt */ {0xc06080ffu, 0xc06080ffu, 0xc06080ffu},
};

/* X-ray's fill alpha byte, about 0.35 of an opaque volume. */
static const uint32_t XRAY_ALPHA = 0x59000000u;

typedef struct HitboxVertex {
    float x, y, z;
    uint32_t colour;
} HitboxVertex;

static RecompHitboxVolume volumes[MAX_VOLUMES];
static unsigned volume_count;
static RecompHitboxMode chosen;  /* PC Settings' saved choice */
static bool online;  /* Live's session lock, which keeps the choice */

static HitboxVertex triangles[MAX_VERTICES];
static uint16_t triangle_indices[MAX_INDICES];
static unsigned triangle_vertices, triangle_indices_used;
static HitboxVertex lines[MAX_VERTICES];
static uint16_t line_indices[MAX_INDICES];
static unsigned line_vertices, line_indices_used;

static bool sane_point(const float point[3])
{
    for (unsigned i = 0u; i < 3u; ++i)
        if (!isfinite(point[i]) || fabsf(point[i]) > MAX_POINT) return false;
    return true;
}

/* Guest tables hold pointers and sizes the game chose, so every read stays
   inside the 64 MiB of guest RAM. */
static bool guest_read(uint32_t from, void *to, size_t size)
{
    if (to == NULL || from >= GUEST_RAM_SIZE || size > GUEST_RAM_SIZE - from) return false;
    recomp_guest_load(to, from, size);
    return true;
}

static bool guest_u8(uint32_t from, uint8_t *to) { return guest_read(from, to, 1u); }
static bool guest_u16(uint32_t from, uint16_t *to) { return guest_read(from, to, 2u); }
static bool guest_u32(uint32_t from, uint32_t *to) { return guest_read(from, to, 4u); }

void recomp_hitbox_view_set_choice(unsigned choice)
{
    chosen = choice < (unsigned)RECOMP_HITBOX_MODES ? (RecompHitboxMode)choice : RECOMP_HITBOX_MODE_OFF;
}
void recomp_hitbox_view_set_online(bool locked) { online = locked; }
static bool shown(void) { return chosen != RECOMP_HITBOX_MODE_OFF && !online; }
static bool xray(void) { return chosen == RECOMP_HITBOX_MODE_XRAY; }

static void clear(void) { volume_count = 0u; }
unsigned recomp_hitbox_view_count(void) { return volume_count; }

bool recomp_hitbox_view_add(const RecompHitboxVolume *volume)
{
    if (volume == NULL || volume_count == MAX_VOLUMES ||
        (unsigned)volume->colour >= RECOMP_HITBOX_COLOURS ||
        (unsigned)volume->side >= RECOMP_HITBOX_SIDES ||
        (unsigned)volume->shape > RECOMP_HITBOX_RING || !sane_point(volume->a))
        return false;
    if (volume->shape != RECOMP_HITBOX_SPHERE && volume->shape != RECOMP_HITBOX_RING &&
        !sane_point(volume->b))
        return false;
    if (!isfinite(volume->radius) || volume->radius <= 0.0f || volume->radius > MAX_POINT)
        return false;
    if (volume->shape == RECOMP_HITBOX_FAN &&
        (!isfinite(volume->half_angle) || volume->half_angle <= 0.0f ||
         volume->half_angle > MAX_HALF_ANGLE ||
         (volume->b[0] == volume->a[0] && volume->b[2] == volume->a[2])))
        return false;
    volumes[volume_count++] = *volume;
    return true;
}

bool recomp_hitbox_hold_sector(const float root[2], int32_t other_yaw,
    bool mirrored, int32_t record_angle, float reach, float surface_distance,
    RecompHitboxHoldSector *out)
{
    if (root == NULL || out == NULL || !isfinite(root[0]) || !isfinite(root[1]) ||
        !isfinite(reach) || reach <= 0.0f || reach > MAX_POINT ||
        !isfinite(surface_distance))
        return false;
    if (surface_distance < MIN_SURFACE_DISTANCE) surface_distance = MIN_SURFACE_DISTANCE;
    const uint32_t angle = mirrored ? 0u - (uint32_t)record_angle : (uint32_t)record_angle;
    const uint32_t turn = ((uint32_t)other_yaw + 0x4000u + angle) & 0xffffu;
    const float direction = (float)turn * TURN_RADIANS;
    float half = ((float)HOLD_HALF_TURN + HOLD_ANGLE_SCALE / surface_distance) * TURN_RADIANS;
    if (half > MAX_HALF_ANGLE) half = MAX_HALF_ANGLE;
    out->origin[0] = root[0];
    out->origin[1] = root[1];
    out->direction[0] = sinf(direction);
    out->direction[1] = cosf(direction);
    out->reach = reach;
    out->half_angle = half;
    return true;
}

static bool triangle_vertex(float x, float y, float z, uint32_t colour)
{
    if (triangle_vertices == MAX_VERTICES) return false;
    triangles[triangle_vertices++] = (HitboxVertex){x, y, z, colour};
    return true;
}

static void triangle_index(uint16_t index)
{
    if (triangle_indices_used < MAX_INDICES) triangle_indices[triangle_indices_used++] = index;
}

static bool line_vertex(float x, float y, float z, uint32_t colour)
{
    if (line_vertices == MAX_VERTICES) return false;
    lines[line_vertices++] = (HitboxVertex){x, y, z, colour};
    return true;
}

/* One lat-long sphere; a full one needs 117 vertices and 576 indices. */
static void mesh_sphere(float x, float y, float z, float radius, uint32_t colour)
{
    const unsigned rings = SPHERE_STACKS, slices = SPHERE_SLICES;
    if (triangle_vertices + (rings + 1u) * (slices + 1u) > MAX_VERTICES ||
        triangle_indices_used + rings * slices * 6u > MAX_INDICES) return;
    const uint16_t base = (uint16_t)triangle_vertices;
    for (unsigned ring = 0u; ring <= rings; ++ring) {
        const float theta = 3.14159265358979323846f * (float)ring / (float)rings;
        const float sin_theta = sinf(theta), cos_theta = cosf(theta);
        for (unsigned slice = 0u; slice <= slices; ++slice) {
            const float phi = 6.28318530717958647692f * (float)slice / (float)slices;
            triangle_vertex(x + radius * sin_theta * cosf(phi), y + radius * cos_theta,
                z + radius * sin_theta * sinf(phi), colour);
        }
    }
    for (unsigned ring = 0u; ring < rings; ++ring) {
        for (unsigned slice = 0u; slice < slices; ++slice) {
            const uint16_t first = (uint16_t)(base + ring * (slices + 1u) + slice);
            const uint16_t below = (uint16_t)(first + slices + 1u);
            triangle_index(first);
            triangle_index(below);
            triangle_index((uint16_t)(first + 1u));
            triangle_index((uint16_t)(first + 1u));
            triangle_index(below);
            triangle_index((uint16_t)(below + 1u));
        }
    }
}

/* A 12-segment circle around one axis: 0 is XY, 1 XZ and 2 YZ. */
static void mesh_ring(float x, float y, float z, float radius, unsigned plane, uint32_t colour)
{
    if (line_vertices + 2u * RING_SEGMENTS > MAX_VERTICES ||
        line_indices_used + 2u * RING_SEGMENTS > MAX_INDICES) return;
    for (unsigned step = 0u; step < RING_SEGMENTS; ++step) {
        for (unsigned end = 0u; end < 2u; ++end) {
            const float angle = 6.28318530717958647692f * (float)(step + end) / (float)RING_SEGMENTS;
            const float c = cosf(angle), s = sinf(angle);
            const float px = x + (plane == 2u ? 0.0f : radius * c);
            const float py = y + (plane == 0u ? radius * s : plane == 1u ? 0.0f : radius * c);
            const float pz = z + (plane == 0u ? 0.0f : radius * s);
            const uint16_t index = (uint16_t)line_vertices;
            line_vertex(px, py, pz, colour);
            if (line_indices_used < MAX_INDICES) line_indices[line_indices_used++] = index;
        }
    }
}

/* A flat XZ fan outline: the rim and the two straight edges, with the engine's
   angle growing as (sin, cos). No fill: the hold search is a flat area, not a
   volume. */
static void mesh_fan(const RecompHitboxVolume *volume, uint32_t outline)
{
    const float dx = volume->b[0] - volume->a[0], dz = volume->b[2] - volume->a[2];
    const float length = sqrtf(dx * dx + dz * dz);
    if (length <= 0.0f) return;
    if (line_vertices + 2u * (FAN_SEGMENTS + 2u) > MAX_VERTICES ||
        line_indices_used + 2u * (FAN_SEGMENTS + 2u) > MAX_INDICES) return;
    const float base = atan2f(dx, dz);
    const float step = 2.0f * volume->half_angle / (float)FAN_SEGMENTS;
    float rim_x[FAN_SEGMENTS + 1u], rim_z[FAN_SEGMENTS + 1u];
    for (unsigned i = 0u; i <= FAN_SEGMENTS; ++i) {
        const float angle = base - volume->half_angle + step * (float)i;
        rim_x[i] = volume->a[0] + volume->radius * sinf(angle);
        rim_z[i] = volume->a[2] + volume->radius * cosf(angle);
    }
    for (unsigned i = 0u; i <= FAN_SEGMENTS; ++i) {
        if (i > 0u) {
            const uint16_t first = (uint16_t)line_vertices;
            line_vertex(rim_x[i - 1u], volume->a[1], rim_z[i - 1u], outline);
            line_vertex(rim_x[i], volume->a[1], rim_z[i], outline);
            line_indices[line_indices_used++] = first;
            line_indices[line_indices_used++] = (uint16_t)(first + 1u);
        }
    }
    for (unsigned end = 0u; end < 2u; ++end) {
        const unsigned i = end == 0u ? 0u : FAN_SEGMENTS;
        const uint16_t first = (uint16_t)line_vertices;
        line_vertex(volume->a[0], volume->a[1], volume->a[2], outline);
        line_vertex(rim_x[i], volume->a[1], rim_z[i], outline);
        line_indices[line_indices_used++] = first;
        line_indices[line_indices_used++] = (uint16_t)(first + 1u);
    }
}

static void mesh_volume(const RecompHitboxVolume *volume)
{
    uint32_t fill = COLOURS[volume->colour][volume->side];
    uint32_t outline = (fill & 0x00ffffffu) | 0xe0000000u;
    if (xray()) {
        fill = (fill & 0x00ffffffu) | XRAY_ALPHA;
        outline = (outline & 0x00ffffffu) | XRAY_ALPHA;
    }
    switch (volume->shape) {
    case RECOMP_HITBOX_SPHERE:
        mesh_sphere(volume->a[0], volume->a[1], volume->a[2], volume->radius, fill);
        for (unsigned plane = 0u; plane < 3u; ++plane)
            mesh_ring(volume->a[0], volume->a[1], volume->a[2], volume->radius, plane, outline);
        break;
    case RECOMP_HITBOX_FAN:
        mesh_fan(volume, outline);
        break;
    case RECOMP_HITBOX_RING:
        mesh_ring(volume->a[0], volume->a[1], volume->a[2], volume->radius, 1u, fill);
        break;
    }
}

static void multiply(const float left[16], const float right[16], float result[16])
{
    for (unsigned row = 0u; row < 4u; ++row) {
        for (unsigned column = 0u; column < 4u; ++column) {
            float value = 0.0f;
            for (unsigned k = 0u; k < 4u; ++k) value += left[row * 4u + k] * right[k * 4u + column];
            result[row * 4u + column] = value;
        }
    }
}

static void submit_draw(const RecompD3dPresenterTarget *target, const RecompD3dOverlayCamera *camera,
    uint32_t primitive, uint32_t triangle_count, const HitboxVertex *vertices, unsigned vertex_count,
    const uint16_t *indices, unsigned index_count)
{
    if (index_count == 0u || vertex_count == 0u) return;
    RecompD3dPresenterCommand command;
    memset(&command, 0, sizeof command);
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    RecompD3dPresenterDrawCommand *draw = &command.data.draw;
    draw->fvf = 0x42u; /* XYZ | DIFFUSE */
    draw->primitive_type = primitive;
    draw->vertex_count = vertex_count;
    draw->index_count = index_count;
    draw->triangle_count = triangle_count;
    draw->vertex_stride = sizeof(HitboxVertex);
    draw->vertex_bytes = vertices;
    draw->index_bytes = indices;
    draw->has_transform = true;
    multiply(camera->view, camera->projection, draw->transform);
    memcpy(draw->viewport, camera->viewport, sizeof draw->viewport);
    /* X-ray shows the volumes through the scene; the frame's depth stays bound. */
    draw->depth.depth_test_enable = !xray();
    draw->depth.depth_write_enable = false;
    draw->depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw->depth.alpha_func = RECOMP_D3D_COMPARE_ALWAYS;
    draw->blend.blend_enable = true;
    draw->blend.color_write_mask = 7u;
    draw->blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
    draw->blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
    draw->blend.op = RECOMP_D3D_BLEND_OP_ADD;
    draw->cull_mode = RECOMP_D3D_CULL_NONE;
    draw->line_width = 1.0f;
    draw->target = *target;
    /* The Swap target resolves without depth. The presenter's main depth is
       the frame's; an offscreen or empty target falls back to no test. */
    if (target->offscreen || target->color.data == 0u) {
        draw->target.no_depth = true;
    } else {
        draw->target.no_depth = false;
        draw->target.custom_depth = false;
    }
    recomp_d3d8_submit(&command);
}

void recomp_hitbox_view_submit(const RecompD3dPresenterTarget *target)
{
    if (target == NULL || volume_count == 0u) return;
    RecompD3dOverlayCamera camera;
    if (!recomp_d3d8_overlay_camera(&camera)) return;
    for (unsigned colour = 0u; colour < RECOMP_HITBOX_COLOURS; ++colour) {
        triangle_vertices = triangle_indices_used = 0u;
        line_vertices = line_indices_used = 0u;
        for (unsigned i = 0u; i < volume_count; ++i)
            if (volumes[i].colour == colour) mesh_volume(&volumes[i]);
        submit_draw(target, &camera, 5u, triangle_indices_used / 3u,
            triangles, triangle_vertices, triangle_indices, triangle_indices_used);
        submit_draw(target, &camera, 2u, line_indices_used / 2u,
            lines, line_vertices, line_indices, line_indices_used);
    }
}

/* fighter_current_hit_record (0x109620): state 10 uses section 6 and any other
   state section 7; the record index is the byte at section 0 - 4 + move * 8,
   and each record is 0x18 bytes. The +0x40 branch reads the bank from the
   mirrored side instead, as the game does for replays. */
static bool hit_record(uint32_t side, uint32_t *record)
{
    const uint32_t base = SIDE_SETUP + side * SIDE_STRIDE;
    uint8_t mirror, state, replay, index;
    if (!guest_u8(base + 0x36u, &mirror) || !guest_u8(base + 0x37u, &state) ||
        !guest_u8(base + 0x40u, &replay) || !guest_u8(base + 0x21u, &index))
        return false;
    if (replay == 1u) {
        const uint32_t other = mirror == 1u ? side ^ 1u : side;
        if (!guest_u8(SIDE_SETUP + other * SIDE_STRIDE + 0x21u, &index)) return false;
    } else if (mirror == 1u) {
        if (!guest_u8(BANK_INDEX_OVERRIDE + side, &index)) return false;
    }
    const uint32_t sections = CHARACTER_DATA_SECTIONS + (uint32_t)index * CHARACTER_SECTION_COUNT * 4u;
    const unsigned section = state == 0x0au ? 6u : 7u;
    uint32_t records, entries;
    if (!guest_u32(sections + section * 4u, &records) ||
        !guest_u32(sections, &entries))
        return false;
    if (records >= GUEST_RAM_SIZE || entries < 4u || entries >= GUEST_RAM_SIZE) return false;
    uint16_t move;
    if (!guest_u16(base + 0x32u, &move)) return false;
    uint8_t entry;
    if (!guest_u8(entries - 4u + (uint32_t)move * 8u, &entry)) return false;
    const uint32_t at = records + (uint32_t)entry * HIT_RECORD_BYTES;
    if (at >= GUEST_RAM_SIZE || HIT_RECORD_BYTES > GUEST_RAM_SIZE - at) return false;
    *record = at;
    return true;
}

/* fighter_move_collision_record (0x10b0e0): states 2, 3 and 14 look up the
   character's collision table at 0x6ab908, then walk 16-byte records keyed by
   the side's move id at +0x32 until 0xffff. */
static bool collision_record(uint32_t side, uint32_t *record)
{
    const uint32_t base = SIDE_SETUP + side * SIDE_STRIDE;
    uint8_t state, character;
    if (!guest_u8(base + 0x37u, &state) || (state != 2u && state != 3u && state != 0x0eu))
        return false;
    if (!guest_u8(base + 0x01u, &character)) return false;
    uint32_t at;
    if (!guest_u32(CHARACTER_COLLISION_RECORDS + (uint32_t)character * 4u, &at) || at == 0u)
        return false;
    uint16_t move;
    if (!guest_u16(base + 0x32u, &move)) return false;
    for (unsigned step = 0u; step < COLLISION_SEARCH_LIMIT; ++step) {
        if (at >= GUEST_RAM_SIZE || COLLISION_RECORD_BYTES > GUEST_RAM_SIZE - at) return false;
        uint16_t key;
        if (!guest_u16(at, &key)) return false;
        if (key == 0xffffu) return false;
        if (key == move) {
            *record = at;
            return true;
        }
        at += COLLISION_RECORD_BYTES;
    }
    return false;
}

/* The stage-collision body radius, clamped as fighter_state_airborne does;
   mode 0 always collides and mode 1 only in a listed state, so the overlay's
   unconditional mode <= 1 check is a display approximation. */
static bool body_radius(uint32_t side, float *radius)
{
    uint32_t record;
    uint8_t mode;
    if (!collision_record(side, &record) || !guest_u8(record + 0x0du, &mode) || mode > 1u)
        return false;
    if (!guest_read(record + 0x04u, radius, sizeof *radius) ||
        !isfinite(*radius) || *radius <= 0.0f)
        return false;
    if (*radius > BODY_RADIUS_LIMIT) *radius = BODY_RADIUS_LIMIT;
    return true;
}

static bool side_root(uint32_t side, float root[3])
{
    return guest_read(SIDE_SETUP + side * SIDE_STRIDE + 0x04u, root, 3u * sizeof *root) &&
        sane_point(root) && !(root[0] == 0.0f && root[1] == 0.0f && root[2] == 0.0f);
}

static bool side_yaw(uint32_t side, int32_t *yaw)
{
    uint32_t raw;
    if (!guest_u32(SIDE_SETUP + side * SIDE_STRIDE + 0x18u, &raw)) return false;
    memcpy(yaw, &raw, sizeof raw);
    return true;
}

/* A fan per side whose current hit record still asks for a hold search. */
static void add_hold_sectors(void)
{
    float roots[FIGHTER_SLOTS][3];
    bool present[FIGHTER_SLOTS];
    for (unsigned side = 0u; side < FIGHTER_SLOTS; ++side)
        present[side] = side_root(side, roots[side]);

    for (unsigned side = 0u; side < FIGHTER_SLOTS; ++side) {
        if (!present[side]) continue;
        uint32_t record, raw_angle;
        uint8_t gate, mirrored;
        float reach;
        int32_t other_yaw, record_angle;
        if (!hit_record(side, &record) || !guest_u8(record + 0x0du, &gate) || gate == 0u)
            continue;
        if (!guest_read(record + 0x10u, &reach, sizeof reach) ||
            !isfinite(reach) || reach <= 0.0f || reach > MAX_POINT)
            continue;
        if (!guest_u32(record + 0x14u, &raw_angle) || !side_yaw(side ^ 1u, &other_yaw) ||
            !guest_u8(SIDE_SETUP + side * SIDE_STRIDE + 0x3eu, &mirrored))
            continue;
        memcpy(&record_angle, &raw_angle, sizeof record_angle);

        float surface = reach;
        if (present[side ^ 1u]) {
            const float dx = roots[side ^ 1u][0] - roots[side][0];
            const float dz = roots[side ^ 1u][2] - roots[side][2];
            surface = sqrtf(dx * dx + dz * dz);
            float radius;
            if (body_radius(side ^ 1u, &radius)) surface -= radius;
        }
        const float root_xz[2] = {roots[side][0], roots[side][2]};
        /* fighter_hold_target_search shifts the origin sideways by the
           constants at 0x440f48/0x440f4c when the opponent is character 9 on
           move 0x175; the overlay does not model that. */
        RecompHitboxHoldSector sector;
        if (!recomp_hitbox_hold_sector(root_xz, other_yaw, mirrored == 1u,
                record_angle, reach, surface, &sector))
            continue;
        const RecompHitboxVolume volume = {
            .shape = RECOMP_HITBOX_FAN,
            .colour = RECOMP_HITBOX_ATTACK,
            .a = {sector.origin[0], roots[side][1], sector.origin[1]},
            .b = {sector.origin[0] + sector.direction[0], roots[side][1],
                sector.origin[1] + sector.direction[1]},
            .radius = sector.reach,
            .side = RECOMP_HITBOX_SIDE_ANY,
            .half_angle = sector.half_angle,
        };
        recomp_hitbox_view_add(&volume);
    }
}

/* The flat stage-collision ring at each fighter's root. */
static void add_body_radii(void)
{
    for (unsigned side = 0u; side < FIGHTER_SLOTS; ++side) {
        float root[3], radius;
        if (!side_root(side, root) || !body_radius(side, &radius)) continue;
        const RecompHitboxVolume ring = {
            .shape = RECOMP_HITBOX_RING,
            .colour = RECOMP_HITBOX_HURT,
            .a = {root[0], root[1], root[2]},
            .b = {root[0], root[1], root[2]},
            .radius = radius,
            .side = RECOMP_HITBOX_SIDE_ANY,
        };
        recomp_hitbox_view_add(&ring);
    }
}

static void add_skeletons(void)
{
    float matrix[BONE_MATRIX_BYTES / sizeof(float)];
    for (unsigned slot = 0u; slot < FIGHTER_SLOTS; ++slot) {
        for (unsigned bone = 0u; bone < FIGHTER_BONES_COUNT; ++bone) {
            recomp_guest_load(matrix,
                FIGHTER_BONES + slot * FIGHTER_BONES_STRIDE + bone * BONE_MATRIX_BYTES, sizeof matrix);
            const float *at = matrix + BONE_TRANSLATION;
            /* Unused slots hold zeros; an absurd or non-finite pose is skipped. */
            if (!sane_point(at) || (at[0] == 0.0f && at[1] == 0.0f && at[2] == 0.0f)) continue;
            const RecompHitboxVolume joint = {
                .shape = RECOMP_HITBOX_SPHERE,
                .colour = bone == 0u ? RECOMP_HITBOX_HIPS : RECOMP_HITBOX_JOINT,
                .a = {at[0], at[1], at[2]},
                .b = {at[0], at[1], at[2]},
                .radius = bone == 0u ? HIPS_RADIUS : JOINT_RADIUS,
                .side = slot == 0u ? RECOMP_HITBOX_SIDE_ONE : RECOMP_HITBOX_SIDE_TWO,
            };
            recomp_hitbox_view_add(&joint);
        }
    }
}

void recomp_hitbox_view_overlay(const RecompD3dPresenterTarget *target)
{
    const uint8_t mode = *recomp_memory(BATTLE_MODE, 1u);
    if (!shown() || recomp_scene_freeze_overlay_hidden() || (mode != 9u && mode != 10u)) {
        clear();
        return;
    }
    add_skeletons();
    add_hold_sectors();
    add_body_radii();
    recomp_hitbox_view_submit(target);
    clear();
}
