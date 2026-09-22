// Self-checking transformer encoder layer for the INT8 + normalization build.
//
// The bundled transformers/transformer.c reports cycles but never checks its output,
// so a build without has_normalizations silently produces wrong results instead of
// failing (see experiments/JOURNAL.md E36). This runs every stage twice - once on
// Gemmini, once through the same routines forced onto the CPU - and diffs them, so
// a missing normalization unit shows up as a FAIL rather than a plausible number.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifndef BAREMETAL
#include <sys/mman.h>
#endif
#include "include/gemmini_testutils.h"

#define SEQ   32
#define HID   64
#define HEADS 4
#define HPH   (HID / HEADS)     // hidden per head
#define EXP   128

static elem_t X   [SEQ][HID] row_align(1);   // input
static elem_t Wq  [HID][HID] row_align(1);
static elem_t Wk  [HID][HID] row_align(1);
static elem_t Wv  [HID][HID] row_align(1);
static elem_t Wo  [HID][HID] row_align(1);
static elem_t W1  [HID][EXP] row_align(1);
static elem_t W2  [EXP][HID] row_align(1);

static elem_t Q[SEQ][HID] row_align(1), K[SEQ][HID] row_align(1), V[SEQ][HID] row_align(1);
static elem_t attn_hw[HEADS][SEQ][SEQ] row_align(1), attn_sw[HEADS][SEQ][SEQ] row_align(1);
static elem_t ctx_hw[SEQ][HID] row_align(1),  ctx_sw[SEQ][HID] row_align(1);
static elem_t proj_hw[SEQ][HID] row_align(1), proj_sw[SEQ][HID] row_align(1);
static elem_t ff1_hw[SEQ][EXP] row_align(1),  ff1_sw[SEQ][EXP] row_align(1);
static elem_t ff2_hw[SEQ][HID] row_align(1),  ff2_sw[SEQ][HID] row_align(1);

// Integer normalization on hardware and in C will not agree bit-for-bit - both round
// at different points - so report the distribution, and treat "off by one" as a pass
// while flagging anything larger as a real divergence.
#define TOL 1

static int diff(const elem_t *a, const elem_t *b, size_t n, const char *name) {
    size_t bad = 0, over_tol = 0; int worst = 0; long sum_abs = 0;
    for (size_t i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
        if (d != 0) bad++;
        if (d > TOL) over_tol++;
        sum_abs += d;
    }
    int ok = (over_tol == 0);
    printf("%-12s %s  nonzero=%lu/%lu  >tol=%lu  max=%d  mean_abs=%ld.%03ld\n",
           name, ok ? "PASS" : "FAIL", (unsigned long)bad, (unsigned long)n,
           (unsigned long)over_tol, worst, sum_abs/(long)n, (sum_abs*1000/(long)n)%1000);
    return ok;
}

static void fill(elem_t *p, size_t n, int seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) { s = s*1103515245u + 12345u; p[i] = (elem_t)((s >> 16) % 15 - 7); }
}

// one matmul stage, run on `type` (WS = Gemmini, CPU = software reference)
// SOFTMAX derives its integer-exp constants by dividing by bert_scale, so passing 0
// divides by zero. The bundled tiled_matmul_ws_softmax test uses 0.05; LAYERNORM and
// IGELU ignore it. (transformers/transformer.c passes 0 here - see JOURNAL E38.)
#define BERT_SCALE 0.05

static void stage_mm(size_t I, size_t J, size_t Kd, const elem_t *A, const elem_t *B,
                     elem_t *C, size_t sA, size_t sB, size_t sC, int act, bool transB,
                     enum tiled_matmul_type_t type) {
    tiled_matmul_auto(I, J, Kd, A, B, NULL, C, sA, sB, 0, sC,
        MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY, MVIN_SCALE_IDENTITY,
        act, ACC_SCALE_IDENTITY, act == SOFTMAX ? BERT_SCALE : 0, false,
        false, transB, false, false, 0, type);
}

int main(void) {
#ifndef BAREMETAL
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) { perror("mlockall failed"); exit(1); }
#endif
    gemmini_flush(0);

    printf("verified encoder: SEQ=%d HID=%d HEADS=%d EXP=%d\n", SEQ, HID, HEADS, EXP);
    fill((elem_t*)X, SEQ*HID, 1);   fill((elem_t*)Wq, HID*HID, 2);
    fill((elem_t*)Wk, HID*HID, 3);  fill((elem_t*)Wv, HID*HID, 4);
    fill((elem_t*)Wo, HID*HID, 5);  fill((elem_t*)W1, HID*EXP, 6);
    fill((elem_t*)W2, EXP*HID, 7);

    unsigned long t0 = read_cycles();
    // QKV projections (shared by both paths - plain matmul, already covered elsewhere)
    stage_mm(SEQ, HID, HID, (elem_t*)X, (elem_t*)Wq, (elem_t*)Q, HID, HID, HID, NO_ACTIVATION, false, WS);
    stage_mm(SEQ, HID, HID, (elem_t*)X, (elem_t*)Wk, (elem_t*)K, HID, HID, HID, NO_ACTIVATION, false, WS);
    stage_mm(SEQ, HID, HID, (elem_t*)X, (elem_t*)Wv, (elem_t*)V, HID, HID, HID, NO_ACTIVATION, false, WS);
    gemmini_fence();
    unsigned long t1 = read_cycles();

    // attention scores with SOFTMAX, per head
    for (int h = 0; h < HEADS; h++)
        stage_mm(SEQ, SEQ, HPH, (elem_t*)Q + h*HPH, (elem_t*)K + h*HPH,
                 (elem_t*)attn_hw[h], HID, HID, SEQ, SOFTMAX, true, WS);
    gemmini_fence();
    unsigned long t2 = read_cycles();
    for (int h = 0; h < HEADS; h++)
        stage_mm(SEQ, SEQ, HPH, (elem_t*)Q + h*HPH, (elem_t*)K + h*HPH,
                 (elem_t*)attn_sw[h], HID, HID, SEQ, SOFTMAX, true, CPU);
    unsigned long t3 = read_cycles();

    // context = attn x V (per head). Both paths take the HARDWARE attention output so
    // this stage is judged on its own, not on whatever softmax did upstream.
    for (int h = 0; h < HEADS; h++) {
        stage_mm(SEQ, HPH, SEQ, (elem_t*)attn_hw[h], (elem_t*)V + h*HPH,
                 (elem_t*)ctx_hw + h*HPH, SEQ, HID, HID, NO_ACTIVATION, false, WS);
        stage_mm(SEQ, HPH, SEQ, (elem_t*)attn_hw[h], (elem_t*)V + h*HPH,
                 (elem_t*)ctx_sw + h*HPH, SEQ, HID, HID, NO_ACTIVATION, false, CPU);
    }
    gemmini_fence();

    // output projection with LAYERNORM
    unsigned long t4 = read_cycles();
    stage_mm(SEQ, HID, HID, (elem_t*)ctx_hw, (elem_t*)Wo, (elem_t*)proj_hw, HID, HID, HID, LAYERNORM, false, WS);
    gemmini_fence();
    unsigned long t5 = read_cycles();
    stage_mm(SEQ, HID, HID, (elem_t*)ctx_hw, (elem_t*)Wo, (elem_t*)proj_sw, HID, HID, HID, LAYERNORM, false, CPU);
    unsigned long t6 = read_cycles();

    // FFN: expand with IGELU, then project back with LAYERNORM
    stage_mm(SEQ, EXP, HID, (elem_t*)proj_hw, (elem_t*)W1, (elem_t*)ff1_hw, HID, EXP, EXP, IGELU, false, WS);
    gemmini_fence();
    unsigned long t7 = read_cycles();
    stage_mm(SEQ, EXP, HID, (elem_t*)proj_hw, (elem_t*)W1, (elem_t*)ff1_sw, HID, EXP, EXP, IGELU, false, CPU);
    unsigned long t8 = read_cycles();

    stage_mm(SEQ, HID, EXP, (elem_t*)ff1_hw, (elem_t*)W2, (elem_t*)ff2_hw, EXP, HID, HID, LAYERNORM, false, WS);
    stage_mm(SEQ, HID, EXP, (elem_t*)ff1_hw, (elem_t*)W2, (elem_t*)ff2_sw, EXP, HID, HID, LAYERNORM, false, CPU);
    gemmini_fence();

    printf("\n-- correctness (Gemmini vs CPU) --\n");
    int ok = 1;
    ok &= diff((elem_t*)attn_hw, (elem_t*)attn_sw, HEADS*SEQ*SEQ, "softmax");
    ok &= diff((elem_t*)ctx_hw,  (elem_t*)ctx_sw,  SEQ*HID,       "attn x V");
    ok &= diff((elem_t*)proj_hw, (elem_t*)proj_sw, SEQ*HID,       "layernorm");
    ok &= diff((elem_t*)ff1_hw,  (elem_t*)ff1_sw,  SEQ*EXP,       "igelu");
    ok &= diff((elem_t*)ff2_hw,  (elem_t*)ff2_sw,  SEQ*HID,       "layernorm2");

    printf("\n-- cycles (ticks) --\n");
    printf("qkv(hw)=%lu softmax(hw)=%lu softmax(cpu)=%lu layernorm(hw)=%lu layernorm(cpu)=%lu igelu(hw)=%lu igelu(cpu)=%lu\n",
           t1-t0, t2-t1, t3-t2, t5-t4, t6-t5, t7-t6, t8-t7);
    // ---- softmax J sweep -------------------------------------------------------
    // Softmax needs max() and sum() over a whole output row. If the hardware folds
    // those per DIM-wide tile instead of per row, results should match the CPU only
    // while J fits in one tile. Sweep J across the DIM=16 boundary to find out.
    printf("\n-- softmax vs J (row length) --\n");
    static elem_t sm_hw[SEQ][128] row_align(1), sm_sw[SEQ][128] row_align(1);
    static const size_t Js[] = {8, 16, 32, 64, 128};
    for (size_t k = 0; k < sizeof(Js)/sizeof(Js[0]); k++) {
        size_t J = Js[k];
        memset(sm_hw, 0, sizeof(sm_hw)); memset(sm_sw, 0, sizeof(sm_sw));
        stage_mm(SEQ, J, HPH, (elem_t*)Q, (elem_t*)K, (elem_t*)sm_hw, HID, HID, 128, SOFTMAX, true, WS);
        gemmini_fence();
        stage_mm(SEQ, J, HPH, (elem_t*)Q, (elem_t*)K, (elem_t*)sm_sw, HID, HID, 128, SOFTMAX, true, CPU);
        size_t bad = 0; int worst = 0;
        for (size_t i = 0; i < SEQ; i++)
            for (size_t j = 0; j < J; j++) {
                int d = (int)sm_hw[i][j] - (int)sm_sw[i][j];
                if (d < 0) d = -d;
                if (d > worst) worst = d;
                if (d > TOL) bad++;
            }
        printf("  J=%3lu (%lu tiles of %d)  %s  >tol=%lu/%lu  max=%d\n",
               (unsigned long)J, (unsigned long)((J + DIM - 1)/DIM), DIM,
               bad == 0 ? "PASS" : "FAIL", (unsigned long)bad,
               (unsigned long)(SEQ*J), worst);
    }

    // ---- who is right? float oracle --------------------------------------------
    // Both paths approximate exp with the integer I-BERT quadratic, so neither will
    // match true softmax exactly. Computing it in double tells us which one drifts
    // further, i.e. whether the hardware or the C reference is the odd one out.
    {
        const size_t J = 32;
        memset(sm_hw, 0, sizeof(sm_hw)); memset(sm_sw, 0, sizeof(sm_sw));
        stage_mm(SEQ, J, HPH, (elem_t*)Q, (elem_t*)K, (elem_t*)sm_hw, HID, HID, 128, SOFTMAX, true, WS);
        gemmini_fence();
        stage_mm(SEQ, J, HPH, (elem_t*)Q, (elem_t*)K, (elem_t*)sm_sw, HID, HID, 128, SOFTMAX, true, CPU);

        double err_hw = 0, err_sw = 0; int worst_hw = 0, worst_sw = 0;
        for (size_t i = 0; i < SEQ; i++) {
            double e[128]; double mx = -1e30, sum = 0;
            for (size_t j = 0; j < J; j++) {
                long acc = 0;
                for (size_t k = 0; k < HPH; k++)
                    acc += (long)Q[i][k] * (long)K[j][k];
                e[j] = (double)acc * BERT_SCALE;
                if (e[j] > mx) mx = e[j];
            }
            for (size_t j = 0; j < J; j++) { e[j] = exp(e[j] - mx); sum += e[j]; }
            for (size_t j = 0; j < J; j++) {
                double ref = 127.0 * e[j] / sum;
                double dh = fabs((double)sm_hw[i][j] - ref);
                double ds = fabs((double)sm_sw[i][j] - ref);
                err_hw += dh; err_sw += ds;
                if (dh > worst_hw) worst_hw = (int)dh;
                if (ds > worst_sw) worst_sw = (int)ds;
            }
        }
        size_t n = SEQ*J;
        printf("\n-- softmax vs true (double) softmax, J=32 --\n");
        printf("  hardware : mean_abs_err=%d.%03d  max=%d\n",
               (int)(err_hw/n), (int)((err_hw/n - (int)(err_hw/n))*1000), worst_hw);
        printf("  C ref    : mean_abs_err=%d.%03d  max=%d\n",
               (int)(err_sw/n), (int)((err_sw/n - (int)(err_sw/n))*1000), worst_sw);
        printf("  verdict  : %s is closer to true softmax\n",
               err_hw < err_sw ? "HARDWARE" : "C REFERENCE");
    }

    printf("\nENCODER_%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
