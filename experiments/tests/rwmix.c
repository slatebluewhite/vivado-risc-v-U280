// Is the ~6 B/cycle ceiling direction-dependent?
// (a) read-dominated: tall-K matmul reads 256KB, writes 1KB  (~99.6% reads)
// (b) balanced:       resadd reads 2/3, writes 1/3
// Both measured in a fixed tick window (E16 methodology).
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define KK 4096
#define IJ 32

static elem_t MA[IJ][KK] row_align(1);
static elem_t MB[KK][IJ] row_align(1);
static elem_t MC[IJ][IJ] row_align(1);

#define RI 512
#define RJ 512
static elem_t RA[RI][RJ] row_align(1);
static elem_t RB[RI][RJ] row_align(1);
static elem_t RC[RI][RJ] row_align(1);

int main(int argc, char **argv) {
    unsigned long budget = argc > 1 ? (unsigned long)atoi(argv[1]) : 200000;
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    // (a) read-dominated
    {
        tiled_matmul_auto(IJ, IJ, KK, (elem_t*)MA, (elem_t*)MB, NULL, (elem_t*)MC,
            KK, IJ, IJ, IJ, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
            NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);
        unsigned long s = read_cycles(), now = s, it = 0;
        while ((now - s) < budget) {
            tiled_matmul_auto(IJ, IJ, KK, (elem_t*)MA, (elem_t*)MB, NULL, (elem_t*)MC,
                KK, IJ, IJ, IJ, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
                NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, false, false, false, false, false, 0, WS);
            it++; now = read_cycles();
        }
        double bytes = (double)it * (2.0*IJ*KK + (double)IJ*IJ);
        double rd = (double)it * 2.0*IJ*KK, wr = (double)it * IJ*IJ;
        printf("READDOM iters=%lu Bper1kcyc=%lu readpct=%lu\n", it,
               (unsigned long)(bytes/((now-s)*100.0)*1000), (unsigned long)(rd/(rd+wr)*100));
    }

    // (b) balanced resadd
    {
        tiled_resadd_auto(RI, RJ, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                          (elem_t*)RA, (elem_t*)RB, (elem_t*)RC, true, WS);
        unsigned long s = read_cycles(), now = s, it = 0;
        while ((now - s) < budget) {
            tiled_resadd_auto(RI, RJ, 2, MVIN_SCALE_IDENTITY, ACC_SCALE_IDENTITY,
                              (elem_t*)RA, (elem_t*)RB, (elem_t*)RC, true, WS);
            it++; now = read_cycles();
        }
        double bytes = (double)it * 3.0*RI*RJ;
        printf("BALANCED iters=%lu Bper1kcyc=%lu readpct=67\n", it,
               (unsigned long)(bytes/((now-s)*100.0)*1000));
    }
    printf("RWMIX_DONE\n");
    return 0;
}
