// Single-size repeated resadd, size from argv. Lets two cores stay on the SAME
// working set for the whole measurement, so parallel-vs-single is comparable.
// Used to separate shared-L2 thrashing from bus saturation.
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

int main(int argc, char **argv) {
    size_t I = argc > 1 ? (size_t)atoi(argv[1]) : 1024;
    size_t J = argc > 2 ? (size_t)atoi(argv[2]) : 1024;
    int reps  = argc > 3 ? atoi(argv[3]) : 8;
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    tiled_resadd_auto(I, J, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                      (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
    unsigned long s = read_cycles();
    for (int r = 0; r < reps; r++)
        tiled_resadd_auto(I, J, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                          (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
    unsigned long e = read_cycles();
    printf("RES %lu %lu KB=%lu reps=%d ticks=%lu per=%lu\n",
           (unsigned long)I, (unsigned long)J, (unsigned long)(I*J*3/1024),
           reps, (unsigned long)(e-s), (unsigned long)((e-s)/reps));
    return 0;
}
