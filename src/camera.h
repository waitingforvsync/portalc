// camera.h - camera state, movement and sliding collision (spec section 10).
// The camera is a circle of radius PT_CAM_RADIUS at an unsigned u16.8 world
// position, always assigned to the cell containing its centre.

#ifndef PT_CAMERA_H_
#define PT_CAMERA_H_

#include <stdint.h>

#include "map.h"

#define PT_CAM_RADIUS 40
#define PT_CAM_MOVE_SPEED (8u << 8)    // units per tick, u16.8
#define PT_CAM_TURN_SPEED 2            // 256ths of a turn per tick

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
} pt_camera;

// Place the camera; the position must be inside the given cell.
void pt_camera_init(pt_camera *c, const pt_map *m, uint8_t cell, uint16_t x, uint16_t y,
                    uint8_t angle);

// Advance one fixed tick: turn, move with sliding collision, cross portals.
void pt_camera_tick(pt_camera *c, const pt_map *m, uint8_t input);

#endif
