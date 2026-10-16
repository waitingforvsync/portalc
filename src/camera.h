// camera.h - camera state, movement and sliding collision. The camera is a
// circle of radius PT_CAM_RADIUS at an unsigned u16.8 world position,
// always assigned to the cell containing its centre, as the angles of the
// cells' vertices from there tell it: the app's vertex cache, which the
// frame's capture then shares.

#ifndef PT_CAMERA_H_
#define PT_CAMERA_H_

#include <stdbool.h>
#include <stdint.h>

#include "arith.h"

#include "map.h"
#include "vcache.h"

#define PT_CAM_RADIUS 40
_Static_assert(PT_CAM_RADIUS * PT_CAM_RADIUS == PT_RPOST_END, "the posts' table covers the circle");
#define PT_CAM_MOVE_SPEED 1    // whole units per tick
#define PT_CAM_TURN_SPEED 1    // 256ths of a turn per tick
#define PT_CAM_MAX_TICKS 8     // a frame's at most: its move under 12 units, as camera.c needs

// the start pose in the demo scene
#define PT_CAM_START_X 1756
#define PT_CAM_START_Y 1536
#define PT_CAM_START_ANGLE 128

// input bits for one tick
enum {
    PT_IN_FWD = 1 << 0,
    PT_IN_BACK = 1 << 1,
    PT_IN_LEFT = 1 << 2,     // strafe
    PT_IN_RIGHT = 1 << 3,    // strafe
    PT_IN_TURNL = 1 << 4,
    PT_IN_TURNR = 1 << 5,
};

typedef struct pt_camera {
    uint32_t x;       // u16.8
    uint32_t y;       // u16.8
    uint8_t angle;    // 256ths of a turn; 0 faces world +x
    uint8_t cell;
    bool settled;     // the last tick stood still, and its resolve pushed nothing
} pt_camera;

// Place the camera, and have the vertex cache look from there; the position
// must lie inside some cell.
void pt_camera_init(pt_camera *c, const pt_map *m, pt_vcache *vc, uint16_t x, uint16_t y,
                    uint8_t angle);

// Advance a frame's ticks (1 to PT_CAM_MAX_TICKS), which share its keys, as
// one: turn and move by that many ticks' worth, with sliding collision, and
// if the camera moved, have the vertex cache look afresh from where it ends
// up and cross portals by its angles.
void pt_camera_tick(pt_camera *c, const pt_map *m, pt_vcache *vc, uint8_t input, uint8_t ticks);

#endif
