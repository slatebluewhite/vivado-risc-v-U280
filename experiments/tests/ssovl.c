// E608: 적재·계산 중첩이 **뱅크가 다르면** 일어나는가 — 하드웨어 가설의 결정적 대조.
//
// E606 은 이중 버퍼가 안 먹는 것을 봤고 원인을 `sp_singleported = true` 로 지목했다
// (같은 뱅크에서 DMA 쓰기가 EX 읽기를 막는다). E607 은 그 대조를 FSM 으로 하려다
// **FSM 이 B 의 뱅크를 1 로 하드코딩**해서 막혔다.
//
// 여기서는 **소프트웨어 타일 경로**로 계산한다 — 주소를 내가 정하므로 뱅크를 고를 수 있다.
// 계산은 항상 뱅크 1,2 (S=2 배치, 행 0..)를 읽고, 그와 동시에 더미 적재를
//   variant 0: 뱅크 1,2 의 **다른 행** (같은 뱅크)
//   variant 1: 뱅크 3,4          (다른 뱅크)
// 로 보낸다. 두 경우의 시간 차이가 곧 포트 경합의 크기다. 결과는 양쪽 다 검사한다.
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
static int SP_BANKS = 4, SP_ENT = 4096;
#endif
#define ACC_ENT 256
#define K 128
#define N 128
#define SH 1                      /* S=2: 뱅크 1,2 를 쓰므로 3,4 가 비어 있다 */

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

static int8_t A[16 * K], B[K * N], C[16 * N];
static int32_t ref[16 * N];

int main(int argc, char **argv) {
  const int M = argc > 1 ? atoi(argv[1]) : 1;
  const long T = argc > 2 ? atol(argv[2]) : 300;
  const int neg = argc > 3 && !strcmp(argv[3], "--neg");
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
#endif
  const int S = 1 << SH, h = DIM >> SH, w = DIM * S, ks = h;
  const int nt = N / w, ch = K / h;
  const int rows = nt * ch * ks;
  if (SP_BANKS < 5) { fprintf(stderr, "이 실험은 뱅크 5개 이상 필요\n"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);

  for (int i = 0; i < M; i++) for (int k = 0; k < K; k++) A[i * K + k] = (int8_t)((i + 3 * k) % 5 - 2);
  for (int k = 0; k < K; k++) for (int n = 0; n < N; n++) B[k * N + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int i = 0; i < M; i++)
    for (int n = 0; n < N; n++) {
      int32_t s = 0;
      for (int k = 0; k < K; k++) s += (int32_t)A[i * K + k] * B[k * N + n];
      ref[i * N + n] = s > 127 ? 127 : s < -128 ? -128 : s;
    }
  if (neg) ref[(M / 2) * N + N / 2] ^= 1;

  grt_config_ex_shape(&CTX, GRT_WS, SH);
  grt_config_st(&CTX, N);
  grt_config_ld(&CTX, N);
  for (int pp = 0; pp < nt; pp++)                       /* 계산용 B: 뱅크 1,2 행 0.. */
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, B + (size_t)(tt * h) * N + pp * w + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + (pp * ch + tt) * ks), DIM, h);
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(tt * DIM), h, M);
  grt_fence();

  double t_alone = 0;
  for (int variant = 0; variant < 6; variant++) {
    /* variant 0: 계산만  1: 같은 뱅크로 더미 적재  2: 다른 뱅크로 더미 적재 */
    const int dummy_bank = variant == 1 ? 1 : 3;
    const int dummy_row  = variant == 1 ? rows : 0;
    memset(C, 0, sizeof C);
    uint64_t t0 = rdt();
    for (long it = 0; it < T; it++) {
      if (variant >= 4) {           /* E609: 더 잘게 — G 타일마다 적재와 계산을 번갈아 */
        const int G = variant == 4 ? 4 : 1;
        grt_config_ld(&CTX, N);
        for (int pp = 0; pp < nt; pp++) {
          for (int t0i = 0; t0i < ch; t0i += G) {
            const int te = t0i + G > ch ? ch : t0i + G;
            for (int tt = t0i; tt < te; tt++)          /* 이 묶음의 더미 적재 */
              for (int s = 0; s < S; s++)
                grt_mvin(&CTX, B + (size_t)(tt * h) * N + pp * w + s * DIM,
                         (uint32_t)(((3 + s) % SP_BANKS) * SP_ENT + (pp * ch + tt) * ks), DIM, h);
            for (int tt = t0i; tt < te; tt++) {        /* 이 묶음의 계산 */
              const uint32_t bsp = 1 * SP_ENT + (pp * ch + tt) * ks;
              const uint32_t cdst = (tt == 0 ? GRT_ACC(pp * M) : GRT_ACC_ACC(pp * M));
              grt_preload(&CTX, bsp, cdst, DIM, h, DIM, M);
              grt_compute(&CTX, (uint32_t)(tt * DIM), GRT_GARBAGE, h, M, 0, 0);
            }
          }
          for (int s = 0; s < S; s++)
            grt_mvout(&CTX, C + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp * M), DIM, M);
        }
        continue;
      }
      if (variant == 3) {           /* E608b: 적재와 계산을 pp 단위로 **섞어** 발행한다.
                                     * RS 엔트리가 적재로만 차서 계산이 못 들어가는 것이
                                     * 원인이라면 여기서 겹침이 나타난다. */
        grt_config_ld(&CTX, N);
        for (int pp = 0; pp < nt; pp++) {
          for (int tt = 0; tt < ch; tt++)               /* 이 pp 몫의 더미 적재 (다른 뱅크) */
            for (int s = 0; s < S; s++)
              grt_mvin(&CTX, B + (size_t)(tt * h) * N + pp * w + s * DIM,
                       (uint32_t)(((3 + s) % SP_BANKS) * SP_ENT + (pp * ch + tt) * ks), DIM, h);
          for (int tt = 0; tt < ch; tt++) {             /* 이 pp 몫의 계산 */
            const uint32_t bsp = 1 * SP_ENT + (pp * ch + tt) * ks;
            const uint32_t cdst = (tt == 0 ? GRT_ACC(pp * M) : GRT_ACC_ACC(pp * M));
            grt_preload(&CTX, bsp, cdst, DIM, h, DIM, M);
            grt_compute(&CTX, (uint32_t)(tt * DIM), GRT_GARBAGE, h, M, 0, 0);
          }
          for (int s = 0; s < S; s++)
            grt_mvout(&CTX, C + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp * M), DIM, M);
        }
        continue;
      }
      if (variant) {                                    /* 더미 적재 (계산과 겹칠 후보) */
        grt_config_ld(&CTX, N);
        for (int pp = 0; pp < nt; pp++)
          for (int tt = 0; tt < ch; tt++)
            for (int s = 0; s < S; s++)
              grt_mvin(&CTX, B + (size_t)(tt * h) * N + pp * w + s * DIM,
                       (uint32_t)(((dummy_bank + s) % SP_BANKS) * SP_ENT + dummy_row + (pp * ch + tt) * ks),
                       DIM, h);
      }
      for (int pp = 0; pp < nt; pp++) {                 /* 계산: 항상 뱅크 1,2 행 0.. */
        for (int tt = 0; tt < ch; tt++) {
          const uint32_t bsp = 1 * SP_ENT + (pp * ch + tt) * ks;
          const uint32_t cdst = (tt == 0 ? GRT_ACC(pp * M) : GRT_ACC_ACC(pp * M));
          grt_preload(&CTX, bsp, cdst, DIM, h, DIM, M);
          grt_compute(&CTX, (uint32_t)(tt * DIM), GRT_GARBAGE, h, M, 0, 0);
        }
        for (int s = 0; s < S; s++)
          grt_mvout(&CTX, C + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp * M), DIM, M);
      }
    }
    grt_fence();
    const double t = (double)(rdt() - t0) * 100.0 / (double)T;
    int bad = 0;
    for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
      if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    if (!variant) t_alone = t;
    printf("ovl variant=%d (%s)  %.1f cyc/step  적재비용 %+.1f  bad=%d %s\n", variant,
           variant == 0 ? "계산만" : variant == 1 ? "같은뱅크" : variant == 2 ? "다른뱅크" : variant == 3 ? "pp단위" : variant == 4 ? "4타일" : "1타일",
           t, t - t_alone, bad, bad ? "FAIL" : "PASS");
  }
  return 0;
}
