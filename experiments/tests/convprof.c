// Where do conv cycles actually go? Instrument a ResNet-shaped 3x3 conv with
// Gemmini's own performance counters (8 slots, so two passes of 8 signals).
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

// ResNet-50 mid-network shape: 28x28x128 -> 28x28x128, 3x3
#define BATCH 4
#define IDIM  28
#define ICH   128
#define OCH   128
#define KDIM  3
#define PAD   1
#define ODIM  28

static elem_t In[BATCH*IDIM*IDIM][ICH] row_align(1);
static elem_t W[KDIM*KDIM*ICH][OCH] row_align(1);
static acc_t  Bias[OCH] row_align_acc(1);
static elem_t Out[BATCH*ODIM*ODIM][OCH] row_align(1);

static void run_conv(void) {
    tiled_conv_auto(BATCH, IDIM, IDIM, ICH, OCH, ODIM, ODIM,
        1 /*stride*/, 1 /*input_dilation*/, 1 /*kernel_dilation*/, PAD, KDIM,
        false, false, false, false, false,
        (elem_t*)In, (elem_t*)W, (acc_t*)Bias, (elem_t*)Out,
        NO_ACTIVATION, ACC_SCALE_IDENTITY, 0, 0, 0,
        WS);
}

struct { int code; const char *name; } sig1[] = {
    {MAIN_EX_CYCLES,        "EX만"},
    {MAIN_LD_CYCLES,        "LD만"},
    {MAIN_ST_CYCLES,        "ST만"},
    {MAIN_LD_ST_EX_CYCLES,  "LD+ST+EX 중첩"},
    {EXE_ACTIVE_CYCLE,      "EXE 활성"},
    {EXE_PRELOAD_HAZ_CYCLE, "EXE preload해저드"},
    {EXE_OVERLAP_HAZ_CYCLE, "EXE overlap해저드"},
    {EXE_FLUSH_CYCLE,       "EXE flush"},
};
struct { int code; const char *name; } sig2[] = {
    {SCRATCHPAD_A_WAIT_CYCLE, "SPAD A 대기"},
    {SCRATCHPAD_B_WAIT_CYCLE, "SPAD B 대기"},
    {ACC_D_WAIT_CYCLE,        "ACC D 대기"},
    {LOAD_DMA_WAIT_CYCLE,     "LOAD DMA 대기"},
    {LOAD_ACTIVE_CYCLE,       "LOAD 활성"},
    {STORE_DMA_WAIT_CYCLE,    "STORE DMA 대기"},
    {RDMA_TL_WAIT_CYCLES,     "RDMA TileLink 대기"},
    {WDMA_TL_WAIT_CYCLES,     "WDMA TileLink 대기"},
};

int main(void) {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    unsigned long s = read_cycles();
    run_conv();
    unsigned long e = read_cycles();
    printf("CONV ticks=%lu (cycles=%lu)\n", e-s, (e-s)*100);

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 8; i++)
            counter_configure(i, pass ? sig2[i].code : sig1[i].code);
        counter_reset();
        run_conv();
        for (int i = 0; i < 8; i++)
            printf("CNT %s = %u\n", pass ? sig2[i].name : sig1[i].name, counter_read(i));
    }
    printf("CONVPROF_DONE\n");
    return 0;
}
