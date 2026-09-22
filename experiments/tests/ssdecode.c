// E619: **decode 레이어 조립** — 지금까지의 단일 matmul 측정을 실제 워크로드로 묶는다.
//
// transformer 한 레이어의 decode 스텝(M=1)은 GEMV 여섯 개다:
//   Q, K, V, O  : [H x H]      FFN1: [H x 4H]      FFN2: [4H x H]
// attention 자체(QK^T, softmax, AV)는 H 에 비해 작고 가속기 대상이 아니므로 뺀다
// (corpus E120: "attention 은 레이어의 4 %, 나머지 96 % 가 projection·FFN").
//
//   ./ssdecode <shape> <T> [--neg]
//     shape 0|1|2, T = 반복 횟수. 환경변수 MODE=resident|stream (기본 stream)
//
// **E616 규칙**: shape 은 런타임 인자다 — 한 바이너리 안에서 비교해야 유효하다.
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
static int H = 128, FMUL = 4, STRIDE = 1;  /* E625: 세그먼트 뱅크 간격. STRIDE=0 이면 자동(E626) */
static int AUTO_STRIDE = 0;
#define HMAX 256
/* FFN 중간 폭. 실제 transformer 는 4H 지만, **4H 는 shape 0(접지 않음)에서 뱅크
 * 용량을 넘는다** (뱅크당 4096 행 필요 vs 2048) — 접기가 상주 가능한 가중치 크기를
 * 늘린다는 뜻이고 그 자체가 결과다(E619 에 기록). 세 shape 을 공정하게 비교하려면
 * 2H 로 낮춰야 한다. */
#define FF (4 * H)
#define FFMAX (4 * HMAX)
#define WMAX (HMAX * FFMAX)         /* 가장 큰 가중치 */

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

static int8_t W[WMAX], x[FFMAX], y[FFMAX];
static int32_t ref[FFMAX];
/* 세그먼트별 연속 배치. shape 0(S=1)은 **한 세그먼트가 K·N 바이트를 다 받으므로**
 * 세그먼트당 WMAX 가 필요하다 — 처음에 WMAX/16 으로 잡아 넘쳤고 FFN 단계가 조용히
 * 틀렸다(검사기가 잡았다). */
static int8_t Bpre[4][WMAX + 4096];

/* E626: 필요한 stride 를 계산한다. 뱅크당 필요 행이 SP_ENT 를 넘는 만큼 간격을 벌리되,
 * 상한은 sp_banks/S (그 이상은 세그먼트끼리 감겨 부딪힌다). 2 의 거듭제곱만 인코딩된다. */
static int calc_stride(int sh, int K, int N) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S, ks = h < 4 ? 4 : h;
  const int need = (N / w) * (K / h) * ks;      /* 뱅크당 필요 행 */
  const int cap = SP_BANKS / S;                 /* 간격 상한 */
  if (S == 1) return 1;                         /* 세그먼트가 하나면 간격은 무의미 — 넘침은 옆 뱅크로 */
  int st = 1;
  while ((long)st * SP_ENT < need && st < cap) st *= 2;
  return st;
}

/* 가중치를 shape 의 세그먼트 배치로 재배열 (오프라인 상당, E603) */
static void prearrange(int sh, int K, int N) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  for (int s = 0; s < S; s++)
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        for (int r = 0; r < h; r++)
          memcpy(&Bpre[s][((pp * ch + tt) * ks + r) * 16],
                 W + (size_t)(tt * h + r) * N + pp * w + s * DIM, 16);
}

/* E623: 열 조각 c (열 c*Nc .. c*Nc+Nc) 만 재배열한다. */
static void prearrange_col(int sh, int K, int N, int Nc, int c, int rowoff) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = Nc / w, ch = K / h;
  for (int s = 0; s < S; s++)
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        for (int r = 0; r < h; r++)
          memcpy(&Bpre[s][(rowoff + (pp * ch + tt) * ks + r) * 16],
                 W + (size_t)(tt * h + r) * N + c * Nc + pp * w + s * DIM, 16);
}

/* 재배열된 가중치를 scratchpad 로 (뱅크당 연속이므로 DIM 행씩). rowoff 는 Bpre 안의 조각 위치. */
static void load_w_off(int sh, int K, int N, int rowoff) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int rows = (N / w) * (K / h) * ks;
  grt_config_ld(&CTX, 16);
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += DIM) {
      const int nb = rows - r0 > DIM ? DIM : rows - r0;
      grt_mvin(&CTX, &Bpre[s][(rowoff + r0) * 16],
               (uint32_t)(((1 + s * STRIDE) % SP_BANKS) * SP_ENT + r0), DIM, nb);
    }
}

/* 활성값 x 를 뱅크 0 으로 (tile 당 1 행) */
static void load_w(int sh, int K, int N) { load_w_off(sh, K, N, 0); }

/* E625: stride 를 쓰면 마지막 세그먼트의 **넘침이 뱅크 0 하단을 덮는다**
 * (S=4·stride=2 → 세그먼트 3 은 뱅크 7, 넘침은 뱅크 0 의 행 0..). 그래서 활성값 A 를
 * 뱅크 0 의 **높은 행**으로 올린다. stride=1 이면 넘침이 없어 무해하다. */
#define A_BASE 1024
static void load_x(int sh, int K) {
  const int h = DIM >> sh, ch = K / h;
  grt_config_ld(&CTX, h);
  grt_mvin_rows(&CTX, x, A_BASE, h, ch, h);
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s shape T [--neg]\n", argv[0]); return 2; }
  const int sh = atoi(argv[1]);
  const long T = atol(argv[2]);
  const int neg = argc > 3 && !strcmp(argv[3], "--neg");
  { const char *e = getenv("HID"); if (e) H = atoi(e);
    const char *f = getenv("FFMUL"); if (f) FMUL = atoi(f);
    const char *g = getenv("STRIDE");
    if (g) { if (!strcmp(g, "auto") || !atoi(g)) AUTO_STRIDE = 1; else STRIDE = atoi(g); } }
  if (H > HMAX) { fprintf(stderr, "H <= %d\n", HMAX); return 2; }
  const char *mode = getenv("MODE");
  const int resident_all = mode && !strcmp(mode, "resident");
  const int auto_mode = mode && !strcmp(mode, "auto");   /* E620: 들어가면 상주, 아니면 스트리밍 */
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
#endif
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);

  /* 여섯 단계: (K, N, 이름) */
  const int stage_K[6] = { H, H, H, H, H,       FMUL*H };
  const int stage_N[6] = { H, H, H, H, FMUL*H,  H      };
  const char *nm[6] = { "Q", "K", "V", "O", "FFN1", "FFN2" };

  grt_config_st(&CTX, H);

  int bad_total = 0;
  double t_stage[6] = {0};
  for (int st = 0; st < 6; st++) {
    const int K = stage_K[st], N = stage_N[st];
    if (AUTO_STRIDE) STRIDE = calc_stride(sh, K, N);     /* E626: 단계마다 고른다 */
    { int sl = 0; while ((1 << sl) < STRIDE) sl++;
      grt_config_ex_shape_stride(&CTX, GRT_WS, sh, sl); }
    for (int k = 0; k < K; k++) x[k] = (int8_t)((3 * k + st) % 5 - 2);
    for (int k = 0; k < K; k++) for (int n = 0; n < N; n++)
      W[(size_t)k * N + n] = (int8_t)((k + 2 * n + st) % 7 - 3);
    for (int n = 0; n < N; n++) {
      int32_t s = 0;
      for (int k = 0; k < K; k++) s += (int32_t)x[k] * W[(size_t)k * N + n];
      ref[n] = s > 127 ? 127 : s < -128 ? -128 : s;
    }
    if (neg && st == 0) ref[N / 2] ^= 1;

    /* E623: 뱅크당 행 수가 한계를 넘으면 **N 을 쪼갠다**. 접힌 shape 은 세그먼트가
     * 뱅크에 갇혀 넘침을 이어 쓸 수 없다(E622). N 블록은 독립이라 누산이 필요 없다. */
    const int Sx0 = 1 << sh, hx0 = DIM >> sh, wx0 = DIM * Sx0, ksx0 = hx0 < 4 ? 4 : hx0;
    /* 분할 수는 **블록 수 N/w 의 약수**여야 한다 — 조각의 열 수가 w 의 배수가 아니면
     * 타일이 쪼개져 결과가 틀린다 (N=192, w=64 에서 2 등분하면 Nc=96, 96/64 = 1.5). */
    const int nt_all = N / wx0;
    int nsplit = 1;
    if (Sx0 > 1)                               /* S=1 은 옆 뱅크로 이어 쓸 수 있다 (E621) */
      while ((nt_all / nsplit) * (K / hx0) * ksx0 > SP_ENT * STRIDE || nt_all % nsplit != 0) {
        nsplit++;
        if (nsplit > nt_all) { nsplit = nt_all; break; }
      }
    const int Nc = (nt_all / nsplit) * wx0;    /* 조각 하나의 열 수 (w 의 배수) */
    grt_config_st(&CTX, N);                    /* 출력 stride 는 원래 N */
    const int rows_c = (Nc / wx0) * (K / hx0) * ksx0;   /* 조각 하나의 뱅크당 행 수 */
    for (int c = 0; c < nsplit; c++)                    /* 오프라인 재배열 (타이밍 밖) */
      prearrange_col(sh, K, N, Nc, c, c * rows_c);
    memset(y, 0, sizeof y);
    for (int c = 0; c < nsplit; c++) {
      load_w_off(sh, K, Nc, c * rows_c); load_x(sh, K);
      grt_ss_gemv_config(&CTX, K, Nc, 1, sh, A_BASE, 0, 0);
      grt_fence();
      grt_ss_gemv(&CTX, y + c * Nc);
    }
    grt_fence();
    int bad = 0;
    for (int n = 0; n < N; n++) if (y[n] != (int8_t)ref[n]) bad++;
    bad_total += bad;

    /* E620: 이 단계의 가중치가 scratchpad 에 들어가는가 (뱅크당 행 수 기준) */
    const int stream_this = resident_all ? 0 : (auto_mode ? 0 : 1);
    const uint64_t t0 = rdt();
    for (long i = 0; i < T; i++)
      for (int c = 0; c < nsplit; c++) {
        if (stream_this || nsplit > 1) {        /* 분할이면 조각마다 가중치를 갈아끼운다 */
          load_w_off(sh, K, Nc, c * rows_c); load_x(sh, K);
          grt_ss_gemv_config(&CTX, K, Nc, 1, sh, A_BASE, 0, 0);
        }
        grt_ss_gemv(&CTX, y + c * Nc);
      }
    grt_fence();
    t_stage[st] = (double)(rdt() - t0) * 100.0 / (double)T;
    printf("  %-4s K=%4d N=%4d st=%d  %8.1f cyc  %s  bad=%d %s\n", nm[st], K, N, STRIDE, t_stage[st],
           stream_this ? "stream  " : nsplit > 1 ? "split   " : "resident", bad, bad ? "FAIL" : "PASS");
  }
  double tot = 0;
  for (int st = 0; st < 6; st++) tot += t_stage[st];
  printf("decode layer shape=%d mode=%s  %.1f cyc/token  (%.1f us @31.25MHz)  %s\n",
         sh, resident_all ? "resident" : auto_mode ? "auto" : "stream", tot, tot / 31.25,
         bad_total ? "FAIL" : "PASS");
  return bad_total != 0;
}
