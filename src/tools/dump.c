// dump.c - offline frame dumper: renders the demo scene from a set of camera
// poses and writes each frame as a P6 PPM (pixel-doubled horizontally to show
// the true 2:1 MODE 5 aspect), for eyeballing without a window.

#include <stdio.h>
#include <stdlib.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "mode5.h"
#include "render.h"

typedef struct pose {
    uint16_t x, y;
    uint8_t angle;
    uint8_t cell;
    const char *name;
} pose;

static pt_map map;
static pt_render render;
static uint8_t rgba[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];

static void write_ppm(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fprintf(fp, "P6\n%d %d\n255\n", PT_SCREEN_WIDTH * 2, PT_SCREEN_SCANLINES);
    for (int yy = 0; yy < PT_SCREEN_SCANLINES; yy++) {
        for (int xx = 0; xx < PT_SCREEN_WIDTH; xx++) {
            const uint8_t *p = &rgba[(yy * PT_SCREEN_WIDTH + xx) * 4];
            fwrite(p, 1, 3, fp);
            fwrite(p, 1, 3, fp);    // double width pixels
        }
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    pt_arith_init();
    pt_scene_init(&map);
    pt_render_init(&render, &map);

    static const pose poses[] = {
        {1756, 1536, 128, 2, "ring_facing_pit"},    // over the pit toward the corridor
        {1756, 1536, 0, 2, "ring_facing_wall"},     // straight at the pentagon wall
        {1696, 1736, 160, 2, "ring_oblique"},       // oblique across the room
        {1536, 1536, 128, 1, "in_pit"},             // standing in the depression
        {1336, 1386, 96, 5, "ring_d"},              // from ring cell D
        {1276, 1536, 128, 4, "facing_corridor"},    // into the corridor
        {1066, 1536, 128, 7, "in_corridor"},        // inside the corridor
        {1036, 1536, 0, 7, "corridor_back"},        // looking back out
        {736, 1536, 128, 8, "room2"},               // the far room
        {836, 1636, 32, 8, "room2_to_door"},        // back toward the doorway
        {712, 1436, 192, 8, "stair_door"},          // facing the stair doorway
        {712, 1236, 192, 11, "stairs_up"},          // climbing the staircase
        {712, 1056, 224, 15, "upper_corridor"},     // into the first bend
        {742, 944, 213, 16, "second_bend"},         // toward the doorway
        {1136, 683, 107, 18, "end_room"},           // looking back at the door
    };

    char path[512];
    for (unsigned i = 0; i < sizeof(poses) / sizeof(poses[0]); i++) {
        const pose *p = &poses[i];
        pt_render_frame(&render, p->x, p->y, p->angle, p->cell);
        pt_mode5_to_rgba(render.fb, rgba);
        snprintf(path, sizeof(path), "%s/%02u_%s.ppm", dir, i, p->name);
        write_ppm(path);
        printf("%s: pool %u pieces\n", path, render.pool_num - 1);
    }
    printf("pool high water: %u\n", render.pool_high - 1);
    return 0;
}
