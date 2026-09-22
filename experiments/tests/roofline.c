// Roofline sweep: square INT8 matmul over N, to locate where this machine
// transitions from compute-bound (mesh-limited) to memory-bound (bus-limited).
// Arithmetic intensity of an N^3 matmul is 2N/3 ops per byte of compulsory traffic.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define MAXN 1024

static elem_t A[MAXN][MAXN] row_align(1);
static elem_t B[MAXN][MAXN] row_align(1);
static elem_t C[MAXN][MAXN] row_align(1);

int main() {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    static const size_t sizes[] = {64, 128, 192, 256, 384, 512, 768, 1024};
    const int n = sizeof(sizes)/sizeof(sizes[0]);

    printf("N,ticks\n");
    for (int k = 0; k < n; k++) {
        size_t N = sizes[k];
        tiled_matmul_auto(N, N, N,
            (elem_t*)A, (elem_t*)B, NULL, (elem_t*)C,
            N, N, N, N,
            MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
            NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
            false, false, false, false, 0, WS);
        unsigned long s = read_cycles();
        tiled_matmul_auto(N, N, N,
            (elem_t*)A, (elem_t*)B, NULL, (elem_t*)C,
            N, N, N, N,
            MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
            NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false,
            false, false, false, false, 0, WS);
        unsigned long e = read_cycles();
        printf("%lu,%lu\n", (unsigned long)N, (unsigned long)(e-s));
    }
    printf("ROOFLINE_DONE\n");
    return 0;
}
