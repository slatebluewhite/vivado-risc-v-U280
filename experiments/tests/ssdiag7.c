// SSGemm 기전 판별 7탄 (E565c): 교대 패턴의 표본 확대.
// 같은 사이클(16 tile 신선 + own mvout + fence)을 10회 — 죽살이 수열이 parity 인지,
// 주기-3 인지, 데이터/행 의존인지 가른다. 행은 매회 다르게(0..9), chunk 구간은
// 짝수 회차 0-15 / 홀수 회차 16-31 로 번갈아 (기대값이 매회 명확).
// 이어서 [B] 같은 실험을 fence 없이 (mvout 만) 5회 — fence(EC idle/flush)의 역할 분리.
// 이어서 [C] 17-tile 체인 1회 — ssdiag4 P0 재현이 이 부팅에서도 그대로인지 (부팅 간 재현성).
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
  char b[256]; int n = snprintf(b, sizeof b, "%s\n", msg);
  write(lfd, b, n); fsync(lfd);
}

static const int sh = 2, H = 128;
static const int S = 4, h = 4, ks = 4;
static const int bBase = 2048;
static int8_t W[128 * 128], x[128];

static int32_t prange(int t0, int t1, int n) {
  int32_t s = 0;
  for (int k = t0 * h; k < t1 * h; k++) s += (int32_t)x[k] * W[k * H + n];
  return s > 127 ? 127 : s < -128 ? -128 : s;
}
static void tileop(int row, int tt, int fresh) {
  grt_preload(&CTX, (uint32_t)(1 * SP_ENT + bBase + tt * ks),
              fresh ? GRT_ACC(row) : GRT_ACC_ACC(row), DIM, h, DIM, 1);
  grt_compute(&CTX, tt, GRT_GARBAGE, h, 1, 0, 0);
}

int main(void) {
  lfd = open("/mnt2/tmp/ssd7.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lfd < 0) { perror("log"); return 2; }
  if (mlockall(MCL_CURRENT | MCL_FUTURE)) { perror("mlockall"); return 2; }
  grt_flush_ctx(&CTX);
  slog("ssd7 start");

  const int nt = H / (DIM * S), ch = H / h;
  for (int k = 0; k < H; k++) for (int n = 0; n < H; n++) W[k * H + n] = (int8_t)((k + 2 * n) % 7 - 3);
  for (int k = 0; k < H; k++) x[k] = (int8_t)((3 * k) % 5 - 2);

  grt_config_ex_shape(&CTX, GRT_WS, sh);
  grt_config_st(&CTX, DIM);
  grt_config_ld(&CTX, H);
  for (int pp = 0; pp < nt; pp++)
    for (int tt = 0; tt < ch; tt++)
      for (int s = 0; s < S; s++)
        grt_mvin(&CTX, W + (size_t)(tt * h) * H + pp * DIM * S + s * DIM,
                 (uint32_t)(((1 + s) % SP_BANKS) * SP_ENT + bBase + (pp * ch + tt) * ks), DIM, h);
  grt_config_ld(&CTX, h);
  grt_mvin(&CTX, x, 0, h, ch);
  grt_fence();
  slog("fill OK");

  static int8_t q[16];
  char verdict[64]; int vn = 0;

  /* [A] 10회: 16 신선 + own mvout + fence */
  for (int c = 0; c < 10; c++) {
    int row = c; int t0 = (c % 2) * 16;
    for (int i = 0; i < 16; i++) tileop(row, t0 + i, i == 0);
    grt_mvout(&CTX, q, GRT_ACC(row), DIM, 1); grt_fence();
    int ok = 1, zero = 1;
    for (int n = 0; n < 16; n++) {
      if (q[n] != (int8_t)prange(t0, t0 + 16, n)) ok = 0;
      if (q[n] != 0) zero = 0;
    }
    verdict[vn++] = ok ? 'O' : (zero ? 'Z' : '?');
    if (!ok && !zero) {
      char b[160]; int off = snprintf(b, sizeof b, "A%d partial y:", c);
      for (int n = 0; n < 8; n++) off += snprintf(b + off, sizeof b - off, " %d", q[n]);
      slog(b);
    }
  }
  verdict[vn] = 0;
  { char b[96]; snprintf(b, sizeof b, "[A] fence-yes x10: %s", verdict); slog(b); }

  /* [B] 5회: fence 없이 (mvout 만; 마지막에 한 번 fence) */
  vn = 0;
  for (int c = 0; c < 5; c++) {
    int row = 10 + c; int t0 = (c % 2) * 16;
    for (int i = 0; i < 16; i++) tileop(row, t0 + i, i == 0);
    grt_mvout(&CTX, q + 0, GRT_ACC(row), DIM, 1);   /* no fence — q 는 나중에 재사용되므로 값은 아래서 다시 읽는다 */
  }
  grt_fence();
  for (int c = 0; c < 5; c++) {
    int row = 10 + c; int t0 = (c % 2) * 16;
    grt_mvout(&CTX, q, GRT_ACC(row), DIM, 1); grt_fence();
    int ok = 1, zero = 1;
    for (int n = 0; n < 16; n++) {
      if (q[n] != (int8_t)prange(t0, t0 + 16, n)) ok = 0;
      if (q[n] != 0) zero = 0;
    }
    verdict[vn++] = ok ? 'O' : (zero ? 'Z' : '?');
  }
  verdict[vn] = 0;
  { char b[96]; snprintf(b, sizeof b, "[B] fence-no  x5: %s", verdict); slog(b); }

  /* [C] 17-tile 체인 재현 */
  for (int tt = 0; tt < 17; tt++) tileop(20, tt, tt == 0);
  grt_mvout(&CTX, q, GRT_ACC(20), DIM, 1); grt_fence();
  { int p16 = 1, p17 = 1;
    for (int n = 0; n < 16; n++) {
      if (q[n] != (int8_t)prange(0, 16, n)) p16 = 0;
      if (q[n] != (int8_t)prange(0, 17, n)) p17 = 0;
    }
    char b[96]; snprintf(b, sizeof b, "[C] T17: %s", p16 ? "cut@16" : (p17 ? "FULL17" : "other")); slog(b); }
  slog("ssd7 ALL DONE");
  return 0;
}
