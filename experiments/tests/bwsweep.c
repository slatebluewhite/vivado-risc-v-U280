// Memory bandwidth sweep: pure elementwise resadd (zero MACs) over working sets
// that cross the 512KB L2 boundary, to find where DRAM bandwidth becomes the limit.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define MAXI 2048
#define MAXJ 2048

static elem_t A[MAXI][MAXJ] row_align(1);
static elem_t B[MAXI][MAXJ] row_align(1);
static elem_t C[MAXI][MAXJ] row_align(1);

int main() {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    static const size_t dims[][2] = {
        {128,512}, {256,512}, {512,512}, {512,1024},
        {1024,1024}, {1024,2048}, {2048,2048},
    };
    const int n = sizeof(dims)/sizeof(dims[0]);

    printf("I,J,KB,ticks\n");
    for (int k = 0; k < n; k++) {
        size_t I = dims[k][0], J = dims[k][1];
        // warm-up pass, then measure steady state
        tiled_resadd_auto(I, J, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                          (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
        unsigned long s = read_cycles();
        tiled_resadd_auto(I, J, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                          (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
        unsigned long e = read_cycles();
        printf("%lu,%lu,%lu,%lu\n", (unsigned long)I, (unsigned long)J,
               (unsigned long)(I*J*3/1024), (unsigned long)(e-s));
    }
    printf("SWEEP_DONE\n");
    return 0;
}
