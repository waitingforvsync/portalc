// testmaps.h - map variants shared by the tests and the Beeb port's host
// tool: the demo scene with extra sprites, so that frames hold several.

#ifndef PT_TESTMAPS_H_
#define PT_TESTMAPS_H_

#include "map.h"

// A 64-unit sprite in the ring's five cells and room 2's first (cells 2-6
// and 8), each at its cell's centroid, on top of the boulder: seven in all.
// The map must be the demo scene; returns pt_map_validate's verdict.
bool pt_testmaps_add_sprites(pt_map *m);

#endif
