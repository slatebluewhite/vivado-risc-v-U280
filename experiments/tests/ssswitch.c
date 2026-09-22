// SSGemm 런타임 shape 전환 비용 (목표 (3) 의 실전 가치).
//
// 보드 측정은 지금까지 "한 프로세스 = 한 shape" 이었다. shape ISA 의 존재 이유는
// **실행 중에 접기를 바꾸는 것**이므로, 한 프로세스 안에서 shape 를 바꿔가며 돌려
//   ① 정확성이 유지되는가 (전환 후 상태가 새지 않는가)
//   ② 전환이 얼마나 비싼가
// 를 잰다.
//
//   ./ssswitch <M> <T> [--neg]
//
// 전환 비용의 실체는 **B 의 재배치**다: 세그먼트 배치가 shape 마다 다르므로 B 를 새로
// 깔아야 한다 (config_ex 자체는 명령 하나). 그래서 두 가지를 따로 잰다:
//   switch_full = config_ex + B refill + FSM config   (실제로 드는 것)
//   switch_cfg  = config_ex + FSM config 만            (B 가 이미 맞는 배치일 때)
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
#define KMAX 256
#define NMAX 256
#define KMAX_ROWS_MAX 2048
static int K = 128, N = 128;

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static inline uint64_t rdt(void) { uint64_t t; asm volatile("rdtime %0" : "=r"(t)); return t; }

static int8_t A[16 * KMAX], B[KMAX * NMAX], C[16 * NMAX];
static int32_t ref[16 * NMAX];

/* E603: 가중치를 **오프라인에서 접힌 배치로 재배열**해 둔 판. 뱅크 b 가 받을 데이터를
 * DRAM 에서 연속으로 모아두면 mvin 하나가 DIM 행까지 실을 수 있어 명령 수가 shape 와
 * 무관해진다 (E602 의 4 배 페널티의 원인 제거). 실제 서빙의 표준 관행이다. */
/* E617: 같은 바이너리 안에서 **버퍼의 물리 페이지 배치**만 바꿔 본다. E616 의 2.4 배가
 * 페이지 aliasing 때문이라면 오프셋에 따라 성능이 크게 달라져야 한다. 뒤에 32 페이지
 * 여유를 두고 런타임에 `PGOFF` 페이지만큼 밀어서 쓴다. */
static int8_t BpreRaw[4][KMAX_ROWS_MAX * 16 + 32 * 4096];
static int8_t *Bpre[4];
static void bpre_init(int pgoff) {
  for (int s = 0; s < 4; s++) Bpre[s] = BpreRaw[s] + (size_t)pgoff * 4096;
}

static void prearrange(int sh, int bBase) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  for (int s = 0; s < S; s++)
    for (int pp = 0; pp < nt; pp++)
      for (int tt = 0; tt < ch; tt++)
        for (int r = 0; r < h; r++)      /* sp 행 (pp*ch+tt)*ks + r 에 해당하는 16 바이트 */
          memcpy(&Bpre[s][((pp * ch + tt) * ks + r) * 16],
                 B + (size_t)(tt * h + r) * N + pp * w + s * DIM, 16);
}

/* E604: **wide mvin** — `block_mvin_stride = DIM` 이므로 cols=DIM*4 인 mvin 하나가
 * sp 행 r0 + b*16 + i (b=0..3, i=0..15) 즉 **64 행**을 채운다. DRAM 쪽은 한 행이
 * 64 바이트이고 그 안에 블록 4 개가 나란히 있어야 하므로, 그렇게 인터리브해 재배열한다.
 * corpus(E205): "mvin 은 폭과 무관하게 명령당 비용이 같다 — MAX_BLOCK_LEN 만큼 실어라". */
static int8_t Bwide[4][KMAX_ROWS_MAX * 16];

static void prearrange_wide(int sh) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  const int rows = nt * ch * ks;
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += 64)
      for (int i = 0; i < 16; i++)
        for (int b = 0; b < 4; b++) {
          const int sp = r0 + b * 16 + i;                 /* 이 명령이 채울 sp 행 */
          int8_t *dst = &Bwide[s][(r0 + i * 4 + b) * 16]; /* DRAM 상의 자리 */
          if (sp >= rows) { memset(dst, 0, 16); continue; }
          const int t = sp / ks, r = sp % ks;             /* sp 행 -> (타일, 타일 내 행) */
          const int pp = t / ch, tt = t % ch;
          if (r < h) memcpy(dst, B + (size_t)(tt * h + r) * N + pp * w + s * DIM, 16);
          else memset(dst, 0, 16);
        }
}

static void load_wide(int sh, int M, int bBase) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  const int rows = nt * ch * ks;
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, N);
  grt_config_ld(&CTX, 64);                    /* DRAM 행 = 64 B (블록 4 개) */
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += 64)
      grt_mvin(&CTX, &Bwide[s][r0 * 16],
               (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + r0), 64, 16);
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(tt * M), h, M);
  grt_ss_gemv_config(&CTX, K, N, M, sh, 0, bBase, 0);
}

/* 재배열된 가중치로 적재: 뱅크당 연속이므로 DIM 행씩 묶어 싣는다. */
static void load_pre(int sh, int M, int bBase) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  const int rows = nt * ch * ks;
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, N);
  grt_config_ld(&CTX, 16);                  /* DRAM 행 간격 = 16 B (연속) */
  for (int s = 0; s < S; s++)
    for (int r0 = 0; r0 < rows; r0 += DIM) {
      const int nb = rows - r0 > DIM ? DIM : rows - r0;
      grt_mvin(&CTX, &Bpre[s][r0 * 16],
               (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + r0), DIM, nb);
    }
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(tt * M), h, M);
  grt_ss_gemv_config(&CTX, K, N, M, sh, 0, bBase, 0);
}

/* E611: pp 블록 하나 몫의 가중치만 적재한다 (재배열된 Bpre 에서). */
static void load_pre_blk(int sh, int bBase, int pp) {
  const int S = 1 << sh, h = DIM >> sh;
  const int ks = h < 4 ? 4 : h;
  const int ch = K / h;
  const int r0 = pp * ch * ks, rows = ch * ks;
  grt_config_ld(&CTX, 16);
  for (int s = 0; s < S; s++)
    for (int r = 0; r < rows; r += DIM) {
      const int nb = rows - r > DIM ? DIM : rows - r;
      grt_mvin(&CTX, &Bpre[s][(r0 + r) * 16],
               (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + r0 + r), DIM, nb);
    }
}

/* shape sh 로 B 를 세그먼트 배치에 깔고 FSM 을 설정한다. A 는 shape 무관(뱅크 0). */
static void setup_shape(int sh, int M, int bBase) {
  const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
  const int ks = h < 4 ? 4 : h;
  const int nt = N / w, ch = K / h;
  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, N);
  grt_config_ld(&CTX, N);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, B + (size_t)(tt * h) * N + pp * w + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks), DIM, h);
  grt_config_ld(&CTX, K);
  for (int tt = 0; tt < ch; tt++)
    grt_mvin(&CTX, A + tt * h, (uint32_t)(tt * M), h, M);   /* FSM 규약: tt 당 M 행 */
  grt_ss_gemv_config(&CTX, K, N, M, sh, 0, bBase, 0);
}

/* E617b: 측정 루프를 **독립 함수**로 — 거대한 main() 의 레지스터 압박에서 분리한다.
 * 발행 루프에 여분의 CPU 일이 들어가면 직접 손해라는 것이 corpus 규칙(E205)이다. */
__attribute__((noinline))
static double blk_loop(int sh, int M, int bBase, int ntb, long T) {
  const uint64_t s0 = rdt();
  for (long i = 0; i < T; i++)
    for (int pp = 0; pp < ntb; pp++) {
      load_pre_blk(sh, bBase, pp);
      grt_ss_gemv_blk(&CTX, C, pp, 1);
    }
  grt_fence();
  return (double)(rdt() - s0) * 100.0 / (double)T;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s M T [--neg]\n", argv[0]); return 2; }
  const int M = atoi(argv[1]);
  const long T = atol(argv[2]);
  const int neg = argc > 3 && !strcmp(argv[3], "--neg");
  if (argc > 4) K = atoi(argv[4]);              /* E612: K·N 을 인자로 */
  if (argc > 5) N = atoi(argv[5]);
#ifndef SPB
  { const char *e = getenv("SP_BANKS"); if (e) SP_BANKS = atoi(e); SP_ENT = 16384 / SP_BANKS; }
#endif
  /* E612: 뱅크 수 > 세그먼트 수이면 B 는 뱅크 0 으로 감기지 않으므로 0 부터 써도 되고
   * 뱅크 전체를 쓸 수 있다 (N=256 에서 bBase=SP_ENT/2 는 용량을 넘긴다). */
  const int bBase = (SP_BANKS > 4) ? 0 : SP_ENT / 2;
  if (M < 1 || M > DIM) { fprintf(stderr, "M은 1..%d\n", DIM); return 2; }
  { const char *e = getenv("PGOFF"); bpre_init(e ? atoi(e) : 0); }
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

  /* ---- ① 정확성: 세 shape 을 한 프로세스에서 번갈아, 매번 검사 ---- */
  const int seq[] = {2, 0, 1, 2, 1, 0};
  int bad_total = 0;
  for (int i = 0; i < 6; i++) {
    memset(C, 0, sizeof C);
    setup_shape(seq[i], M, bBase);
    grt_fence();
    grt_ss_gemv(&CTX, C);
    grt_fence();
    int bad = 0;
    for (int r = 0; r < M; r++)
      for (int n = 0; n < N; n++)
        if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    printf("switch step %d shape=%d bad=%d %s\n", i, seq[i], bad, bad ? "FAIL" : "PASS");
    bad_total += bad;
  }

  /* E614: 블록을 하나만 골라 격리 측정한다 (E613: 문맥이 2.45 배를 바꾼다). */
  const char *onlyv = getenv("ONLY");
  const int only_blk = onlyv != NULL;
  #define RUN(tag) (onlyv == NULL || strstr(onlyv, tag) != NULL)  /* E616: 부분 문자열 — "pre,blk" 로 여러 블록 */
  uint64_t t0 = 0; double base = 0, cfg = 0;
  if (RUN("base")) {
  /* ---- ② 전환 비용: 같은 shape 재실행 vs shape 전환 ---- */
  setup_shape(2, M, bBase); grt_fence();
  t0 = rdt();
  for (long i = 0; i < T; i++) { grt_ss_gemv(&CTX, C); }
  grt_fence();
  base = (double)(rdt() - t0) * 100.0 / (double)T;

  t0 = rdt();                                   /* config 만 바꿔 재설정 (B 는 그대로) */
  for (long i = 0; i < T; i++) {
    grt_config_ex_shape(&CTX, GRT_WS, 2);
    grt_ss_gemv_config(&CTX, K, N, M, 2, 0, bBase, 0);
    grt_ss_gemv(&CTX, C);
  }
  grt_fence();
  cfg = (double)(rdt() - t0) * 100.0 / (double)T;
  }                                             /* RUN("base") 끝 */

  /* E602: 실제 decode 는 토큰마다 가중치를 DRAM 에서 스트리밍한다 (상주 불가).
   * 같은 shape 로 매 스텝 B 를 다시 깔고 GEMV — 이것이 스트리밍 decode 의 한 스텝이다. */
  if (RUN("stream")) for (int sh = 0; sh <= 2; sh++) {
    setup_shape(sh, M, bBase); grt_fence();
    uint64_t s0 = rdt();
    for (long i = 0; i < T; i++) { setup_shape(sh, M, bBase); grt_ss_gemv(&CTX, C); }
    grt_fence();
    printf("stream shape=%d  %.1f cyc/step\n", sh, (double)(rdt() - s0) * 100.0 / (double)T);
  }

  if (RUN("pre")) for (int sh = 0; sh <= 2; sh++) {   /* E603: 재배열된 가중치로 스트리밍 */
    prearrange(sh, bBase);
    load_pre(sh, M, bBase); grt_fence();
    memset(C, 0, sizeof C);
    grt_ss_gemv(&CTX, C); grt_fence();
    int bad = 0;
    for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
      if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    uint64_t s0 = rdt();
    for (long i = 0; i < T; i++) { load_pre(sh, M, bBase); grt_ss_gemv(&CTX, C); }
    grt_fence();
    printf("prearranged shape=%d  %.1f cyc/step  bad=%d %s\n", sh,
           (double)(rdt() - s0) * 100.0 / (double)T, bad, bad ? "FAIL" : "PASS");
    bad_total += bad;
  }

  if (RUN("wide")) for (int sh = 0; sh <= 2; sh++) {  /* E604: wide mvin 판 */
    prearrange_wide(sh);
    load_wide(sh, M, bBase); grt_fence();
    memset(C, 0, sizeof C);
    grt_ss_gemv(&CTX, C); grt_fence();
    int bad = 0;
    for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
      if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    uint64_t s0 = rdt();
    for (long i = 0; i < T; i++) { load_wide(sh, M, bBase); grt_ss_gemv(&CTX, C); }
    grt_fence();
    printf("wide shape=%d  %.1f cyc/step  bad=%d %s\n", sh,
           (double)(rdt() - s0) * 100.0 / (double)T, bad, bad ? "FAIL" : "PASS");
    bad_total += bad;
  }

  /* E606: 적재·계산 **이중 버퍼**. E605 의 합성 모델이 6 % 안에 맞았다는 것은 둘이
   * 겹치지 않는다는 뜻이다. 스크래치패드를 반으로 갈라 다음 스텝의 가중치를 먼저
   * 발행하고 현재 버퍼로 계산하면 RS 가 둘을 겹쳐 준다 (주소가 달라 의존이 없다). */
  if (RUN("dbuf")) for (int sh = 0; sh <= 2; sh++) {
    const int S = 1 << sh, h = DIM >> sh, w = DIM * S;
    const int ks = h < 4 ? 4 : h;
    const int rows = (N / w) * (K / h) * ks;
    const int buf[2] = { 0, rows };                 /* 두 버퍼의 bBase */
    if (2 * rows > SP_ENT) { printf("dbuf shape=%d SKIP (용량)\n", sh); continue; }
    prearrange(sh, 0);
    load_pre(sh, M, buf[0]); grt_fence();
    memset(C, 0, sizeof C);
    grt_ss_gemv_config(&CTX, K, N, M, sh, 0, buf[0], 0);
    grt_ss_gemv(&CTX, C); grt_fence();
    int bad = 0;
    for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
      if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    uint64_t s0 = rdt();
    for (long i = 0; i < T; i++) {
      load_pre(sh, M, buf[(i + 1) & 1]);            /* 다음 버퍼를 먼저 발행 (비동기) */
      grt_ss_gemv_config(&CTX, K, N, M, sh, 0, buf[i & 1], 0);
      grt_ss_gemv(&CTX, C);                         /* 현재 버퍼로 계산 — 겹쳐야 한다 */
    }
    grt_fence();
    printf("dbuf shape=%d  %.1f cyc/step  bad=%d %s\n", sh,
           (double)(rdt() - s0) * 100.0 / (double)T, bad, bad ? "FAIL" : "PASS");
    bad_total += bad;
  }

  /* E607 (결정적 대조): 두 버퍼를 **다른 뱅크**에 둔다. S=2 면 A(뱅크 0) + 버퍼 A(1,2)
   * + 버퍼 B(3,4) 로 8 뱅크 안에 들어간다. 소프트웨어는 E606 과 같고 **뱅크만 다르다** —
   * 여기서 줄면 E606 의 원인이 단일 포트 경합임이 확정된다. */
  if (SP_BANKS >= 5) {
    const int sh = 1, S = 2, h = 8, w = 32, ks = 8;
    const int rows = (N / w) * (K / h) * ks;
    prearrange(sh, 0);
    for (int variant = 0; variant < 2; variant++) {
      /* variant 0: 두 버퍼가 같은 뱅크(행만 다름)  1: 다른 뱅크 */
      const int bankoff[2] = { 1, variant ? 3 : 1 };
      const int rowoff[2]  = { 0, variant ? 0 : rows };
      if (!variant && 2 * rows > SP_ENT) { printf("dbuf2 variant0 SKIP\n"); continue; }
      for (int b = 0; b < 2; b++) {                    /* 두 버퍼를 채운다 */
        grt_config_ld(&CTX, 16);
        for (int s = 0; s < S; s++)
          for (int r0 = 0; r0 < rows; r0 += DIM) {
            const int nb = rows - r0 > DIM ? DIM : rows - r0;
            grt_mvin(&CTX, &Bpre[s][r0 * 16],
                     (uint32_t)(((bankoff[b] + s) % SP_BANKS) * SP_ENT + rowoff[b] + r0), DIM, nb);
          }
      }
      grt_config_ld(&CTX, K);
      for (int tt = 0; tt < K / h; tt++)
        grt_mvin(&CTX, A + tt * h, (uint32_t)(tt * M), h, M);
      grt_fence();
      memset(C, 0, sizeof C);
      grt_config_ex_shape(&CTX, GRT_WS, sh);
      grt_ss_gemv_config(&CTX, K, N, M, sh, 0, bankoff[0] * SP_ENT + rowoff[0], 0);
      grt_ss_gemv(&CTX, C); grt_fence();
      int bad = 0;
      for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
        if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
      uint64_t s0 = rdt();
      for (long i = 0; i < T; i++) {
        const int nxt = (i + 1) & 1, cur = i & 1;
        grt_config_ld(&CTX, 16);                      /* 다음 버퍼 적재 (비동기) */
        for (int s = 0; s < S; s++)
          for (int r0 = 0; r0 < rows; r0 += DIM) {
            const int nb = rows - r0 > DIM ? DIM : rows - r0;
            grt_mvin(&CTX, &Bpre[s][r0 * 16],
                     (uint32_t)(((bankoff[nxt] + s) % SP_BANKS) * SP_ENT + rowoff[nxt] + r0), DIM, nb);
          }
        grt_ss_gemv_config(&CTX, K, N, M, sh, 0, bankoff[cur] * SP_ENT + rowoff[cur], 0);
        grt_ss_gemv(&CTX, C);
      }
      grt_fence();
      printf("dbuf2 %s  %.1f cyc/step  bad=%d %s\n", variant ? "다른뱅크" : "같은뱅크",
             (double)(rdt() - s0) * 100.0 / (double)T, bad, bad ? "FAIL" : "PASS");
      bad_total += bad;
    }
  }

  /* E611: **부분 실행 + 블록별 적재**. FSM 을 pp 블록마다 부르고 그 사이에 다음 블록의
   * 가중치를 적재해 겹친다 (E609 의 4 타일 입도를 FSM 경로로 옮긴 것). 먼저 부분 실행이
   * 통짜와 같은 결과를 내는지 검사하고, 그 다음 스트리밍 시간을 잰다. */
  if (RUN("blkf")) for (int sh = 0; sh <= 2; sh++) {   /* E617b: 독립 함수 판 */
    const int ntb = N / (DIM << sh);
    prearrange(sh, bBase);
    load_pre(sh, M, bBase); grt_fence();
    memset(C, 0, sizeof C);
    for (int pp = 0; pp < ntb; pp++) grt_ss_gemv_blk(&CTX, C, pp, 1);
    grt_fence();
    int bad = 0;
    for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
      if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    double t1 = blk_loop(sh, M, bBase, ntb, T);
    double t2 = blk_loop(sh, M, bBase, ntb, T);
    printf("blkf shape=%d ntb=%d  %.1f / %.1f cyc/step  bad=%d %s\n", sh, ntb, t1, t2,
           bad, bad ? "FAIL" : "PASS");
    bad_total += bad;
  }

  if (RUN("blk")) for (int sh = 0; sh <= 2; sh++) {
    const int S = 1 << sh, w = DIM * S;
    const int ntb = N / w;
    prearrange(sh, bBase);
    load_pre(sh, M, bBase); grt_fence();
    memset(C, 0, sizeof C);                      /* ① 부분 실행의 정확성 */
    for (int pp = 0; pp < ntb; pp++) grt_ss_gemv_blk(&CTX, C, pp, 1);
    grt_fence();
    int bad = 0;
    for (int r = 0; r < M; r++) for (int n = 0; n < N; n++)
      if (C[r * N + n] != (int8_t)ref[r * N + n]) bad++;
    /* E616: 같은 루프를 **세 번** 재고 전부 출력한다. 문맥 효과가 워밍업이라면
     * 2·3 번째가 1 번째보다 크게 빨라지고, 그렇다면 측정 규약은 "워밍업 후 측정"이 된다. */
    double tt3[3];
    for (int rep = 0; rep < 3; rep++) {
      uint64_t s0 = rdt();
      for (long i = 0; i < T; i++)
        for (int pp = 0; pp < ntb; pp++) {
          load_pre_blk(sh, bBase, pp);
          grt_ss_gemv_blk(&CTX, C, pp, 1);
        }
      grt_fence();
      tt3[rep] = (double)(rdt() - s0) * 100.0 / (double)T;
    }
    printf("blk shape=%d ntb=%d  %.1f / %.1f / %.1f cyc/step  bad=%d %s\n", sh, ntb,
           tt3[0], tt3[1], tt3[2], bad, bad ? "FAIL" : "PASS");
    bad_total += bad;
  }

  if (only_blk) return bad_total != 0;
  t0 = rdt();                                   /* 전환: shape 2 <-> 1 을 번갈아 (B 재배치 포함) */
  for (long i = 0; i < T; i++) {
    setup_shape(i & 1 ? 1 : 2, M, bBase);
    grt_ss_gemv(&CTX, C);
  }
  grt_fence();
  const double full = (double)(rdt() - t0) * 100.0 / (double)T;

  printf("M=%d T=%ld  base=%.1f  cfg_only=%.1f (+%.1f)  full_switch=%.1f (+%.1f)  %s\n",
         M, T, base, cfg, cfg - base, full, full - base, bad_total ? "FAIL" : "PASS");
  return bad_total != 0;
}
