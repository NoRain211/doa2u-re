/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hitbox_view.h"
#include "d3d8_adapter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest memory in 64 KiB pages, made on first touch. */
RecompRuntime recomp_runtime;
uint8_t *recomp_fast_ram;
static uint8_t *pages[0x4000000u >> 16];
uint8_t *recomp_memory(uint32_t address, size_t width)
{
    if (address >= 0x4000000u || width > 0x10000u || ((address + width - 1u) >> 16) != (address >> 16)) {
        fprintf(stderr, "bad access %08x+%zu\n", address, width);
        abort();
    }
    uint8_t **page = &pages[address >> 16];
    if (*page == NULL) *page = calloc(1u, 0x10000u);
    return *page + (address & 0xffffu);
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)(void *)recomp_memory(address, 4u); }
void recomp_guest_load(void *to, uint32_t from, size_t size) { memcpy(to, recomp_memory(from, size), size); }
static void store(uint32_t at, const void *from, size_t size) { memcpy(recomp_memory(at, size), from, size); }
static void put8(uint32_t at, uint8_t value) { store(at, &value, sizeof value); }
static void put16(uint32_t at, uint16_t value) { store(at, &value, sizeof value); }
static void put32(uint32_t at, uint32_t value) { store(at, &value, sizeof value); }
static void putf(uint32_t at, float value) { store(at, &value, sizeof value); }

enum {
    BATTLE_MODE = 0x0087cfa0u,
    FIGHTER_BONES = 0x00875c20u,
    FIGHTER_BONES_STRIDE = 0x800u,
    BONE_MATRIX_BYTES = 0x40u,
    GUEST_PAGE = 0x00870000u,
    GUEST_PAGE_SIZE = 0x10000u,
    SIDE_SETUP = 0x0087ae30u,
    SIDE_STRIDE = 0x94u,
    CHARACTER_DATA_SECTIONS = 0x008a3cf8u,
    CHARACTER_COLLISION_RECORDS = 0x006ab908u,
    BANK_INDEX_OVERRIDE = 0x00df3db8u, /* g_side_bank_index_override */
    SCRATCH = 0x00900000u,
    MAX_COMMANDS = 32u,
};

static bool camera_valid;
static RecompD3dOverlayCamera camera;
static bool overlay_hidden;
static RecompD3dPresenterCommand commands[MAX_COMMANDS];
static unsigned submit_count;

bool recomp_d3d8_overlay_camera(RecompD3dOverlayCamera *out)
{
    if (!camera_valid) return false;
    *out = camera;
    return true;
}
void recomp_d3d8_overlay_camera_reset(void) {}
bool recomp_scene_freeze_overlay_hidden(void) { return overlay_hidden; }
void recomp_d3d8_submit(const RecompD3dPresenterCommand *command)
{
    if (submit_count < MAX_COMMANDS) commands[submit_count] = *command;
    ++submit_count;
}

static int fails;
static void expect(const char *what, int ok) { if (!ok) { fprintf(stderr, "FAIL hitbox view: %s\n", what); ++fails; } }

static void set_camera(void)
{
    memset(&camera, 0, sizeof camera);
    camera.view[0] = camera.view[5] = camera.view[10] = camera.view[15] = 1.0f;
    camera.view[12] = 4.0f;
    camera.view[13] = -5.0f;
    camera.view[14] = 6.0f;
    camera.projection[0] = 2.0f;
    camera.projection[5] = -3.0f;
    camera.projection[10] = 0.5f;
    camera.projection[15] = 1.0f;
    camera.viewport[0] = 12.0f;
    camera.viewport[1] = 8.0f;
    camera.viewport[2] = 640.0f;
    camera.viewport[3] = 480.0f;
    camera.viewport[4] = 0.0f;
    camera.viewport[5] = 1.0f;
    camera_valid = true;
}

static void set_bone(uint32_t slot, uint32_t bone, float x, float y, float z)
{
    float matrix[16] = {0};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    store(FIGHTER_BONES + slot * FIGHTER_BONES_STRIDE + bone * BONE_MATRIX_BYTES, matrix, sizeof matrix);
}

static void set_mode(uint8_t mode) { *recomp_memory(BATTLE_MODE, 1u) = mode; }

/* The overlay empties the list after drawing, so a mode it skips empties it. */
static void clear_list(void)
{
    const uint8_t mode = *recomp_memory(BATTLE_MODE, 1u);
    set_mode(8u);
    recomp_hitbox_view_overlay(NULL);
    set_mode(mode);
}

static void setup_fighters(void)
{
    memset(recomp_memory(GUEST_PAGE, GUEST_PAGE_SIZE), 0, GUEST_PAGE_SIZE);
    set_bone(0u, 0u, 0.0f, 0.9f, 0.0f);
    set_bone(0u, 1u, 0.1f, 1.1f, 0.1f);
    set_bone(1u, 0u, 0.0f, 0.9f, 2.0f);
    set_bone(1u, 1u, -0.1f, 1.1f, 2.1f);
    set_mode(9u);
}

/* view x projection with row vectors, the presenter's mul(pos, wvp) layout:
   rows 2 and -3, 0.5 scale, and the translation in row 3. */
static const float EXPECTED_TRANSFORM[16] = {
    2.0f, 0.0f, 0.0f, 0.0f,
    0.0f, -3.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.5f, 0.0f,
    8.0f, 15.0f, 3.0f, 1.0f,
};

typedef struct HitboxVertexDummy { float x, y, z; uint32_t colour; } HitboxVertexDummy;

static bool command_draws(const RecompD3dPresenterCommand *command)
{
    const RecompD3dPresenterDrawCommand *draw = &command->data.draw;
    return command->type == RECOMP_D3D_PRESENTER_COMMAND_DRAW && draw->fvf == 0x42u &&
        draw->has_transform && draw->vertex_stride == sizeof(HitboxVertexDummy) &&
        draw->vertex_count > 0u && draw->index_count > 0u &&
        draw->depth.depth_test_enable && !draw->depth.depth_write_enable &&
        draw->depth.depth_func == RECOMP_D3D_COMPARE_LESS_EQUAL &&
        draw->blend.blend_enable && draw->blend.src_factor == RECOMP_D3D_BLEND_SRC_ALPHA &&
        draw->blend.dst_factor == RECOMP_D3D_BLEND_INV_SRC_ALPHA &&
        draw->blend.op == RECOMP_D3D_BLEND_OP_ADD && draw->cull_mode == RECOMP_D3D_CULL_NONE &&
        !draw->target.no_depth && !draw->target.custom_depth &&
        memcmp(draw->viewport, camera.viewport, sizeof draw->viewport) == 0;
}

static bool all_vertices_finite(const RecompD3dPresenterDrawCommand *draw)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        if (!isfinite(vertices[i].x) || !isfinite(vertices[i].y) || !isfinite(vertices[i].z)) return false;
    return true;
}

static bool vertices_have(const RecompD3dPresenterDrawCommand *draw, uint32_t colour)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        if (vertices[i].colour == colour) return true;
    return false;
}

/* Every vertex's alpha byte, for the X-ray and brighter-hips checks. */
static bool vertices_alpha(const RecompD3dPresenterDrawCommand *draw, uint32_t alpha)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        if ((vertices[i].colour >> 24) != alpha) return false;
    return true;
}

static bool vertices_have_point(const RecompD3dPresenterDrawCommand *draw,
    float x, float y, float z)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        if (vertices[i].x == x && vertices[i].y == y && vertices[i].z == z) return true;
    return false;
}

static bool vertices_have_point_near(const RecompD3dPresenterDrawCommand *draw,
    float x, float y, float z, float tolerance)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        if (fabsf(vertices[i].x - x) <= tolerance && fabsf(vertices[i].y - y) <= tolerance &&
            fabsf(vertices[i].z - z) <= tolerance) return true;
    return false;
}

/* A flat fan's two straight edges end at origin + radius * (sin, cos) of its
   base angle plus or minus the half sweep, so both ends pin its direction. */
static bool fan_faces(const RecompD3dPresenterDrawCommand *draw, const float origin[3],
    float radius, float half_angle, float direction)
{
    const float low = direction - half_angle, high = direction + half_angle;
    return vertices_have_point_near(draw, origin[0] + radius * sinf(low), origin[1],
               origin[2] + radius * cosf(low), 0.001f) &&
        vertices_have_point_near(draw, origin[0] + radius * sinf(high), origin[1],
               origin[2] + radius * cosf(high), 0.001f);
}

static bool vertices_at_height(const RecompD3dPresenterDrawCommand *draw, float y)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i)
        if (vertices[i].y != y) return false;
    return true;
}

static float max_xz_radius(const RecompD3dPresenterDrawCommand *draw, float x, float z)
{
    const HitboxVertexDummy *vertices = draw->vertex_bytes;
    float best = 0.0f;
    for (uint32_t i = 0u; i < draw->vertex_count; ++i) {
        const float dx = vertices[i].x - x, dz = vertices[i].z - z;
        const float distance = sqrtf(dx * dx + dz * dz);
        if (distance > best) best = distance;
    }
    return best;
}

/* The side record, character section and collision table the overlay reads.
   Section 0's index byte sits four bytes before its table; section 7's first
   record is the hit record. P1 reads P2's +0x18 yaw and its own +0x3e flag, so
   both sides' values differ from the ones the correct read must use. */
static void setup_hold_record(void)
{
    const uint32_t section0 = SCRATCH + 0x08u, section7 = SCRATCH + 0x100u;
    put16(SIDE_SETUP + 0x32u, 0u);
    put8(SIDE_SETUP + 0x36u, 0u);
    put8(SIDE_SETUP + 0x37u, 0u);
    put8(SIDE_SETUP + 0x40u, 0u);
    put8(SIDE_SETUP + 0x21u, 0u);
    put32(SIDE_SETUP + 0x18u, 0x2000u);           /* P1's own yaw, unused */
    put8(SIDE_SETUP + 0x3eu, 0u);                 /* P1's own facing flag */
    put8(SIDE_SETUP + SIDE_STRIDE + 0x3eu, 1u);   /* P2's, which P1 must not read */
    put8(SIDE_SETUP + SIDE_STRIDE + 0x21u, 0u);
    put32(SIDE_SETUP + SIDE_STRIDE + 0x18u, 0u);  /* P2's yaw, which P1 reads */
    put32(CHARACTER_DATA_SECTIONS + (0u * 0x21u + 0u) * 4u, section0);
    put32(CHARACTER_DATA_SECTIONS + (0u * 0x21u + 7u) * 4u, section7);
    put8(section0 - 4u, 1u);
    const uint32_t record = section7 + 0x18u;
    put8(record + 0x0du, 1u);
    putf(record + 0x10u, 1.5f);
    put32(record + 0x14u, 0x2000u);
}

static void setup_body_record(void)
{
    const uint32_t record = SCRATCH + 0x200u;
    put8(SIDE_SETUP + 0x01u, 2u);
    put8(SIDE_SETUP + 0x37u, 3u);
    put16(SIDE_SETUP + 0x32u, 0u);
    put32(CHARACTER_COLLISION_RECORDS + 2u * 4u, record);
    put16(record, 0u);
    putf(record + 0x04u, 1.25f);
    put8(record + 0x0du, 0u);
}

int main(void)
{
    static uint8_t sentinel[GUEST_PAGE_SIZE];
    RecompD3dPresenterTarget target;
    memset(&target, 0, sizeof target);
    target.color.data = 0x00008000u;
    target.color.width = 640u;
    target.color.height = 480u;
    target.color.format_byte = 0x12u;
    target.no_depth = true;

    set_camera();
    setup_fighters();

    /* Off: the mode and a camera alone draw nothing. */
    recomp_hitbox_view_set_choice(RECOMP_HITBOX_MODE_OFF);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("off draws nothing", submit_count == 0u);

    /* On outside Sparring and Training: still nothing. */
    recomp_hitbox_view_set_choice(RECOMP_HITBOX_MODE_ON);
    set_mode(8u);
    recomp_hitbox_view_overlay(&target);
    expect("other modes draw nothing", submit_count == 0u);

    /* Sparring: the two fighters' joints and hips, depth-tested. */
    set_mode(9u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("sparring draws", submit_count == 4u);
    for (unsigned i = 0u; i < submit_count && i < MAX_COMMANDS; ++i) {
        expect("draw command", command_draws(&commands[i]));
        expect("transform copied", memcmp(commands[i].data.draw.transform, EXPECTED_TRANSFORM,
            sizeof EXPECTED_TRANSFORM) == 0);
        expect("finite vertices", all_vertices_finite(&commands[i].data.draw));
    }

    /* Training has the same gate. */
    set_mode(10u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("training draws", submit_count == 4u);

    /* Online locks the viewer off and offline brings the saved choice back. */
    recomp_hitbox_view_set_online(true);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("online draws nothing", submit_count == 0u);
    recomp_hitbox_view_set_online(false);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("offline restores the choice", submit_count == 4u);

    /* A frame without a captured scene camera is skipped. */
    camera_valid = false;
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("no camera draws nothing", submit_count == 0u);
    camera_valid = true;

    /* The freeze's HUD hide drops the overlay too. */
    overlay_hidden = true;
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("hidden overlay draws nothing", submit_count == 0u);
    overlay_hidden = false;

    /* A non-finite or absurd bone is skipped; the other joints still draw and
       every submitted vertex stays finite. */
    set_bone(0u, 1u, NAN, 1.1f, 0.1f);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("non-finite bone still draws the rest", submit_count == 4u);
    for (unsigned i = 0u; i < submit_count && i < MAX_COMMANDS; ++i)
        expect("non-finite bone stays finite", all_vertices_finite(&commands[i].data.draw));
    set_bone(0u, 1u, 5000.0f, 1.1f, 0.1f);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("absurd bone still draws the rest", submit_count == 4u);
    for (unsigned i = 0u; i < submit_count && i < MAX_COMMANDS; ++i)
        expect("absurd bone stays finite", all_vertices_finite(&commands[i].data.draw));
    setup_fighters();

    /* The overlay only reads guest memory. */
    memcpy(sentinel, recomp_memory(GUEST_PAGE, GUEST_PAGE_SIZE), sizeof sentinel);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("guest memory unchanged", memcmp(sentinel, recomp_memory(GUEST_PAGE, GUEST_PAGE_SIZE), sizeof sentinel) == 0);

    /* The volume list: spheres, flat fan outlines and rings, one class at a
       time. */
    clear_list();
    expect("empty list", recomp_hitbox_view_count() == 0u);
    const RecompHitboxVolume sphere = {
        .shape = RECOMP_HITBOX_SPHERE, .colour = RECOMP_HITBOX_JOINT,
        .a = {0.0f, 1.0f, 0.0f}, .radius = 0.2f,
    };
    const RecompHitboxVolume fan = {
        .shape = RECOMP_HITBOX_FAN, .colour = RECOMP_HITBOX_ATTACK,
        .a = {0.0f, 1.0f, 0.0f}, .b = {0.0f, 1.0f, 1.0f}, .radius = 1.0f,
        .half_angle = 0.5f,
    };
    const RecompHitboxVolume ring = {
        .shape = RECOMP_HITBOX_RING, .colour = RECOMP_HITBOX_HURT,
        .a = {0.0f, 1.0f, 0.0f}, .radius = 1.0f,
    };
    expect("add sphere", recomp_hitbox_view_add(&sphere));
    expect("add fan", recomp_hitbox_view_add(&fan));
    expect("add ring", recomp_hitbox_view_add(&ring));
    expect("list count", recomp_hitbox_view_count() == 3u);
    RecompHitboxVolume bad = sphere;
    bad.radius = 0.0f;
    expect("reject empty sphere", !recomp_hitbox_view_add(&bad));
    bad = sphere;
    bad.a[0] = INFINITY;
    expect("reject non-finite point", !recomp_hitbox_view_add(&bad));
    bad = sphere;
    bad.colour = RECOMP_HITBOX_COLOURS;
    expect("reject colour", !recomp_hitbox_view_add(&bad));
    bad = sphere;
    bad.shape = (RecompHitboxShape)7;
    expect("reject shape", !recomp_hitbox_view_add(&bad));
    /* A fan needs a second finite end, a radius and a sweep; a ring ignores
       its end. */
    bad = fan;
    bad.b[1] = NAN;
    expect("reject non-finite fan", !recomp_hitbox_view_add(&bad));
    bad = fan;
    bad.radius = 0.0f;
    expect("reject empty fan", !recomp_hitbox_view_add(&bad));
    bad = fan;
    bad.half_angle = 0.0f;
    expect("reject fan sweep", !recomp_hitbox_view_add(&bad));
    bad = fan;
    bad.b[2] = bad.a[2];
    expect("reject fan direction", !recomp_hitbox_view_add(&bad));
    expect("list count after rejects", recomp_hitbox_view_count() == 3u);

    submit_count = 0u;
    recomp_hitbox_view_submit(&target);
    expect("three classes draw", submit_count == 4u); /* joint 2, fan 1, ring 1 */
    bool saw_triangles = false, saw_lines = false;
    for (unsigned i = 0u; i < submit_count && i < MAX_COMMANDS; ++i) {
        expect("list command", command_draws(&commands[i]));
        saw_triangles = saw_triangles || commands[i].data.draw.primitive_type == 5u;
        saw_lines = saw_lines || commands[i].data.draw.primitive_type == 2u;
    }
    expect("both primitive kinds", saw_triangles && saw_lines);
    camera_valid = false;
    submit_count = 0u;
    recomp_hitbox_view_submit(&target);
    expect("submit needs a camera", submit_count == 0u);
    /* The list holds at most MAX_VOLUMES. */
    camera_valid = true;
    clear_list();
    for (unsigned i = 0u; i < 256u; ++i)
        if (!recomp_hitbox_view_add(&sphere)) { expect("cap adds", 0); break; }
    expect("cap count", recomp_hitbox_view_count() == 256u);
    expect("cap rejects", !recomp_hitbox_view_add(&sphere));
    clear_list();
    /* An offscreen target has no frame depth: draw without a depth test. */
    expect("add for offscreen", recomp_hitbox_view_add(&sphere));
    submit_count = 0u;
    RecompD3dPresenterTarget offscreen = target;
    offscreen.offscreen = true;
    recomp_hitbox_view_submit(&offscreen);
    expect("offscreen draws without depth", submit_count == 2u &&
        commands[0].data.draw.target.no_depth && commands[1].data.draw.target.no_depth);
    clear_list();
    expect("cleared list", recomp_hitbox_view_count() == 0u);

    /* The skeleton's two sides are P1 cyan and P2 yellow; the hips are the same
       hues at more alpha. Submitted commands share the mesh arrays, whose
       content is the last colour class meshed, so this checks the hips class;
       the explicit volumes below check a class on its own. */
    recomp_hitbox_view_set_choice(RECOMP_HITBOX_MODE_ON);
    setup_fighters();
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("side colours draw", submit_count == 4u);
    expect("P1 and P2 hips", vertices_have(&commands[2].data.draw, 0xe080ffffu) &&
        vertices_have(&commands[2].data.draw, 0xe0ffff80u) &&
        vertices_alpha(&commands[2].data.draw, 0xe0u));

    /* A joint draws in its side's colour; no side keeps the class colour, and
       an unknown side is rejected. */
    RecompHitboxVolume joint = sphere;
    joint.colour = RECOMP_HITBOX_JOINT;
    RecompHitboxVolume bad_side = joint;
    bad_side.side = RECOMP_HITBOX_SIDES;
    expect("reject side", !recomp_hitbox_view_add(&bad_side));
    clear_list();
    joint.side = RECOMP_HITBOX_SIDE_ONE;
    expect("add P1 joint", recomp_hitbox_view_add(&joint));
    joint.side = RECOMP_HITBOX_SIDE_TWO;
    expect("add P2 joint", recomp_hitbox_view_add(&joint));
    joint.side = RECOMP_HITBOX_SIDE_ANY;
    expect("add neutral joint", recomp_hitbox_view_add(&joint));
    submit_count = 0u;
    recomp_hitbox_view_submit(&target);
    expect("side colours drawn", submit_count == 2u &&
        vertices_have(&commands[0].data.draw, 0xc000ffffu) &&
        vertices_have(&commands[0].data.draw, 0xc0ffff00u) &&
        vertices_have(&commands[0].data.draw, 0xc040b8ffu) &&
        vertices_alpha(&commands[0].data.draw, 0xc0u));

    /* The hips are the same two hues, brighter than the joints. */
    clear_list();
    RecompHitboxVolume hips = joint;
    hips.colour = RECOMP_HITBOX_HIPS;
    hips.side = RECOMP_HITBOX_SIDE_ONE;
    expect("add P1 hips", recomp_hitbox_view_add(&hips));
    hips.side = RECOMP_HITBOX_SIDE_TWO;
    expect("add P2 hips", recomp_hitbox_view_add(&hips));
    submit_count = 0u;
    recomp_hitbox_view_submit(&target);
    expect("hips drawn brighter", submit_count == 2u &&
        vertices_have(&commands[0].data.draw, 0xe080ffffu) &&
        vertices_have(&commands[0].data.draw, 0xe0ffff80u) &&
        vertices_alpha(&commands[0].data.draw, 0xe0u));

    /* X-ray: the same volumes through bodies and scenery, depth test off, at
       about 0.35 alpha; the frame's depth stays bound and writes nothing. */
    clear_list();
    recomp_hitbox_view_set_choice(RECOMP_HITBOX_MODE_XRAY);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("x-ray draws", submit_count == 4u);
    for (unsigned i = 0u; i < submit_count && i < MAX_COMMANDS; ++i) {
        expect("x-ray depth test off", !commands[i].data.draw.depth.depth_test_enable &&
            !commands[i].data.draw.depth.depth_write_enable);
        expect("x-ray keeps the target", !commands[i].data.draw.target.no_depth &&
            !commands[i].data.draw.target.custom_depth);
        expect("x-ray alpha", vertices_alpha(&commands[i].data.draw, 0x59u));
    }
    /* Online forces the viewer off and keeps the saved X-ray choice. */
    recomp_hitbox_view_set_online(true);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("x-ray online draws nothing", submit_count == 0u);
    recomp_hitbox_view_set_online(false);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("x-ray restored offline", submit_count == 4u &&
        !commands[0].data.draw.depth.depth_test_enable);
    /* Leaves the row On, as the earlier blocks leave it. */
    recomp_hitbox_view_set_choice(RECOMP_HITBOX_MODE_ON);

    /* The hold sector's maths: the root is the origin, the opponent's +0x18
       yaw plus 0x4000 (a quarter turn) plus the record's +0x14 angle is the
       facing, which the fighter's own +0x3e flag negates, and the engine's
       0x1555 + 4368.6 / d gives the half sweep. */
    const float hold_root[2] = {1.0f, 2.0f};
    RecompHitboxHoldSector sector;
    expect("hold geometry", recomp_hitbox_hold_sector(hold_root, 0, false, 0,
        2.0f, 2.0f, &sector));
    expect("hold origin", sector.origin[0] == 1.0f && sector.origin[1] == 2.0f);
    expect("hold reach", sector.reach == 2.0f);
    expect("hold facing", fabsf(sector.direction[0] - 1.0f) < 0.001f &&
        fabsf(sector.direction[1]) < 0.001f);
    const float expected_half =
        (5461.0f + 4368.6f / 2.0f) * 6.28318530717958647692f / 65536.0f;
    expect("hold half angle", fabsf(sector.half_angle - expected_half) < 0.001f);
    expect("hold record angle", recomp_hitbox_hold_sector(hold_root, 0, false, 0x2000,
        2.0f, 2.0f, &sector) && fabsf(sector.direction[0] - 0.70710678f) < 0.001f &&
        fabsf(sector.direction[1] + 0.70710678f) < 0.001f);
    expect("hold facing mirror", recomp_hitbox_hold_sector(hold_root, 0, true, 0x2000,
        2.0f, 2.0f, &sector) && fabsf(sector.direction[0] - 0.70710678f) < 0.001f &&
        fabsf(sector.direction[1] - 0.70710678f) < 0.001f);
    expect("hold distance floor", recomp_hitbox_hold_sector(hold_root, 0, false, 0,
        2.0f, -1.0f, &sector) && sector.half_angle <= 3.1415927f);
    expect("hold rejects reach", !recomp_hitbox_hold_sector(hold_root, 0, false, 0,
        0.0f, 2.0f, &sector));

    /* Synthetic guest records: state 0's active hit record draws a fan at its
       root, and its collision record draws the stage-collision ring. */
    setup_fighters();
    putf(SIDE_SETUP + 0x04u, 1.0f);
    putf(SIDE_SETUP + 0x08u, 0.9f);
    putf(SIDE_SETUP + 0x0cu, 2.0f);
    putf(SIDE_SETUP + SIDE_STRIDE + 0x04u, 1.0f);
    putf(SIDE_SETUP + SIDE_STRIDE + 0x08u, 0.9f);
    putf(SIDE_SETUP + SIDE_STRIDE + 0x0cu, 3.5f);
    setup_hold_record();
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("hold sector draws", submit_count == 5u);
    /* Outline only: no triangles for the flat search area. */
    expect("hold sector fan", command_draws(&commands[4]) &&
        commands[4].data.draw.primitive_type == 2u &&
        vertices_have(&commands[4].data.draw, 0xe0ff4040u));
    expect("hold sector at root", vertices_have_point(&commands[4].data.draw,
        1.0f, 0.9f, 2.0f) && vertices_at_height(&commands[4].data.draw, 0.9f));

    /* P1's facing is P2's +0x18 yaw (0) + 0x4000 + the record's +0x14
       (0x2000), unmirrored; P2's is P1's 0x2000 + 0x4000 with its own +0x3e
       negating the record, so the two fans face 45 degrees apart. Each edge
       end pins a fan's direction and sweep. */
    const float turn_radians = 6.28318530717958647692f / 65536.0f;
    const float hold_half = (5461.0f + 4368.6f / 1.5f) * turn_radians;
    const float hold_p1[3] = {1.0f, 0.9f, 2.0f}, hold_p2[3] = {1.0f, 0.9f, 3.5f};
    expect("hold sector faces P1", fan_faces(&commands[4].data.draw, hold_p1, 1.5f,
        hold_half, 0x6000u * turn_radians));
    expect("hold sector faces P2", fan_faces(&commands[4].data.draw, hold_p2, 1.5f,
        hold_half, 0x4000u * turn_radians));

    /* The +0x40 replay branch reads the bank from the mirrored side, not P1's
       own bank or the override. Both of those name empty bank 1, so P1's fan
       returns only when the replay read is taken. */
    put8(SIDE_SETUP + 0x21u, 1u);
    put8(BANK_INDEX_OVERRIDE, 1u);
    put8(SIDE_SETUP + 0x36u, 1u);
    put8(SIDE_SETUP + 0x40u, 1u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("hold sector replay", submit_count == 5u &&
        fan_faces(&commands[4].data.draw, hold_p1, 1.5f, hold_half, 0x6000u * turn_radians) &&
        fan_faces(&commands[4].data.draw, hold_p2, 1.5f, hold_half, 0x4000u * turn_radians));
    /* Without the replay flag, the mirror override's byte alone names the
       bank; at 1 it is the same empty bank, so only P2's fan is left. */
    put8(SIDE_SETUP + 0x40u, 0u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("hold sector override", submit_count == 5u &&
        !fan_faces(&commands[4].data.draw, hold_p1, 1.5f, hold_half, 0x6000u * turn_radians) &&
        fan_faces(&commands[4].data.draw, hold_p2, 1.5f, hold_half, 0x4000u * turn_radians));
    /* At 0, the same mirror draws P1's fan from bank 0. */
    put8(BANK_INDEX_OVERRIDE, 0u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("hold sector override bank", submit_count == 5u &&
        fan_faces(&commands[4].data.draw, hold_p1, 1.5f, hold_half, 0x6000u * turn_radians) &&
        fan_faces(&commands[4].data.draw, hold_p2, 1.5f, hold_half, 0x4000u * turn_radians));
    /* Back to the plain own-bank read. */
    put8(SIDE_SETUP + 0x36u, 0u);
    put8(SIDE_SETUP + 0x21u, 0u);

    /* The gate is the current hit record's +0x0d byte. */
    const uint32_t hit = SCRATCH + 0x100u + 0x18u;
    put8(hit + 0x0du, 0u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("hold sector gate", submit_count == 4u);
    put8(hit + 0x0du, 1u);

    /* Body radius: the worst case radius of the ring's flat XZ circle. */
    setup_body_record();
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("body radius draws", submit_count == 6u);
    expect("body radius ring", command_draws(&commands[5]) &&
        commands[5].data.draw.primitive_type == 2u &&
        vertices_have(&commands[5].data.draw, 0xc06080ffu) &&
        vertices_at_height(&commands[5].data.draw, 0.9f));
    expect("body radius value", fabsf(max_xz_radius(&commands[5].data.draw, 1.0f, 2.0f) -
        1.25f) < 0.001f);
    putf(SCRATCH + 0x200u + 0x04u, 4.0f);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("body radius clamp", submit_count == 6u &&
        fabsf(max_xz_radius(&commands[5].data.draw, 1.0f, 2.0f) - 2.5f) < 0.001f);

    /* Only the recorded collision states draw it, and a missing move stops
       the walk at the table's 0xffff terminator. */
    put8(SIDE_SETUP + 0x37u, 1u);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("body radius state", submit_count == 5u);
    put8(SIDE_SETUP + 0x37u, 3u);
    put16(SIDE_SETUP + 0x32u, 5u);
    put16(SCRATCH + 0x210u, 0xffffu);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("body radius move", submit_count == 5u);
    put16(SIDE_SETUP + 0x32u, 0u);
    put32(CHARACTER_COLLISION_RECORDS + 2u * 4u, 0xffffffffu);
    submit_count = 0u;
    recomp_hitbox_view_overlay(&target);
    expect("body radius pointer", submit_count == 5u);

    return fails == 0 ? 0 : 1;
}
