// D4 validation: is folding a residual add into the matmul accumulator (via the
// bias input D) faster than doing matmul and then a separate resadd pass?
// Unfused: matmul -> DRAM, then resadd reads C + S and writes C  (3 extra streams)
// Fused:   matmul with D=S, the add happens in the accumulator (0 extra streams)
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define N 256

static elem_t MA[N][N] row_align(1);
static elem_t MB[N][N] row_align(1);
static elem_t MC[N][N] row_align(1);
static elem_t MS[N][N] row_align(1);   // skip connection tensor

int main(int argc, char **argv) {
    unsigned long budget = argc > 1 ? (unsigned long)atoi(argv[1]) : 200000;
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    // (a) unfused: matmul, then a separate residual add pass
    {
        unsigned long s = read_cycles(), now = s, it = 0;
        while ((now - s) < budget) {
            tiled_matmul_auto(N, N, N, (elem_t*)MA, (elem_t*)MB, NULL, (elem_t*)MC,
                N, N, N, N, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);
            tiled_resadd_auto(N, N, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                ACC_SCALE_IDENTITY, (elem_t*)MC, (elem_t*)MS, (elem_t*)MC, false, WS);
            it++; now = read_cycles();
        }
        printf("UNFUSED iters=%lu ticks_per=%lu\n", it, (now-s)/(it?it:1));
    }

    // (b) fused: skip tensor supplied as the accumulator's initial value (bias)
    {
        unsigned long s = read_cycles(), now = s, it = 0;
        while ((now - s) < budget) {
            tiled_matmul_auto(N, N, N, (elem_t*)MA, (elem_t*)MB, (void*)MS, (elem_t*)MC,
                N, N, N, N, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false,
                true /*low_D: D is elem_t*/, 0, WS);
            it++; now = read_cycles();
        }
        printf("FUSED iters=%lu ticks_per=%lu\n", it, (now-s)/(it?it:1));
    }
    printf("FUSE_DONE\n");
    return 0;
}
