// Fixed-time-window throughput test. Both cores run for the SAME tick budget and
// report how many iterations they completed, so parallel runs are guaranteed to
// overlap for essentially the whole measurement (unlike fixed-iteration timing).
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
    size_t I     = argc > 1 ? (size_t)atoi(argv[1]) : 1024;
    size_t J     = argc > 2 ? (size_t)atoi(argv[2]) : 1024;
    unsigned long budget = argc > 3 ? (unsigned long)atoi(argv[3]) : 30000; // ticks
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    tiled_resadd_auto(I, J, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                      (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);

    unsigned long s = read_cycles(), now = s;
    unsigned long iters = 0;
    while ((now - s) < budget) {
        tiled_resadd_auto(I, J, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                          (elem_t*)A, (elem_t*)B, (elem_t*)C, true, WS);
        iters++;
        now = read_cycles();
    }
    unsigned long elapsed = now - s;
    // bytes moved = iters * I*J*3 ; report bytes-per-1000-cycles to keep integers
    unsigned long bp1k = (unsigned long)((double)iters * I * J * 3 / (elapsed * 100.0) * 1000);
    printf("RATE %lu %lu KB=%lu iters=%lu elapsed=%lu Bper1kcyc=%lu\n",
           (unsigned long)I, (unsigned long)J, (unsigned long)(I*J*3/1024),
           iters, elapsed, bp1k);
    return 0;
}
