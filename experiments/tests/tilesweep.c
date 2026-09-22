// Tile-shape sweep at constant tile area (8KB, the ACC_ROWS/2 limit).
// tile_J sets the contiguous burst length; tile_I sets how many strided jumps.
// Flat bandwidth => hard datapath width limit. Rising with tile_J => burst/latency limit.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define DIM_I 1024
#define DIM_J 1024

static elem_t A[DIM_I][DIM_J] row_align(1);
static elem_t B[DIM_I][DIM_J] row_align(1);
static elem_t C[DIM_I][DIM_J] row_align(1);

int main() {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    // constant area: tile_I * ceil(tile_J/16) == 512 acc rows
    static const size_t tiles[][2] = {
        {16,512}, {32,256}, {64,128}, {128,64}, {256,32}, {512,16},
    };
    const int n = sizeof(tiles)/sizeof(tiles[0]);

    printf("tile_I,tile_J,burstB,ticks\n");
    for (int k = 0; k < n; k++) {
        size_t ti = tiles[k][0], tj = tiles[k][1];
        tiled_resadd(DIM_I, DIM_J, DIM_J, ti, tj, 2,
                     MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                     (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
        unsigned long s = read_cycles();
        tiled_resadd(DIM_I, DIM_J, DIM_J, ti, tj, 2,
                     MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                     (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
        unsigned long e = read_cycles();
        printf("%lu,%lu,%lu,%lu\n", (unsigned long)ti, (unsigned long)tj,
               (unsigned long)tj, (unsigned long)(e-s));
    }
    printf("TILESWEEP_DONE\n");
    return 0;
}
