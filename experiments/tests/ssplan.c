// E638: `grt_ss_plan` 이 고른 shape 이 실제로 가장 빠른가를 **새 셀**에서 확인한다.
// 규칙을 뽑아낸 격자(K=N=128/256, E627)로 채점하면 순환이므로, 종횡비가 다른 셀을 쓴다.
//
//   ./ssplan <M> <K> <N> <T> [--neg]
//
// 세 shape 을 모두 돌리되 각각 계획기가 준 stride 를 쓰고, 결과를 CPU 참값과 대조한다.
// 계획기의 선택이 최속인지, 아니면 몇 % 손해인지 출력한다.
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

/* E837c: 이 비트스트림이 지원하는 최대 shape (0/1/2). `max_segments=2` 보드에 shape 2 를
 * 발행하면 aliasing 되어 조용히 틀리므로(E587), 계획기와 스윕 양쪽을 여기에 맞춘다. */
static int max_sh = 2;

#define KMAX 512
#define NMAX 512
static int K = 128, N = 128, M = 1;

static int8_t W[KMAX * NMAX], A[16 * KMAX], C[16 * NMAX];
static int32_t ref[16 * NMAX];
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

/* 세그먼트 s 는 뱅크 (1 + s*stride) mod SP_BANKS. 넘침은 다음 뱅크로 이어진다 (E624). */
static void load_b(int sh, int stride) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int rows = (N / w) * (K / h) * ks;
  grt_config_ld(&CTX, 16);
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += DIM) {
      const int nb = rows - r0 > DIM ? DIM : rows - r0;
      grt_mvin(&CTX, &Bpre[s][r0 * 16],
               (uint32_t)(((1 + s * stride) % SP_BANKS) * SP_ENT + r0), DIM, nb);
    }
}

/* A: 뱅크 0. FSM 규약으로 청크 tt 당 M 행이므로 발자국은 **ch·M 행**이다.
 * E639: 이것을 고정 오프셋으로 두면 K 와 M 이 크면 뱅크 0 을 넘어 B(뱅크 1)를 덮고
 * **보드가 wedge 된다**. 뱅크 위쪽에 붙여 놓고, 안 들어가면 그 셀을 건너뛴다.
 * 세그먼트 넘침은 뱅크의 **낮은** 행으로 들어오므로 위쪽이 안전하다 (E625). */
static int a_base_for(int sh) {
  grt_ss_plan_t q;                    /* E640: 계획기의 넘침/A 검사를 그대로 쓴다 */
  grt_ss_plan_hw(&q, M, K, N, SP_BANKS, SP_ENT, 0, max_sh);
  (void)sh;
  return q.a_base;
}
static int a_base_for_shape(int sh) { /* shape 을 강제해 같은 검사를 돌린다 */
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int need = (N / w) * (K / h) * ks;
  const int cap = (S > 1) ? SP_BANKS / S : 1;
  int st = 1; while ((long)st * SP_ENT < need && st < cap) st *= 2;
  if ((long)need > (long)st * SP_ENT) return -1;
  const int nb = (need + SP_ENT - 1) / SP_ENT;
  int hits0 = 0;
  for (int sg = 0; sg < S; sg++) for (int j = 0; j < nb; j++)
    if (((1 + sg * st + j) % SP_BANKS) == 0) hits0 = 1;
  const int spill = (hits0 && nb > 1) ? (need - (nb - 1) * SP_ENT) : 0;
  const int rows = (K / h) * M, base = SP_ENT - rows;
  return (rows > SP_ENT || base < spill) ? -1 : base;
}
static void load_a(int sh, int abase) {
  const int h = DIM >> sh, ch = K / h;
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(abase + tt * M), h, M);
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s M K N T [--neg]\n", argv[0]); return 2; }
  M = atoi(argv[1]); K = atoi(argv[2]); N = atoi(argv[3]);
  const long T = atol(argv[4]);
  const int neg = argc > 5 && !strcmp(argv[5], "--neg");
  if (K > KMAX || N > NMAX || M < 1 || M > 16) { fprintf(stderr, "범위 초과\n"); return 2; }
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
#endif
  { const char *e = getenv("SS_MAX_SH");
    if (e) { max_sh = atoi(e); if (max_sh < 0) max_sh = 0; if (max_sh > 2) max_sh = 2; } }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);

  for (int i = 0; i < M; i++) for (int k = 0; k < K; k++) A[i * K + k] = (int8_t)((i + 3 * k) % 5 - 2);
  for (int k = 0; k < K; k++) for (int n = 0; n < N; n++)
    W[(size_t)k * N + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int i = 0; i < M; i++)
    for (int n = 0; n < N; n++) {
      int32_t s = 0;
      for (int k = 0; k < K; k++) s += (int32_t)A[i * K + k] * W[(size_t)k * N + n];
      ref[i * N + n] = s > 127 ? 127 : s < -128 ? -128 : s;
    }
  if (neg) ref[(M / 2) * N + N / 2] ^= 1;

  grt_ss_plan_t plan;
  grt_ss_plan_hw(&plan, M, K, N, SP_BANKS, SP_ENT, 0, max_sh);
  printf("plan M=%d K=%d N=%d -> shape=%d (S=%d) strideLog=%d nsplit=%d rows/bank=%d\n",
         M, K, N, plan.shape, plan.segs, plan.stride_log, plan.nsplit, plan.rows_per_bank);
  if (plan.nsplit > 1) { printf("  (분할 필요 — 이 시험은 상주 셀만 다룬다)\n"); return 3; }

  double t[3]; int bad[3];
  const char *only = getenv("ONLY_SHAPE");   /* E643: wedge 를 shape 단위로 가르기 위해 */
  for (int sh = 0; sh <= 2; sh++) {
    if (only && atoi(only) != sh) { t[sh] = -1; bad[sh] = 0; continue; }
    if (sh > max_sh) { t[sh] = -1; bad[sh] = 0;
      printf("  shape%d: 이 보드가 지원하지 않음 (SS_MAX_SH=%d) — 건너뜀\n", sh, max_sh); continue; }
    const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
    if (N % w || K % h) { t[sh] = -1; bad[sh] = 0; printf("  shape%d: 나눠떨어지지 않음\n", sh); continue; }
    grt_ss_plan_t q; { const int save = plan.shape; (void)save; }
    /* 이 shape 에 맞는 stride 를 계획기 규칙으로 다시 계산 */
    const int need = (N / w) * (K / h) * ks;
    const int cap = (S > 1) ? SP_BANKS / S : 1;
    int st = 1; while ((long)st * SP_ENT < need && st < cap) st *= 2;
    if ((long)need > (long)st * SP_ENT) { t[sh] = -1; bad[sh] = 0; printf("  shape%d: 상주 불가 (%d 행/뱅크)\n", sh, need); continue; }
    int sl = 0; while ((1 << sl) < st) sl++;
    (void)q;
    const int abase = a_base_for_shape(sh);
    if (abase < 0) { t[sh] = -1; bad[sh] = 0; printf("  shape%d: A 자리 없음 (A %d 행, 넘침과 충돌)\n", sh, (K / h) * M); continue; }
    prearrange(sh);
    grt_config_ex_shape_stride(&CTX, GRT_WS, sh, sl);
    grt_config_st(&CTX, N);
    load_b(sh, st); load_a(sh, abase);
    grt_ss_gemv_config(&CTX, K, N, M, sh, abase, 0, 0);
    grt_fence();
    memset(C, 0, sizeof C);
    const uint64_t t0 = rdt();
    for (long it = 0; it < T; it++) grt_ss_gemv(&CTX, C);
    grt_fence();
    t[sh] = (double)(rdt() - t0) * 100.0 / (double)T;
    bad[sh] = 0;
    for (int i = 0; i < M; i++) for (int n = 0; n < N; n++)
      if (C[i * N + n] != (int8_t)ref[i * N + n]) bad[sh]++;
    printf("  shape%d S=%d st=%d  %8.1f cyc  bad=%d %s\n", sh, S, st, t[sh], bad[sh], bad[sh] ? "FAIL" : "PASS");
  }
  int best = -1;
  for (int sh = 0; sh <= 2; sh++) if (t[sh] > 0 && (best < 0 || t[sh] < t[best])) best = sh;
  const double picked = t[plan.shape] > 0 ? t[plan.shape] : -1;
  printf("PLAN M=%d K=%d N=%d picked=%d best=%d loss=%.2f%% %s\n", M, K, N, plan.shape, best,
         (picked > 0 && best >= 0) ? (picked / t[best] - 1) * 100 : -1.0,
         (bad[0] || bad[1] || bad[2]) ? "FAIL" : "PASS");
  return 0;
}
