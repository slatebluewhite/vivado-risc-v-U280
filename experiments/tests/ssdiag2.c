// SSGemm 단계식 진단 (E558): t2(S=4, H=128) wedge 를 **한 부팅으로** 이분탐색한다.
// 각 단계가 프로그램 안에서 fsync 되므로 wedge 로 죽어도 직전 단계까지 NFS 에 남는다
// (corpus 규약 — 셸 리다이렉트는 프로세스가 죽으면 못 뱉는다).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "include/gemmini_rt.h"

#define DIM 16
#define SP_BANKS 4
#define SP_ENT 4096
#define ACC_ENT 256

static const grt_ctx CTX = { DIM, 1, 4, GRT_OP_INT8 };
static int lfd = -1;
static void slog(const char *msg) {
  char b[128]; int n = snprintf(b, sizeof b, "%s\n", msg);
  write(lfd, b, n); fsync(lfd);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd2.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("start (S=4, H=128)");

  const int sh = 2, H = 128;
  const int S = 4, h = 4, w = 64, ks = 4;
  const int nt = H / w, ch = H / h;              /* 2, 32 */
  const int bBase = 2048;

  static int8_t W[128 * 128], x[128], y[128];
  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);

  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, DIM);
  grt_config_ld(&CTX, H);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, W + (size_t)(tt * h) * H + pp * w + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks), DIM, h);
  grt_fence();
  slog("W-OK (256 mvins)");

  /* E560: W 되읽기 — mvin 이 잘못 썼는지, EC 가 잘못 읽는지의 절단 (ssdiag2 1판의
   * 구멍: 256개 mvin 의 내용 검증이 없었다). mvout-from-sp 로 전 tile 을 되읽는다. */
  {
    static int8_t rb[16];
    int badw = 0, checked = 0;
    grt_config_st(&CTX, DIM);
    for (int pp = 0; pp < nt && badw < 8; pp++)
      for (int tt = 0; tt < ch && badw < 8; tt++)
        for (int s = 0; s < S && badw < 8; s++)
          for (int r = 0; r < h; r++) {
            uint32_t sp = (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks + r);
            grt_mvout(&CTX, rb, sp, DIM, 1);
            grt_fence();
            for (int c2 = 0; c2 < DIM; c2++) {
              checked++;
              if (rb[c2] != W[(size_t)(tt * h + r) * H + pp * w + s * DIM + c2] && badw++ < 8) {
                char b[96]; snprintf(b, sizeof b, "W-RB BAD pp=%d tt=%d s=%d r=%d c=%d got=%d",
                                     pp, tt, s, r, c2, rb[c2]); slog(b);
              }
            }
          }
    char b[64]; snprintf(b, sizeof b, "W-RB: %s (%d checked)", badw ? "FAIL" : "PASS", checked);
    slog(b);
  }

  grt_config_ld(&CTX, h);
  grt_mvin_rows(&CTX, x, 0, h, ch, h);   /* E566 */
  grt_fence();
  slog("x-OK");

  /* E563: 기전 판별 4종.
   *  Q1: T=17 값 로깅 — got 이 T16 부분합인지 (17번째만 소실?)
   *  Q2: 1..16 -> 행 R1, 17번째만 acc=0 으로 행 R2 — 신선한 행이면 착지하는가
   *  Q3: SLOW(각 tile fence, 1-tile 체인 x8) 값 로깅 — E562 모순의 해부
   *  Q4: 12 + fence + 12 (같은 행 이어 누적) — fence 리셋 여부 */
  {
    static int8_t q[64]; char b[200]; int off;
    /* Q1 */
    for (int tt = 0; tt < 17; tt++) {
      grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
                  tt == 0 ? GRT_ACC(50) : GRT_ACC_ACC(50), DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
    }
    grt_config_st(&CTX, DIM);
    grt_mvout(&CTX, q, GRT_ACC(50), DIM, 1); grt_fence();
    off = snprintf(b, sizeof b, "Q1 T=17 y0..7:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
    slog(b);
    /* Q2 */
    for (int tt = 0; tt < 16; tt++) {
      grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
                  tt == 0 ? GRT_ACC(51) : GRT_ACC_ACC(51), DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
    }
    grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + 16 * ks), GRT_ACC(52), DIM, h, DIM, 1);
    grt_compute(&CTX, 16, GRT_GARBAGE, h, 1, 0, 0);
    grt_mvout(&CTX, q, GRT_ACC(52), DIM, 1); grt_fence();
    off = snprintf(b, sizeof b, "Q2 tile17@R2 y0..7:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
    slog(b);
    /* Q3 */
    for (int tt = 0; tt < 8; tt++) {
      grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
                  tt == 0 ? GRT_ACC(53) : GRT_ACC_ACC(53), DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
      grt_fence();
    }
    grt_mvout(&CTX, q, GRT_ACC(53), DIM, 1); grt_fence();
    off = snprintf(b, sizeof b, "Q3 slow8 y0..7:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
    slog(b);
    /* Q4 */
    for (int half = 0; half < 2; half++) {
      for (int i2 = 0; i2 < 12; i2++) {
        int tt = half * 12 + i2;
        grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
                    tt == 0 ? GRT_ACC(54) : GRT_ACC_ACC(54), DIM, h, DIM, 1);
        grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
      }
      grt_fence();
    }
    grt_mvout(&CTX, q, GRT_ACC(54), DIM, 1); grt_fence();
    off = snprintf(b, sizeof b, "Q4 12+f+12 y0..7:");
    for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
    slog(b);
  }

  /* A: preload/compute 만 (mvout 없음) — 64 tile */
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++) {
      uint32_t bsp = 1 * SP_ENT + bBase + (pp * ch + tt) * ks;
      uint32_t csp = (tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp));
      grt_preload(&CTX, bsp, csp, DIM, h, DIM, 1);
      grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
    }
  grt_fence();
  slog("A-OK (64 preload/compute)");

  /* B: mvout 8개 */
  for (int pp = 0; pp < nt; pp++)
    for (int s = 0; s < S; s++)
      grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
  grt_fence();
  slog("B-OK (8 mvouts)");

  /* 정확성 한 번 */
  int bad = 0;
  for (int n = 0; n < H; n++) {
    int32_t sacc = 0;
    for (int k = 0; k < H; k++) sacc += (int32_t)x[k] * W[k * H + n];
    int8_t exp = sacc > 127 ? 127 : sacc < -128 ? -128 : (int8_t)sacc;
    if (y[n] != exp) bad++;
  }
  { char b[64]; snprintf(b, sizeof b, "CHK: bad %d/128", bad); slog(b); }

  /* C~E: timestep 반복 확장 */
  const long stages[3] = { 1, 10, 100 };
  const char *names[3] = { "C-OK (1 more step)", "D-OK (10 steps)", "E-OK (100 steps)" };
  for (int st = 0; st < 3; st++) {
    for (long it = 0; it < stages[st]; it++) {
      grt_config_ld(&CTX, h);
      grt_mvin_rows(&CTX, x, 0, h, ch, h);   /* E566 */
      for (int pp = 0; pp < nt; pp++)
        for (int tt = 0; tt < ch; tt++) {
          uint32_t bsp = 1 * SP_ENT + bBase + (pp * ch + tt) * ks;
          uint32_t csp = (tt == 0 ? GRT_ACC(pp) : GRT_ACC_ACC(pp));
          grt_preload(&CTX, bsp, csp, DIM, h, DIM, 1);
          grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
        }
      for (int pp = 0; pp < nt; pp++)
        for (int s = 0; s < S; s++)
          grt_mvout(&CTX, y + pp * w + s * DIM, GRT_ACC(s * ACC_ENT + pp), DIM, 1);
    }
    grt_fence();
    slog(names[st]);
  }
  slog("ALL-OK");
  return 0;
}
