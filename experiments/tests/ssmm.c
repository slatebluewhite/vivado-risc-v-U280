// SSGemm 실제 matmul (목표 (2), PREREG_E578 (B)): C[M][N] = A[M][K] * B[K][N] 를
// 세 shape 로 돌려 **정확성과 사이클**을 잰다. 지금까지의 보드 측정(ssgemv)은 전부
// M=1(GEMV)이었고, M>1 의 정확성은 실리콘에서 한 번도 확인된 적이 없다.
//
//   ./ssmm <shape> <M> <T> [--neg]
//     shape = 0(16x16) | 1(8x32) | 2(4x64)
//     M     = 타일-op 당 A 행 수 (1..16 — rows 필드가 5비트라 16 이 상한, E566)
//     T     = 반복 횟수
//
// K = N = 128 고정. A 와 B 를 **미리 scratchpad 에 상주**시키고 루프 안에는
// preload/compute + mvout 만 둔다 — DMA 가 아니라 EX 행 비용을 재는 것이 목적.
//
// 행 회계 (PREREG_E578 (B)): 타일-op 당 preload h 행 + compute M 행,
// 타일-op 수 = (N/w)*(K/h) = 64 (shape 무관). 접기 이득 예측 = (16+M)/(16/S+M).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
#ifdef SPB                      /* 컴파일 타임 고정판 — 발행 루프의 주소 계산을 접기 위해.
                                 * CLAUDE.md: "발행 루프는 비어 있어야 한다" (E205 계열) */
static const int SP_BANKS = SPB, SP_ENT = 16384 / SPB;
#else
static int SP_BANKS = 4, SP_ENT = 4096;
#endif
#define ACC_ENT 256

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

/* E601: K·N 을 인자로 — 규칙이 K=N=128 밖에서도 성립하는지 보려면 필요하다.
 * 정적 배열은 최대치로 잡고 실제 크기는 런타임에 쓴다. */
#define KMAX 256
#define NMAX 256
static int K = 128, N = 128;

static int8_t A[16 * KMAX], B[KMAX * NMAX], C[16 * NMAX];
static int32_t ref[16 * NMAX];

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s shape M T [--neg]\n", argv[0]); return 2; }
  const int sh = atoi(argv[1]), M = atoi(argv[2]);
  const long T = atol(argv[3]);
  const int neg = argc > 4 && !strcmp(argv[4], "--neg");
  /* E588: mvout 을 루프 밖으로. 매 반복이 같은 matmul 을 GRT_ACC(덮어쓰기)로 다시
   * 계산하므로 마지막 반복 뒤 한 번 저장하면 결과는 동일하다 — 정확성 검사 그대로
   * 유효하고, shape 무관한 store/DMA 몫이 시간에서 빠져 계산부만 남는다. */
  const int nostore = argc > 4 && !strcmp(argv[4], "--nostore");
  /* E589: FSM 경로(funct 23/24)로 같은 matmul 을 돌린다. FSM 은 이미 M 을 지원한다
   * (cfgM 이 preload 의 c_rows, compute 의 a_rows, aRow/accRow 주소에 쓰인다) —
   * E588 이 "없다"고 한 계측기가 실은 있었다. A 의 행 간격만 FSM 규약(tt 당 M 행)에
   * 맞춰 다시 깔면 된다. */
  const int fsm = argc > 4 && !strcmp(argv[4], "--fsm");
  if (argc > 5) K = atoi(argv[5]);
  if (argc > 6) N = atoi(argv[6]);
  if (K > KMAX || N > NMAX) { fprintf(stderr, "K,N <= %d\n", KMAX); return 2; }
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  if (M < 1 || M > DIM) { fprintf(stderr, "M은 1..%d (rows 필드 5비트)\n", DIM); return 2; }
  if (N % w || K % h) { fprintf(stderr, "K,N이 shape에 안 맞음\n"); return 2; }

  #ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e);
    SP_ENT = 16384 / SP_BANKS; }
#endif        /* 총 16384 행(256 KB/16 B)을 뱅크로 나눔 */
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
  if (neg) ref[(M / 2) * N + N / 2] ^= 1;   /* 음성 대조 */

  /* ---- B 를 세그먼트 배치로 상주 (타일 (pp,tt) 의 세그먼트 s -> 뱅크 (1+s)%4) ---- */
  /* E601: 뱅크 수 > 세그먼트 수이면 B 는 뱅크 0 으로 감기지 않으므로 bBase=0 이 안전하고
   * 뱅크 전체를 B 에 쓸 수 있다. 아니면 A(뱅크 0)와 겹치지 않게 절반 위로 띄운다. */
  const int aBase = 0, bBase = (SP_BANKS > S) ? 0 : SP_ENT / 2, accBase = 0;
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, N);
  grt_config_ld(&CTX, N);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, B + (size_t)(tt * h) * N + pp * w + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks), DIM, h);
  /* ---- A 를 K 청크별로 상주 (뱅크 0, 청크 tt 는 행 aBase + tt*DIM 부터 M 행) ---- */
  grt_config_ld(&CTX, K);
  /* A 의 행 간격: 소프트웨어 경로는 tt 당 DIM 행, FSM 은 tt 당 M 행(aRow = aBase+tt*M) */
  const int astride = fsm ? M : DIM;
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(aBase + tt * astride), h, M);
  grt_fence();
  if (fsm) grt_ss_gemv_config(&CTX, K, N, M, sh, aBase, bBase, accBase);

  uint64_t t0 = rdt();
  for (long it = 0; it < T; it++) {
    if (fsm) { grt_ss_gemv(&CTX, C); continue; }
    for (int pp = 0; pp < nt; pp++) {
      for (int tt = 0; tt < ch; tt++) {
        uint32_t bsp = 1 * SP_ENT + bBase + (pp * ch + tt) * ks;
        uint32_t cdst = (tt == 0 ? GRT_ACC(accBase + pp * M) : GRT_ACC_ACC(accBase + pp * M));
        grt_preload(&CTX, bsp, cdst, DIM, h, DIM, M);
        grt_compute(&CTX, (uint32_t)(aBase + tt * DIM), GRT_GARBAGE, h, M, 0, 0);
      }
      if (!nostore)
        for (int s = 0; s < S; s++)
          grt_mvout(&CTX, C + pp * w + s * DIM,
                    GRT_ACC(s * ACC_ENT + accBase + pp * M), DIM, M);
    }
  }
  if (nostore && !fsm)                           /* 마지막 결과를 한 번만 회수 */
    for (int pp = 0; pp < nt; pp++)
      for (int s = 0; s < S; s++)
        grt_mvout(&CTX, C + pp * w + s * DIM,
                  GRT_ACC(s * ACC_ENT + accBase + pp * M), DIM, M);
  grt_fence();
  uint64_t dt = rdt() - t0;

  int bad = 0;
  for (int i = 0; i < M; i++)
    for (int n = 0; n < N; n++)
      if (C[i * N + n] != (int8_t)ref[i * N + n] && bad++ < 5)
        printf("MISMATCH i=%d n=%d got=%d exp=%d\n", i, n, C[i * N + n], (int)ref[i * N + n]);
  const long rows = (long)nt * ch * (h + M);
  printf("mm%s shape=%d S=%d M=%d K=%d N=%d T=%ld  ticks=%llu  cyc/step=%.1f  rows/step=%ld  cyc/row=%.2f  %s\n",
         fsm ? "f" : "", sh, S, M, K, N, T, (unsigned long long)dt, (double)dt * 100.0 / (double)T,
         rows, (double)dt * 100.0 / (double)T / (double)rows, bad ? "FAIL" : "PASS");
  return bad != 0;
}
