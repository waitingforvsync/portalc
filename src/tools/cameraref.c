// cameraref.c - the camera's tick's answers as JSON, for make test's check of the Beeb's
// (beeb/test/camera.6502). Run as portalc_cameraref > camera.json, over the demo scene. Built as
// portalc_camerastressref (CAMERA_STRESS), it's over the stress scene: its wedge's sharp corner
// blocks a slide for its three rounds, which the demo scene's square ones never do.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "places.h"
#include "vcache.h"

#define FRAMES 32    // from each place

static pt_map m;
static pt_vcache vc;

// A 32-bit Galois LFSR (taps &80200003), stepped a byte on (eight steps) before each made-up
// value, as test/test.6502's random_byte steps it.
static uint32_t random = 0xACE1;

static uint8_t step(void)
{
    for (unsigned i = 0; i < 8; i++)
        random = (random >> 1) ^ ((random & 1) ? 0x80200003u : 0);
    return (uint8_t)random;
}

static void hex(const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        printf("%02X", b[i]);
}

// A frame's camera: its x and y (fraction, low, high), its facing (1024ths, low then high), its
// cell and settled.
static void frame_bytes(const pt_camera *c)
{
    uint16_t facing = (uint16_t)(c->angle << 2);
    uint8_t b[10] = {(uint8_t)c->x, (uint8_t)(c->x >> 8), (uint8_t)(c->x >> 16),
                     (uint8_t)c->y, (uint8_t)(c->y >> 8), (uint8_t)(c->y >> 16),
                     (uint8_t)facing, (uint8_t)(facing >> 8), c->cell, c->settled};
    hex(b, sizeof b);
}

int main(void)
{
    pt_arith_init();
#ifdef CAMERA_STRESS
    pt_stress_scene_init(&m);
#else
    pt_scene_init(&m);
#endif
    pt_vcache_init(&vc);

    // from each place, the camera facing a made-up way, then frames of made-up keys (6 bits) and
    // ticks (1 to PT_CAM_MAX_TICKS, a byte's low bits): after each, its x and y (fraction, low,
    // high), its facing (1024ths, low then high), its cell and settled
    _Static_assert((PT_CAM_MAX_TICKS & (PT_CAM_MAX_TICKS - 1)) == 0, "ticks: a byte's low bits");
    static place at[PT_MAX_PLACES];
    unsigned n = places(&m, at);
    printf("{\n  \"places\": [");
    for (unsigned p = 0; p < n; p++) {
        uint8_t angle = step();
        pt_camera c = {.x = (uint32_t)at[p].x << 8, .y = (uint32_t)at[p].y << 8, .angle = angle,
                       .cell = at[p].cell, .settled = false};
        pt_vcache_reset(&vc, at[p].x, at[p].y);
        printf(p ? ",\n    \"" : "\n    \"");
        for (unsigned f = 0; f < FRAMES; f++) {
            uint8_t keys = step() & 63;
            uint8_t ticks = (uint8_t)((step() & (PT_CAM_MAX_TICKS - 1)) + 1);
            pt_camera_tick(&c, &m, &vc, keys, ticks);
            frame_bytes(&c);
        }
        printf("\"");
    }

    // from each place near a vertex, the camera facing it (rounded to 256ths), then
    // PT_CAM_MAX_TICKS ticks a frame into the corner: forward 16, forward and left 8, forward and
    // right 8
    printf("\n  ],\n  \"corners\": [");
    for (unsigned p = 0; p < n; p++) {
        printf(p ? ",\n    \"" : "\n    \"");
        if (at[p].vertex != 0xFF) {
            pt_vcache_reset(&vc, at[p].x, at[p].y);
            uint16_t a = pt_vcache_vert(&vc, &m, at[p].vertex)->angle;
            pt_camera c = {.x = (uint32_t)at[p].x << 8, .y = (uint32_t)at[p].y << 8,
                           .angle = (uint8_t)((a + 2u) >> 2), .cell = at[p].cell, .settled = false};
            for (unsigned f = 0; f < FRAMES; f++) {
                uint8_t keys = f < 16   ? PT_IN_FWD
                               : f < 24 ? PT_IN_FWD | PT_IN_LEFT
                                        : PT_IN_FWD | PT_IN_RIGHT;
                pt_camera_tick(&c, &m, &vc, keys, PT_CAM_MAX_TICKS);
                frame_bytes(&c);
            }
        }
        printf("\"");
    }
    printf("\n  ]\n}\n");
    return 0;
}
