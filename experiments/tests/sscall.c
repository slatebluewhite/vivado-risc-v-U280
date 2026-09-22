// E632: **호출당 상수**를 직접 잰다. E630 이 남긴 마지막 비효율은 op 경계가 아니라
// 호출 경계(FSM 기동 + mvout 드레인)이고, 4 점 회귀의 절편으로만 추정돼 있었다(~49).
//
// `grt_ss_gemv_blk(y, ppStart, ppCount)` 는 **같은 데이터 배치로** pp 블록 범위만 돌린다.
// 그러므로 한 GEMV 를 k 번에 나눠 부르면 일도 바이트도 mvout 수도 그대로이고 **호출 수만**
// k 배가 된다. 시간을 k 에 회귀한 기울기가 곧 호출당 상수다.
//
// shape 의존성이 원인을 가른다:
//   상수가 S 에 비례 -> mvout 드레인 (pp 당 mvout 이 S 개)
//   상수가 평평      -> FSM 기동 / 파이프 깊이
//
//   ./sscall <shape> <K> <N> <T> [--neg]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
#ifdef SPB
static const int SP_BANKS = SPB, SP_ENT = 16384 / SPB;
#else
static int SP_BANKS = 8, SP_ENT = 2048;
#endif

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

#define KMAX 256
#define NMAX 512
static int K = 128, N = 512;

static int8_t W[KMAX * NMAX], x[KMAX], y[NMAX];
static int32_t ref[NMAX];
static int8_t Bpre[4][KMAX * NMAX];

static void prearrange(int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  for (int s = 0; s < S; s++)
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        for (int r = 0; r < h; r++)
          memcpy(&Bpre[s][((pp * ch + tt) * ks + r) * 16],
                 W + (size_t)(tt * h + r) * N + pp * w + s * DIM, 16);
}

static void load_b(int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int rows = (N / w) * (K / h) * ks;
  grt_config_ld(&CTX, 16);
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += DIM) {
      const int nb = rows - r0 > DIM ? DIM : rows - r0;
      grt_mvin(&CTX, &Bpre[s][r0 * 16], (uint32_t)(1 * SP_ENT + s * SP_ENT + r0), DIM, nb);
    }
}

#define A_BASE 1024
static void load_x(int sh) {
  const int h = DIM >> sh, ch = K / h;
  grt_config_ld(&CTX, h);
  grt_mvin_rows(&CTX, x, A_BASE, h, ch, h);
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s shape K N T [--neg]\n", argv[0]); return 2; }
  const int sh = atoi(argv[1]);
  K = atoi(argv[2]); N = atoi(argv[3]);
  const long T = atol(argv[4]);
  const int neg = argc > 5 && !strcmp(argv[5], "--neg");
  if (K > KMAX || N > NMAX) { fprintf(stderr, "K<=%d N<=%d\n", KMAX, NMAX); return 2; }
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
#endif
  const int S = 1 << sh, w = DIM * S, nt = N / w;
  const int rows = (N / w) * (K / (DIM >> sh)) * ((DIM >> sh) < 4 ? 4 : (DIM >> sh));
  if (rows > SP_ENT) { fprintf(stderr, "뱅크당 %d 행 > %d — stride 필요\n", rows, SP_ENT); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);

  for (int k = 0; k < K; k++) x[k] = (int8_t)((3 * k + 1) % 5 - 2);
  for (int k = 0; k < K; k++) for (int n = 0; n < N; n++)
    W[(size_t)k * N + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int n = 0; n < N; n++) {
    int32_t s = 0;
    for (int k = 0; k < K; k++) s += (int32_t)x[k] * W[(size_t)k * N + n];
    ref[n] = s > 127 ? 127 : s < -128 ? -128 : s;
  }
  if (neg) ref[N / 2] ^= 1;

  prearrange(sh);
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, N);
  load_b(sh); load_x(sh);
  grt_ss_gemv_config(&CTX, K, N, 1, sh, A_BASE, 0, 0);
  grt_fence();

  printf("shape=%d S=%d K=%d N=%d nt=%d rows/bank=%d\n", sh, S, K, N, nt, rows);
  for (int kc = 1; kc <= 8; kc *= 2) {
    if (nt % kc) { printf("  calls=%d: nt=%d 로 안 나눠짐 — 건너뜀\n", kc, nt); continue; }
    const int per = nt / kc;
    memset(y, 0, sizeof y);
    const uint64_t t0 = rdt();
    for (long it = 0; it < T; it++)
      for (int c = 0; c < kc; c++) grt_ss_gemv_blk(&CTX, y, c * per, per);
    grt_fence();
    const double cyc = (double)(rdt() - t0) * 100.0 / (double)T;
    int bad = 0;
    for (int n = 0; n < N; n++) if (y[n] != (int8_t)ref[n]) bad++;
    printf("  calls=%d (pp %d each)  %8.1f cyc  bad=%d %s\n", kc, per, cyc, bad, bad ? "FAIL" : "PASS");
  }
  return 0;
}
