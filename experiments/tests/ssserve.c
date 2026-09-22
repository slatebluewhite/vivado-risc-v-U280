// E628: 서빙 워크로드의 shape 전환 정책. prefill(M=16)은 S=1 을, decode(M=1)은 S=4 를
// 원하는데(E627), 전환은 B 를 새 세그먼트 배치로 다시 깔아야 한다(E600, ~3500 cyc).
// 네 정책을 **한 바이너리 안에서 런타임 인자로** 비교한다 (E626 규칙).
//
//   ./ssserve <K> <N> <P> <D> <policy> <T> [--neg]
//     P = prefill 청크 수 (청크당 M=16), D = decode 스텝 수 (M=1), T = 반복
//     policy 0=전부 S=1  1=전부 S=4  2=적응형(전환시 B 재적재)  3=적응형 이중상주
//
// 배치 (SP_BANKS=8, SP_ENT=2048, K=N=128):
//   S=1 의 B: 뱅크 1 부터 1024 행 (bBase=0)
//   S=4 의 B: 뱅크 1..4 의 행 1024.. 각 256 행 (bBase=BB4)  -> 겹치지 않는다
//   A: 뱅크 0. shape 과 M 마다 ch·M 행이 필요하므로 네 조합을 각각 다른 aBase 에 상주.
// 시간 측정에는 gemv 호출과 **전환 비용**만 넣는다 (A 는 전부 미리 상주 — 정책 간
// 동일하므로 비교를 왜곡하지 않는다).
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
#define NMAX 256
static int K = 128, N = 128;

static int8_t W[KMAX * NMAX];
static int8_t Xp[16 * KMAX], xd[KMAX];
static int8_t Cp[16 * NMAX], Cd[NMAX];
static int32_t refp[16 * NMAX], refd[NMAX];
/* 두 shape 의 세그먼트 배치를 따로 보관 (오프라인 재배열 상당) */
static int8_t Bpre[2][4][KMAX * NMAX];

/* B 를 shape sh 의 세그먼트 배치로 재배열. slot 0 = S=1, slot 1 = S=4 */
static void prearrange(int slot, int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  for (int s = 0; s < S; s++)
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        for (int r = 0; r < h; r++)
          memcpy(&Bpre[slot][s][((pp * ch + tt) * ks + r) * 16],
                 W + (size_t)(tt * h + r) * N + pp * w + s * DIM, 16);
}

/* 재배열된 B 를 scratchpad 로. bBase 는 뱅크 안의 행 오프셋. */
static void load_b(int slot, int sh, int bBase) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int rows = (N / w) * (K / h) * ks;
  grt_config_ld(&CTX, 16);
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += DIM) {
      const int nb = rows - r0 > DIM ? DIM : rows - r0;
      grt_mvin(&CTX, &Bpre[slot][s][r0 * 16],
               (uint32_t)(1 * SP_ENT + bBase + s * SP_ENT + r0), DIM, nb);
    }
}

/* A 를 뱅크 0 의 aBase 부터. FSM 규약: 청크 tt 는 행 aBase + tt*M 부터 M 행. */
static void load_a(const int8_t *src, int sh, int M, int aBase) {
  const int h = DIM >> sh, ch = K / h;
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, src + tt * h, (uint32_t)(aBase + tt * M), h, M);
}

int main(int argc, char **argv) {
  if (argc < 7) { fprintf(stderr, "usage: %s K N P D policy T [--neg]\n", argv[0]); return 2; }
  K = atoi(argv[1]); N = atoi(argv[2]);
  const int P = atoi(argv[3]), D = atoi(argv[4]), pol = atoi(argv[5]);
  const long T = atol(argv[6]);
  const int neg = argc > 7 && !strcmp(argv[7], "--neg");
  if (K > KMAX || N > NMAX) { fprintf(stderr, "K,N <= %d\n", KMAX); return 2; }
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
#endif
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);

  for (int i = 0; i < 16; i++) for (int k = 0; k < K; k++) Xp[i * K + k] = (int8_t)((i + 3 * k) % 5 - 2);
  for (int k = 0; k < K; k++) xd[k] = (int8_t)((5 * k + 1) % 7 - 3);
  for (int k = 0; k < K; k++) for (int n = 0; n < N; n++)
    W[(size_t)k * N + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int i = 0; i < 16; i++)
    for (int n = 0; n < N; n++) {
      int32_t s = 0;
      for (int k = 0; k < K; k++) s += (int32_t)Xp[i * K + k] * W[(size_t)k * N + n];
      refp[i * N + n] = s > 127 ? 127 : s < -128 ? -128 : s;
    }
  for (int n = 0; n < N; n++) {
    int32_t s = 0;
    for (int k = 0; k < K; k++) s += (int32_t)xd[k] * W[(size_t)k * N + n];
    refd[n] = s > 127 ? 127 : s < -128 ? -128 : s;
  }
  if (neg) refd[N / 2] ^= 1;

  prearrange(0, 0);   /* S=1 배치 */
  prearrange(1, 2);   /* S=4 배치 */

  /* E629: S=1 은 뱅크 1 부터 평탄하게 rows1 행. 접힌 배치는 그 **다음 뱅크 경계**부터
   * 시작하게 bBase 를 준다 — bRow 가 평탄 인덱스이므로 기준 뱅크가 그만큼 옮겨간다. */
  const int rows1 = (N / 16) * (K / 16) * 16;
  const int rows4 = (N / 64) * (K / 4) * 4;                   /* 뱅크당 */
  const int BB4 = ((rows1 + SP_ENT - 1) / SP_ENT) * SP_ENT;
  const int bank4 = 1 + BB4 / SP_ENT;                          /* 접힌 배치의 기준 뱅크 */
  if (bank4 + 3 > SP_BANKS - 1 || rows4 > SP_ENT) {
    fprintf(stderr, "이중상주 공간 부족 (기준뱅크 %d, 뱅크당 %d 행)\n", bank4, rows4); return 2; }

  grt_config_st(&CTX, N);   /* 출력 stride — 빠뜨리면 prefill 이 흩어진다 */

  /* A 네 조합을 미리 상주: (shape, M) = (0,16) (0,1) (2,16) (2,1) */
  const int aP1 = 0, aD1 = aP1 + K, aP4 = aD1 + K / 16, aD4 = aP4 + 4 * K;
  if (aD4 + K / 4 > SP_ENT) { fprintf(stderr, "A 자리 부족\n"); return 2; }
  load_a(Xp, 0, 16, aP1); load_a(xd, 0, 1, aD1);
  load_a(Xp, 2, 16, aP4); load_a(xd, 2, 1, aD4);
  grt_fence();

  /* 초기 B 적재 (타이밍 밖) */
  if (pol == 0)      { grt_config_ex_shape(&CTX, GRT_WS, 0); load_b(0, 0, 0); }
  else if (pol == 1) { grt_config_ex_shape(&CTX, GRT_WS, 2); load_b(1, 2, 0); }
  else if (pol == 2) { grt_config_ex_shape(&CTX, GRT_WS, 0); load_b(0, 0, 0); }
  else               { grt_config_ex_shape(&CTX, GRT_WS, 0); load_b(0, 0, 0);
                       grt_config_ex_shape(&CTX, GRT_WS, 2); load_b(1, 2, BB4); }
  grt_fence();

  const uint64_t t0 = rdt();
  for (long it = 0; it < T; it++) {
    /* ---- prefill: M=16 을 P 청크 ---- */
    const int psh = (pol == 1) ? 2 : 0;
    const int pab = (pol == 1) ? aP4 : aP1;
    const int pbb = (pol == 1) ? 0 : 0;
    grt_config_ex_shape(&CTX, GRT_WS, psh);
    grt_ss_gemv_config(&CTX, K, N, 16, psh, pab, pbb, 0);
    for (int p = 0; p < P; p++) grt_ss_gemv(&CTX, Cp);

    /* ---- 전환 ---- */
    if (pol == 2) {                 /* B 를 S=4 배치로 다시 깐다 (이것이 전환 비용) */
      grt_config_ex_shape(&CTX, GRT_WS, 2);
      load_b(1, 2, 0);
    } else if (pol == 3) {          /* 이미 깔려 있다 — config 만 */
      grt_config_ex_shape(&CTX, GRT_WS, 2);
    }

    /* ---- decode: M=1 을 D 스텝 ---- */
    const int dsh = (pol == 0) ? 0 : 2;
    const int dab = (pol == 0) ? aD1 : aD4;
    const int dbb = (pol == 3) ? BB4 : 0;
    grt_config_ex_shape(&CTX, GRT_WS, dsh);
    grt_ss_gemv_config(&CTX, K, N, 1, dsh, dab, dbb, 0);
    for (int d = 0; d < D; d++) grt_ss_gemv(&CTX, Cd);

    /* ---- 다음 반복을 위해 prefill 배치로 되돌린다 (정책 2 만 실제 비용) ---- */
    if (pol == 2) { grt_config_ex_shape(&CTX, GRT_WS, 0); load_b(0, 0, 0); }
  }
  grt_fence();
  const double cyc = (double)(rdt() - t0) * 100.0 / (double)T;

  int badp = 0, badd = 0;
  for (int i = 0; i < 16; i++) for (int n = 0; n < N; n++)
    if (Cp[i * N + n] != (int8_t)refp[i * N + n]) badp++;
  for (int n = 0; n < N; n++) if (Cd[n] != (int8_t)refd[n]) badd++;
  printf("serve K=%d N=%d P=%d D=%d pol=%d  %.1f cyc  badp=%d badd=%d %s\n",
         K, N, P, D, pol, cyc, badp, badd, (badp || badd) ? "FAIL" : "PASS");
  return (badp || badd) != 0;
}
