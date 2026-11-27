/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_HITBOX_VIEW_H
#define DOA2U_RECOMP_HITBOX_VIEW_H

#include "d3d_presenter.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sparring's 3D hitbox viewer: world-space volumes drawn by the semantic
   presenter and depth-tested against the scene. This pass draws only data
   already proven in guest RAM: the two fighters' skeletons, each fighter's
   hold/throw search sector and its stage-collision body radius. Strikes are
   not drawn because the game decides them from authored script data, with no
   world-space strike geometry. */

typedef enum RecompHitboxColour {
    RECOMP_HITBOX_JOINT,   /* a bone joint */
    RECOMP_HITBOX_HIPS,    /* bone 0, a larger marker */
    RECOMP_HITBOX_ATTACK,  /* the hold/throw search sector */
    RECOMP_HITBOX_HURT,    /* the stage-collision body radius */
    RECOMP_HITBOX_COLOURS,
} RecompHitboxColour;

typedef enum RecompHitboxShape {
    RECOMP_HITBOX_SPHERE,
    RECOMP_HITBOX_FAN,   /* flat XZ sector from A toward B, radius and half_angle */
    RECOMP_HITBOX_RING,  /* flat XZ circle around A with radius */
} RecompHitboxShape;

/* Which fighter a volume belongs to, for its colour: P1 cyan, P2 yellow. A
   source that is not one fighter's, such as a throw cone, stays neutral. */
typedef enum RecompHitboxSide {
    RECOMP_HITBOX_SIDE_ANY,  /* the colour class's own colour */
    RECOMP_HITBOX_SIDE_ONE,
    RECOMP_HITBOX_SIDE_TWO,
    RECOMP_HITBOX_SIDES,
} RecompHitboxSide;

/* A is a point or the first end, B the second end; a sphere and a ring ignore
   B, and a fan uses B only for its XZ direction, with radius as the reach and
   half_angle as the half sweep in radians. */
typedef struct RecompHitboxVolume {
    RecompHitboxShape shape;
    RecompHitboxColour colour;
    float a[3];
    float b[3];
    float radius;
    RecompHitboxSide side;
    float half_angle;
} RecompHitboxVolume;

/* The hold/throw search fighter_hold_target_search (0x104b50) runs: a flat
   XZ fan at the fighter's root, pointing along the opponent's +0x18 yaw plus
   0x4000 (a quarter turn), plus the current hit record's +0x14 angle, negated
   when the fighter's own +0x3e facing flag is 1. Angles are 16-bit turns
   (0x10000 = 360°) and the half sweep is 0x1555 + 4368.6 / surface distance,
   as in fighter_object_sector_search (0x145890). Returns false when an input
   is unusable. */
typedef struct RecompHitboxHoldSector {
    float origin[2];     /* world XZ */
    float direction[2];  /* unit world XZ */
    float reach;         /* metres */
    float half_angle;    /* radians */
} RecompHitboxHoldSector;

bool recomp_hitbox_hold_sector(const float root[2], int32_t other_yaw,
    bool mirrored, int32_t record_angle, float reach, float surface_distance,
    RecompHitboxHoldSector *out);

/* PC Settings' Hitboxes row: Off, On and X-ray, their indexes in this order. */
typedef enum RecompHitboxMode {
    RECOMP_HITBOX_MODE_OFF,
    RECOMP_HITBOX_MODE_ON,
    RECOMP_HITBOX_MODE_XRAY,  /* through the scene, at about a third the alpha */
    RECOMP_HITBOX_MODES,
} RecompHitboxMode;

/* The row's choice. The saved choice stays while Live is on; the viewer itself
   is off for that session. */
void recomp_hitbox_view_set_choice(unsigned choice);
/* Online locks the viewer off for the session and offline restores the saved
   choice, like the Free Camera. */
void recomp_hitbox_view_set_online(bool online);
/* Adds one volume for the next submit; false when the list is full, the
   colour, shape or side is unknown, or a point or radius is not sane. */
bool recomp_hitbox_view_add(const RecompHitboxVolume *volume);
unsigned recomp_hitbox_view_count(void);
/* Draws the list with the frame's captured camera: translucent triangles for
   spheres, 1-pixel lines for sphere outlines, fan outlines and rings, one draw
   per colour class and primitive. Does nothing without a camera. */
void recomp_hitbox_view_submit(const RecompD3dPresenterTarget *target);
/* RECOMP_D3D_OVERLAY_HITBOXES: in Sparring and Training, adds the two
   fighters' skeletons, hold/throw sectors and body radii, submits the list,
   then empties it. */
void recomp_hitbox_view_overlay(const RecompD3dPresenterTarget *target);

#ifdef __cplusplus
}
#endif

#endif
